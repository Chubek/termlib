# frozen_string_literal: true
module Termak
  class Backend
    NAMES = %w[c qbe].freeze
    attr_reader :name

    def initialize(name = 'c', environment = Environ.new)
      raise Error, "unknown AOT backend: #{name}" unless NAMES.include?(name)
      @name = name
      @driver = environment.command('TERMSCRIPT', 'termscript')
    end

    def compile(source, output, cwd:, static: false)
      command = @driver + ['--backend', @name, '-o', output]
      command << '--static' if static
      System.run(command + [source], cwd: cwd)
    end
  end
end
