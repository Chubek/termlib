# frozen_string_literal: true
require 'shellwords'

module Termak
  class Environ
    def initialize(values = ENV)
      @values = values.to_h.freeze
    end

    def command(name, default)
      # Split a compiler command such as `ccache cc`, without shell evaluation.
      words = Shellwords.split(@values.fetch(name, default))
      raise Error, "#{name} cannot be empty" if words.empty?
      words
    rescue ArgumentError => e
      raise Error, "#{name}: #{e.message}"
    end

    def flags(name)
      Shellwords.split(@values.fetch(name, ''))
    rescue ArgumentError => e
      raise Error, "#{name}: #{e.message}"
    end

    def [](name)
      @values[name]
    end
  end
end
