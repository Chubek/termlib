# frozen_string_literal: true
require 'digest'
require 'json'

module Termak
  class Toolchain
    def initialize(environment = Environ.new)
      @env = environment
    end

    def build(target, root, build_dir, dependencies, cxx_link: false)
      output = System.inside(build_dir, target.output_name)
      FileUtils.mkdir_p(File.dirname(output))
      sources = target.sources.map { |s| System.inside(root, s) }
      sources.each { |s| raise Error, "missing source: #{s}" unless File.file?(s) }
      raise Error, "target #{target.name} has no sources" if sources.empty?
      if sources.any? { |s| File.extname(s) == '.tsc' }
        raise Error, 'Termscript executables require exactly one .tsc source' unless sources.size == 1 && target.kind == :executable
        raise Error, 'native dependencies for Termscript targets are not supported yet' unless dependencies.empty?
        System.atomic_output(output) do |temporary|
          Backend.new(target.backend, @env).compile(sources.first, temporary, cwd: root, static: target.static)
          File.chmod(0o777 & ~File.umask, temporary)
        end
        return output
      end
      objects = sources.map do |source|
        cxx = %w[.cpp .cc .cxx].include?(File.extname(source))
        raise Error, "unsupported source: #{source}" unless cxx || File.extname(source) == '.c'
        object = System.inside(build_dir, "obj/#{target.name}/#{Digest::SHA256.hexdigest(source)[0, 20]}.o")
        compiler = @env.command(cxx ? 'CXX' : 'CC', cxx ? 'c++' : 'cc')
        flags = @env.flags('CPPFLAGS') + @env.flags(cxx ? 'CXXFLAGS' : 'CFLAGS')
        flags += [cxx ? '-std=c++20' : '-std=c11', '-fPIC'] + target.flags
        flags += target.includes.map { |p| '-I' + File.expand_path(p, root) }
        command = compiler + flags + ['-c', source, '-o', object]
        inputs = [source] + Dir.glob(File.join(root, '**', '*.{h,hpp}')).sort
        signature = Digest::SHA256.hexdigest(JSON.generate([command, inputs]))
        stamp = object + '.command'
        # Any header change in the project invalidates all objects in that project.
        fresh = File.file?(object) && File.file?(stamp) && File.read(stamp) == signature &&
                inputs.all? { |p| File.mtime(p) <= File.mtime(object) }
        unless fresh
          FileUtils.mkdir_p(File.dirname(object))
          System.atomic_output(object) do |temporary|
            System.run(compiler + flags + ['-c', source, '-o', temporary], cwd: root)
          end
          System.atomic_write(stamp, signature)
        end
        object
      end
      if target.kind == :library
        # Recreate archives so removed sources cannot leave stale members behind.
        temp = output + ".tmp-#{Process.pid}"
        begin
          System.run(@env.command('AR', 'ar') + ['rcs', temp] + objects, cwd: root)
          File.rename(temp, output)
        ensure
          File.unlink(temp) if File.exist?(temp)
        end
      else
        cxx = cxx_link || sources.any? { |s| %w[.cpp .cc .cxx].include?(File.extname(s)) }
        command = @env.command(cxx ? 'CXX' : 'CC', cxx ? 'c++' : 'cc')
        System.atomic_output(output) do |temporary|
          args = command + ['-o', temporary] + objects + dependencies.reverse + target.link_flags + @env.flags('LDFLAGS')
          args << '-static' if target.static
          System.run(args, cwd: root)
          File.chmod(0o777 & ~File.umask, temporary)
        end
      end
      output
    end
  end
end
