# frozen_string_literal: true
require 'open3'
require 'fileutils'
require 'tempfile'

module Termak
  class Error < StandardError; end

  module System
    module_function

    # Argument vectors only: filenames and compiler flags never become shell text.
    def run(argv, cwd: Dir.pwd, env: {}, verbose: true)
      raise Error, 'empty command' if argv.empty?
      warn argv.inspect if verbose
      command = argv.map(&:to_s)
      # The [executable, argv0] form prevents Ruby's single-string shell mode,
      # including for a zero-argument executable whose filename has spaces.
      out, err, status = Open3.capture3(env, [command.first, command.first], *command.drop(1), chdir: cwd)
      $stdout.write(out)
      $stderr.write(err)
      raise Error, "#{argv.first} failed (#{status.exitstatus || status.termsig})" unless status.success?
      out
    rescue SystemCallError => e
      raise Error, e.message
    end

    def atomic_write(path, bytes)
      FileUtils.mkdir_p(File.dirname(path))
      Tempfile.create(['.termak-', '.tmp'], File.dirname(path)) do |temp|
        temp.binmode
        temp.write(bytes)
        temp.close
        File.rename(temp.path, path)
      end
    end

    # Publish compiler/linker output only after a successful command. The old
    # artifact remains intact on failure; temporary outputs are always removed.
    def atomic_output(path)
      FileUtils.mkdir_p(File.dirname(path))
      Tempfile.create(['.termak-output-', '.tmp'], File.dirname(path)) do |temp|
        temp.close
        yield temp.path
        File.rename(temp.path, path)
      end
    end

    def inside(root, relative)
      root = File.expand_path(root)
      path = File.expand_path(relative, root)
      raise Error, "path escapes root: #{relative}" unless path.start_with?(root + File::SEPARATOR)
      path
    end

    # Sources must remain inside the manifest root after resolving symlinks too.
    # This is validation, not a sandbox against concurrent filesystem mutation.
    def source(root, relative)
      path = inside(root, relative)
      raise Error, "missing source: #{path}" unless File.file?(path)
      resolved = File.realpath(path)
      base = File.realpath(root)
      raise Error, "source escapes root through symlink: #{relative}" unless resolved.start_with?(base + File::SEPARATOR)
      path
    end
  end
end
