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
    end

    def output_name
      kind == :library ? "lib#{name}.a" : name
    end
  end

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

    def build(names = targets.keys, directory: 'build', environment: Environ.new)
      sorted = order(names) # Validate the entire graph before executing anything.
      build_dir = File.expand_path(directory, root)
      raise Error, 'build directory cannot be the source root' if build_dir == root
      outputs = {}
      toolchain = Toolchain.new(environment)
      Array(@hooks[:before_build]).each { |hook| hook.call(self) }
      sorted.each do |target|
        dependency_targets = order(target.dependencies)
        dependencies = dependency_targets.map { |d| outputs.fetch(d.name) }
        cxx = ([target] + dependency_targets).any? do |dependency|
          dependency.sources.any? { |source| %w[.cpp .cc .cxx].include?(File.extname(source)) }
        end
        outputs[target.name] = toolchain.build(target, root, build_dir, dependencies, cxx_link: cxx)
      end
      Array(@hooks[:after_build]).each { |hook| hook.call(self) }
      outputs
    end

    def install(outputs, prefix:, destdir: nil)
      installer = Install.new(prefix: prefix, destdir: destdir)
      outputs.each do |name, path|
        executable = targets.fetch(name).kind == :executable
        installer.copy(path, "#{executable ? 'bin' : 'lib'}/#{File.basename(path)}", executable: executable)
      end
      @files.each { |source, destination| installer.copy(source, destination) }
    end
  end

  def self.main(args)
    options = { file: 'Termakfile', directory: 'build', prefix: '/usr/local', destdir: ENV['DESTDIR'] }
    parser = OptionParser.new do |o|
      o.banner = 'Usage: termak [options] [build|check|install|list] [TARGET ...]'
      o.on('-f FILE') { |v| options[:file] = v }
      o.on('-B DIR') { |v| options[:directory] = v }
      o.on('--prefix PATH') { |v| options[:prefix] = v }
      o.on('--destdir PATH') { |v| options[:destdir] = v }
      o.on('-h', '--help') { puts o; return 0 }
    end
    parser.parse!(args)
    command = args.shift || 'build'
    raise Error, "unknown command: #{command}" unless %w[build check install list].include?(command)
    project = Project.new(options[:file])
    names = args.empty? ? project.targets.keys : args
    project.order(names)
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
