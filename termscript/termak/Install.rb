# frozen_string_literal: true
module Termak
  class Install
    def initialize(prefix: '/usr/local', destdir: nil)
      raise Error, 'install prefix must be absolute' unless prefix.start_with?('/')
      @root = destdir ? File.join(File.expand_path(destdir), prefix.sub(%r{\A/+}, '')) : prefix
    end

    def copy(source, relative, executable: false)
      destination = System.inside(@root, relative)
      # Refuse symlink ancestors rather than writing outside DESTDIR through one.
      parent = File.dirname(destination)
      cursor = parent
      until cursor == File.dirname(cursor)
        raise Error, "symlink in install path: #{cursor}" if File.symlink?(cursor)
        cursor = File.dirname(cursor)
      end
      raise Error, "symlink destination: #{destination}" if File.symlink?(destination)
      FileUtils.mkdir_p(parent)
      System.atomic_write(destination, File.binread(source))
      File.chmod(executable ? 0o755 : 0o644, destination)
      destination
    end
  end
end
