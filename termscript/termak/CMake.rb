# frozen_string_literal: true

module Termak
  # Explicit integration for trusted external CMake projects. Native Termak
  # targets still use our compiler/cache/atomic-publish implementation.
  class CMakeTarget < Target
    attr_reader :source_dir, :cmake_target, :artifact, :options, :component, :cache_name

    def initialize(name, source_dir:, target:, artifact:, options: [], component: nil, cache: nil, cxx: false, kind: :library)
      raise Error, 'CMake target kind must be library or executable' unless %i[library executable].include?(kind)
      super(name, kind, sources: [])
      @source_dir, @cmake_target, @artifact = source_dir, target, artifact
      @options, @component, @cache_name, @cxx = Array(options), component, (cache || name).to_s, cxx
      [source_dir, target, artifact].each do |value|
        raise Error, 'CMake paths/targets must be nonempty strings without NUL' unless value.is_a?(String) && !value.empty? && !value.include?("\0")
      end
      valid_name = /\A[A-Za-z_][A-Za-z0-9_.+-]*\z/
      raise Error, 'invalid CMake target/cache name' unless valid_name.match?(target) && valid_name.match?(@cache_name)
      raise Error, 'invalid CMake component' unless component.nil? || (component.is_a?(String) && valid_name.match?(component))
      parts = artifact.split('/', -1)
      raise Error, 'CMake artifact must be a normalized relative file path' if artifact.start_with?('/') || parts.any? { |p| p.empty? || %w[. ..].include?(p) }
      raise Error, 'CMake options must be explicit -DNAME[:TYPE]=VALUE definitions' unless @options.all? do |value|
        value.is_a?(String) && !value.include?("\0") && /\A-D[A-Za-z_][A-Za-z0-9_]*(?::[A-Za-z]+)?=/.match?(value)
      end
      raise Error, 'cxx must be boolean' unless [true, false].include?(cxx)
    end

    def needs_cxx?
      @cxx
    end

    def validate(root)
      source = File.expand_path(source_dir, root)
      file = File.join(source, 'CMakeLists.txt')
      raise Error, "missing/empty CMake project: #{file}" unless File.file?(file) && File.size?(file)
      source
    end

    def build_cmake(root, build_dir, environment)
      source = validate(root)
      directory = System.inside(build_dir, "cmake/#{cache_name}")
      command = environment.command('CMAKE', 'cmake')
      System.run(command + ['-S', source, '-B', directory] + options, cwd: root, env: environment.to_h)
      System.run(command + ['--build', directory, '--target', cmake_target], cwd: root, env: environment.to_h)
      System.source(directory, artifact)
    end

    def install_cmake(output, root:, prefix:, destdir:, environment: Environ.new)
      # CMakeLists/install scripts are trusted executable build configuration.
      # Guard the stage/prefix itself; delegated subdirectory writes follow
      # CMake's installer semantics, not the native copy() symlink policy.
      Install.new(prefix: prefix, destdir: destdir).check_root
      directory = File.expand_path(output)
      artifact.split('/').size.times { directory = File.dirname(directory) }
      raise Error, 'missing CMake build cache for installation' unless File.file?(File.join(directory, 'CMakeCache.txt'))
      command = environment.command('CMAKE', 'cmake')
      env = environment.to_h.merge('DESTDIR' => destdir ? File.expand_path(destdir) : nil)
      # Reconfigure/rebuild so embedded paths and pkg-config metadata agree with
      # --prefix. A full install builds all normal targets, not excluded modules.
      System.run(command + ['-S', validate(root), '-B', directory, "-DCMAKE_INSTALL_PREFIX=#{prefix}"], env: env)
      build = command + ['--build', directory]
      build += ['--target', cmake_target] if component
      System.run(build, env: env)
      install = command + ['--install', directory, '--prefix', prefix]
      install += ['--component', component] if component
      System.run(install, env: env)
    end
  end
end
