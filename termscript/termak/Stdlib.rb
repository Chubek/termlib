# frozen_string_literal: true

module Termak
  # Source-checkout recipes share the authoritative CMake dependency/feature
  # configuration. Per-module archives contain real adapter objects; they link
  # against the full stdlib/runtime, not an invented self-contained module ABI.
  module StdlibRecipes
    ROOT = File.expand_path('../..', __dir__).freeze
    OPTIONS = %w[-DBUILD_TESTING=OFF -DTERMSCRIPT_NATIVE=OFF -DTERMSCRIPT_JITERATI=OFF
                 -DDOMWEAVE_BUILD_TERMSCRIPT_STDLIB=ON -DFETCHCONTENT_FULLY_DISCONNECTED=ON].freeze

    def termscript_module(name)
      name = name.to_s
      cmake = File.read(File.join(ROOT, 'termscript/stdlib/CMakeLists.txt'))
      declaration = cmake.match(/set\(TS_STDLIB_MODULES\s+(.*?)\)/m)
      raise Error, 'cannot find the authoritative standard library module list' unless declaration
      modules = declaration[1].split
      unless (modules + %w[font_discovery stylesheet common]).include?(name)
        raise Error, "unknown or unimplemented standard library module: #{name}"
      end
      cmake_project "termscript_std_#{name}", source_dir: ROOT,
                    target: "termscript_std_#{name}",
                    artifact: "termscript/stdlib/libtermscript_std_#{name}.a",
                    options: OPTIONS, component: "termscript-module-#{name}",
                    cache: 'termscript', cxx: true
    end

    def termscript_stdlib
      cmake_project :termscript_stdlib, source_dir: ROOT, target: 'termscript_stdlib',
                    artifact: 'termscript/stdlib/libtermscript_stdlib.a',
                    options: OPTIONS, cache: 'termscript', cxx: true
    end
  end
  Project.include(StdlibRecipes)
end
