# frozen_string_literal: true
require 'net/http'
require 'uri'
require 'openssl'
require 'digest'

module Termak
  module Web
    module_function

    # Explicit downloads only. No credential logging, redirects or insecure TLS.
    def fetch(url, destination, sha256:, max_bytes: 64 * 1024 * 1024)
      uri = URI(url)
      raise Error, 'package URLs must use HTTPS without credentials' unless uri.is_a?(URI::HTTPS) && !uri.userinfo && uri.host
      raise Error, 'a pinned SHA-256 is required' unless /\A[0-9a-f]{64}\z/.match?(sha256)
      bytes = +''.b
      Net::HTTP.start(uri.host, uri.port, use_ssl: true,
                      verify_mode: OpenSSL::SSL::VERIFY_PEER,
                      open_timeout: 10, read_timeout: 30) do |http|
        http.request(Net::HTTP::Get.new(uri.request_uri)) do |response|
          raise Error, "package HTTP status #{response.code}" unless response.is_a?(Net::HTTPSuccess)
          response.read_body do |chunk|
            raise Error, 'package size limit exceeded' if bytes.bytesize + chunk.bytesize > max_bytes
            bytes << chunk
          end
        end
      end
      raise Error, 'package SHA-256 mismatch' unless Digest::SHA256.hexdigest(bytes) == sha256
      System.atomic_write(destination, bytes)
      destination
    rescue URI::InvalidURIError, IOError, SystemCallError, Timeout::Error, OpenSSL::SSL::SSLError => e
      raise Error, "package download failed: #{e.class}"
    end
  end
end
