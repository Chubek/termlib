# frozen_string_literal: true
module Termak
  module Plugin
    module_function

    # Plugins, like Termakfiles, are trusted Ruby code, not sandboxed packages.
    def load(project, path)
      absolute = System.inside(project.root, path)
      raise Error, "missing plugin: #{path}" unless File.file?(absolute)
      project.instance_eval(File.read(absolute), absolute, 1)
    end
  end
end
