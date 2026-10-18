# frozen_string_literal: true
require 'optparse'
require_relative 'System'
require_relative 'Environ'
require_relative 'Backend'
require_relative 'Toolchain'
require_relative 'Install'
require_relative 'Plugin'
require_relative 'PackageRepository'

module Termak
  class Target
    attr_reader :name, :kind, :sources, :dependencies, :includes, :flags, :link_flags
    attr_accessor :backend, :static

    def initialize(name, kind, sources:, depends: [], include_dirs: [], cflags: [], ldflags: [], backend: 'c', static: false)
      raise Error, "invalid target name: #{name}" unless /\A[A-Za-z_][A-Za-z0-9_-]*\z/.match?(name.to_s)
      @name, @kind = name.to_s, kind
      @sources, @dependencies, @includes = Array(sources), Array(depends).map(&:to_s), Array(include_dirs)
      @flags, @link_flags, @backend, @static = Array(cflags), Array(ldflags), backend, static
      [@sources, @includes, @flags, @link_flags].each do |values|
        raise Error, 'target paths and flags must be strings without NUL' unless values.all? { |v| v.is_a?(String) && !v.include?("\0") }
      end
      raise Error, 'static must be boolean' unless [true, false].include?(@static)
    end

    def output_name
      kind == :library ? "lib#{name}.a" : name
    end

    def needs_cxx?
      sources.any? { |source| %w[.cpp .cc .cxx].include?(File.extname(source)) }
    end
  end

  require_relative 'CMake'

  class Project
    attr_reader :root, :targets

    def initialize(file)
      @root = File.dirname(File.expand_path(file))
      @targets, @hooks, @files = {}, {}, []
      # Deliberately trusted DSL. Never evaluate a downloaded manifest implicitly.
      instance_eval(File.read(file), File.expand_path(file), 1)
    end

    def library(name, **options)
      add(Target.new(name, :library, **options))
    end

    def executable(name, **options)
      add(Target.new(name, :executable, **options))
    end

    def cmake_project(name, **options)
      add(CMakeTarget.new(name, **options))
    end

    def add(target)
      raise Error, "duplicate target: #{target.name}" if targets.key?(target.name)
      targets[target.name] = target
    end

    def plugin(path)
      Plugin.load(self, path)
    end

    def on(event, &block)
      raise Error, 'hooks require a block' unless block
      raise Error, "unknown hook: #{event}" unless %i[before_build after_build].include?(event)
      (@hooks[event] ||= []) << block
    end

    def install_file(source, destination)
      System.inside('/__termak_install_root', destination)
      @files << [System.inside(root, source), destination]
    end

    def order(names)
      done, visiting, result = {}, {}, []
      visit = lambda do |name|
        raise Error, "dependency cycle at #{name}" if visiting[name]
        return if done[name]
        target = targets[name] or raise Error, "unknown target: #{name}"
        visiting[name] = true
        target.dependencies.each { |d| visit.call(d) }
        visiting.delete(name)
        done[name] = true
        result << target
      end
      names.each { |name| visit.call(name) }
      result
    end

    # Validate every selected input before hooks, compilation or downloads run.
    def check(names = targets.keys)
      order(names).each do |target|
        if target.is_a?(CMakeTarget)
          target.validate(root)
          next
        end
        raise Error, "target #{target.name} has no sources" if target.sources.empty?
        sources = target.sources.map { |source| System.source(root, source) }
        if sources.any? { |source| File.extname(source) == '.tsc' }
          unless sources.size == 1 && target.kind == :executable
            raise Error, 'Termscript executables require exactly one .tsc source'
          end
          raise Error, 'native dependencies for Termscript targets are not supported yet' unless target.dependencies.empty?
          raise Error, "unknown AOT backend: #{target.backend}" unless Backend::NAMES.include?(target.backend)
        else
          sources.each do |source|
            raise Error, "unsupported source: #{source}" unless %w[.c .cpp .cc .cxx].include?(File.extname(source))
          end
        end
      end
      @files.each { |source, _destination| System.source(root, source) }
      true
    end

    def build(names = targets.keys, directory: 'build', environment: Environ.new)
      check(names)
      sorted = order(names)
      build_dir = File.expand_path(directory, root)
      raise Error, 'build directory cannot be the source root' if build_dir == root
      outputs = {}
      toolchain = Toolchain.new(environment)
      Array(@hooks[:before_build]).each { |hook| hook.call(self) }
      sorted.each do |target|
        dependency_targets = order(target.dependencies)
        dependencies = dependency_targets.map { |d| outputs.fetch(d.name) }
        cxx = ([target] + dependency_targets).any?(&:needs_cxx?)
        outputs[target.name] = toolchain.build(target, root, build_dir, dependencies, cxx_link: cxx)
      end
      Array(@hooks[:after_build]).each { |hook| hook.call(self) }
      outputs
    end

    def install(outputs, prefix:, destdir: nil, environment: Environ.new)
      installer = Install.new(prefix: prefix, destdir: destdir)
      outputs.each do |name, path|
        if targets.fetch(name).is_a?(CMakeTarget)
          targets.fetch(name).install_cmake(path, root: root, prefix: prefix, destdir: destdir, environment: environment)
          next
        end
        executable = targets.fetch(name).kind == :executable
        installer.copy(path, "#{executable ? 'bin' : 'lib'}/#{File.basename(path)}", executable: executable)
      end
      @files.each { |source, destination| installer.copy(source, destination) }
    end
  end

  def self.main(args)
    options = { file: 'Termakfile', directory: 'build', prefix: '/usr/local', destdir: ENV['DESTDIR'],
                cache: '.termak/cache' }
    parser = OptionParser.new do |o|
      o.banner = "Usage: termak [options] [build|check|install|list] [TARGET ...]\n       termak --index FILE [--cache DIR] fetch NAME VERSION"
      o.on('-f FILE') { |v| options[:file] = v }
      o.on('-B DIR') { |v| options[:directory] = v }
      o.on('--prefix PATH') { |v| options[:prefix] = v }
      o.on('--destdir PATH') { |v| options[:destdir] = v }
      o.on('--index FILE', 'Explicit pinned package index (JSON)') { |v| options[:index] = v }
      o.on('--cache DIR', 'Verified package archive cache') { |v| options[:cache] = v }
      o.on('-h', '--help') { puts o; return 0 }
    end
    parser.parse!(args)
    command = args.shift || 'build'
    if command == 'fetch'
      raise Error, 'fetch requires --index FILE and NAME VERSION' unless options[:index] && args.size == 2
      puts PackageRepository.new(options[:index]).fetch(*args, cache: options[:cache])
      return 0
    end
    raise Error, "unknown command: #{command}" unless %w[build check install list].include?(command)
    project = Project.new(options[:file])
    names = args.empty? ? project.targets.keys : args
    command == 'list' ? project.order(names) : project.check(names)
    case command
    when 'list' then project.targets.each_value { |t| puts "#{t.name}\t#{t.kind}" }
    when 'check' then puts 'Termakfile OK'
    else
      outputs = project.build(names, directory: options[:directory])
      project.install(outputs, prefix: options[:prefix], destdir: options[:destdir]) if command == 'install'
    end
    0
  rescue Error, OptionParser::ParseError, SystemCallError, SyntaxError => e
    warn "termak: #{e.message}"
    1
  end
end

exit Termak.main(ARGV) if $PROGRAM_NAME == __FILE__
