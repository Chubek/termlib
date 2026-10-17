# frozen_string_literal: true
require 'json'
require_relative 'Web'

module Termak
  class PackageRepository
    def initialize(index_path)
      @entries = JSON.parse(File.read(index_path))
      raise Error, 'package index must be an object' unless @entries.is_a?(Hash)
    rescue JSON::ParserError => e
      raise Error, "invalid package index: #{e.message}"
    end

    # Stores verified archives; extraction and execution are never implicit.
    def fetch(name, version, cache:)
      raise Error, 'invalid package name/version' unless [name, version].all? { |v| /\A[A-Za-z0-9_-][A-Za-z0-9_.-]*\z/.match?(v) }
      versions = @entries.fetch(name, {})
      raise Error, 'package versions must be an object' unless versions.is_a?(Hash)
      entry = versions.fetch(version) { raise Error, "unknown package #{name}@#{version}" }
      raise Error, 'package entry must be an object' unless entry.is_a?(Hash)
      hash = entry.fetch('sha256')
      raise Error, 'invalid package checksum' unless /\A[0-9a-f]{64}\z/.match?(hash)
      path = System.inside(cache, "#{name}-#{version}-#{hash}.archive")
      return path if File.file?(path) && Digest::SHA256.file(path).hexdigest == hash
      Web.fetch(entry.fetch('url'), path, sha256: hash)
    rescue KeyError, TypeError => e
      raise Error, "invalid package index entry: #{e.message}"
    end
  end
end
