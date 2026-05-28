#pragma once
#include <yuumi/types.hpp>
#include <asio.hpp>
#include <algorithm>
#include <filesystem>
#include <system_error>
#include <string>
#include <string_view>

namespace yuumi {
    constexpr std::string_view SOCKET_EXTENSION = ".sock";
    constexpr std::size_t MAX_PIPE_NAME_LENGTH = 64;

    inline std::string resolve_transport_address(const std::string& pipe_name) {
        const std::size_t safe_name_length = std::min(pipe_name.size(), MAX_PIPE_NAME_LENGTH);
        const std::string safe_name = pipe_name.substr(0, safe_name_length);
        return (std::filesystem::temp_directory_path() / (safe_name + std::string(SOCKET_EXTENSION))).string();
    }

    class Transport {
    public:
        using SocketType = asio::local::stream_protocol::socket;

        explicit Transport(asio::io_context& ctx) : _io_ctx(ctx), _socket(ctx) {}

        Result<> listen(const std::string& pipe_name) {
            std::string socket_path = resolve_transport_address(pipe_name);
            std::error_code fs_ec;
            std::filesystem::remove(socket_path, fs_ec);
            if (fs_ec && fs_ec != std::errc::no_such_file_or_directory) {
                return std::unexpected(Error::ConnectionFailed);
            }

            asio::error_code ec;
            asio::local::stream_protocol::endpoint endpoint(socket_path);
            asio::local::stream_protocol::acceptor acceptor(_io_ctx);
            acceptor.open(endpoint.protocol(), ec);
            if (ec) return std::unexpected(Error::ConnectionFailed);
            acceptor.bind(endpoint, ec);
            if (ec) return std::unexpected(Error::ConnectionFailed);
            acceptor.listen(asio::socket_base::max_listen_connections, ec);
            if (ec) return std::unexpected(Error::ConnectionFailed);
            acceptor.accept(_socket, ec);
            if (ec) return std::unexpected(Error::ConnectionFailed);
            return {};
        }

        SocketType& socket() { return _socket; }

    private:
        asio::io_context& _io_ctx;
        SocketType _socket;
    };
}

/*
 * transport.hpp: Local IPC transport primitives for the C++ bridge.
 * - Normalizes transport addresses from pipe names using the system temp directory.
 * - Listens on a local stream socket and accepts a single client connection.
 * - Exposes the accepted socket for protocol read/write operations.
 */
