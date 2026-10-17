# frozen_string_literal: true
require 'tmpdir'
require_relative '../termscript/termak/Termakfile'

class TermakTest
  def assert(value)
    raise 'assertion failed' unless value
  end

  def assert_equal(expected, actual)
    raise "expected #{expected.inspect}, got #{actual.inspect}" unless expected == actual
  end

  def assert_raises(type)
    begin
      yield
    rescue type
      return
    end
    raise "expected #{type}"
  end
  def setup
    @dir = Dir.mktmpdir('termak-', '/tmp/opencode')
  end

  def teardown
    FileUtils.remove_entry(@dir)
  end

  def write(name, contents)
    path = File.join(@dir, name)
    File.write(path, contents)
    path
  end

  def project(text)
    Termak::Project.new(write('Termakfile', text))
  end

  def test_compile_link_and_staged_install
    write("value with 'quote.c", 'int answer(void) { return 42; }')
    write('main.c', 'int answer(void); int main(void) { return answer() != 42; }')
    p = project(<<~RUBY)
      library :value, sources: ["value with 'quote.c"]
      executable :app, sources: ['main.c'], depends: [:value]
      install_file 'main.c', 'share/fixture/main.c'
    RUBY
    assert_equal %w[value app], p.order(['app']).map(&:name)
    outputs = p.build(['app'], directory: "build with 'quote")
    Termak::System.run([outputs.fetch('app')], verbose: false)
    p.install(outputs, prefix: '/usr/local', destdir: File.join(@dir, 'stage'))
    assert File.executable?(File.join(@dir, 'stage/usr/local/bin/app'))
    assert File.file?(File.join(@dir, 'stage/usr/local/lib/libvalue.a'))
    assert File.file?(File.join(@dir, 'stage/usr/local/share/fixture/main.c'))
  end

  def test_validation_before_execution
    p = project("library :a, sources: ['missing.c'], depends: [:b]\nlibrary :b, sources: ['missing.c'], depends: [:a]\n")
    assert_raises(Termak::Error) { p.build }
    assert_raises(Termak::Error) { p.order(['unknown']) }
    assert_raises(Termak::Error) { project("library :a, sources: []\nlibrary :a, sources: []") }
    assert_raises(Termak::Error) { Termak::Backend.new('unknown') }
  end

  def test_transitive_cpp_dependency_selects_cpp_linker
    write('value.cpp', '#include <string>' + "\n" + 'extern "C" int answer(void) { return (int)std::string(42, \'x\').size(); }')
    write('bridge.c', 'int answer(void); int bridge(void) { return answer(); }')
    write('main.c', 'int bridge(void); int main(void) { return bridge() != 42; }')
    p = project(<<~RUBY)
      library :value, sources: ['value.cpp']
      library :bridge, sources: ['bridge.c'], depends: [:value]
      executable :app, sources: ['main.c'], depends: [:bridge]
    RUBY
    outputs = p.build(['app'])
    Termak::System.run([outputs.fetch('app')], verbose: false)
  end

  def test_failed_compile_preserves_object_and_executable
    write('main.c', 'int main(void) { return 0; }')
    p = project("executable :app, sources: ['main.c']")
    outputs = p.build
    object = Dir[File.join(@dir, 'build/obj/app/*.o')].fetch(0)
    before = File.binread(object)
    executable = File.binread(outputs.fetch('app'))
    fake = write('bad-compiler.rb', <<~RUBY)
      File.binwrite(ARGV.fetch(ARGV.index('-o') + 1), 'broken object')
      exit 1
    RUBY
    env = Termak::Environ.new('CC' => "ruby #{Shellwords.escape(fake)}")
    assert_raises(Termak::Error) { p.build(environment: env) }
    assert_equal before, File.binread(object)
    assert_equal executable, File.binread(outputs.fetch('app'))
    assert_equal [], Dir[File.join(@dir, 'build/**/.termak-output-*')]
    p.build
    Termak::System.run([outputs.fetch('app')], verbose: false)
  end

  def test_failed_link_preserves_executable
    write('main.c', 'int main(void) { return 0; }')
    p = project("executable :app, sources: ['main.c']")
    outputs = p.build
    before = File.binread(outputs.fetch('app'))
    fake = write('bad-linker.rb', <<~RUBY)
      exec('cc', *ARGV) if ARGV.include?('-c')
      File.binwrite(ARGV.fetch(ARGV.index('-o') + 1), 'broken executable')
      exit 1
    RUBY
    env = Termak::Environ.new('CC' => "ruby #{Shellwords.escape(fake)}")
    assert_raises(Termak::Error) { p.build(environment: env) }
    assert_equal before, File.binread(outputs.fetch('app'))
    Termak::System.run([outputs.fetch('app')], verbose: false)
  end

  def test_termscript_native_dependencies_are_not_silently_ignored
    write('main.tsc', 'print(42)')
    target = Termak::Target.new('app', :executable, sources: ['main.tsc'])
    assert_raises(Termak::Error) do
      Termak::Toolchain.new.build(target, @dir, File.join(@dir, 'build'), ['dependency.a'])
    end
    assert !File.exist?(File.join(@dir, 'build/app'))
  end

  def test_path_and_install_guards
    assert_raises(Termak::Error) { Termak::System.inside(@dir, '../escape') }
    source = write('data', 'hello')
    stage = File.join(@dir, 'stage')
    FileUtils.mkdir_p(stage)
    File.symlink(@dir, File.join(stage, 'link'))
    installer = Termak::Install.new(prefix: stage)
    assert_raises(Termak::Error) { installer.copy(source, 'link/out') }
    assert_raises(Termak::Error) { installer.copy(source, '../../out') }
  end

  def test_verified_offline_cache_and_download_policy
    bytes = 'fixture'
    hash = Digest::SHA256.hexdigest(bytes)
    index = write('index.json', JSON.generate('fixture' => {'1' => {'url' => 'https://invalid.example/archive', 'sha256' => hash}}))
    cache = File.join(@dir, 'cache')
    FileUtils.mkdir_p(cache)
    path = File.join(cache, "fixture-1-#{hash}.archive")
    File.write(path, bytes)
    assert_equal path, Termak::PackageRepository.new(index).fetch('fixture', '1', cache: cache)
    assert_raises(Termak::Error) { Termak::Web.fetch('http://example.invalid/', path, sha256: hash) }
    assert_raises(Termak::Error) { Termak::Web.fetch('https://user:pass@example.invalid/', path, sha256: hash) }
  end

  def test_trusted_plugins_and_hooks
    write('plugin.rb', "on(:before_build) { |p| File.write(File.join(p.root, 'called'), 'yes') }")
    p = project("plugin 'plugin.rb'")
    assert_equal({}, p.build)
    assert_equal 'yes', File.read(File.join(@dir, 'called'))
  end
end

tests = TermakTest.instance_methods(false).grep(/^test_/).sort
tests.each do |method|
  test = TermakTest.new
  test.setup
  begin
    test.public_send(method)
  ensure
    test.teardown
  end
end
puts "#{tests.length} Termak tests passed"
