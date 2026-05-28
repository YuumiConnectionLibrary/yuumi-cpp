#pragma once
#include <yuumi/protocol.hpp>
#include <yuumi/transport.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace yuumi {

    inline constexpr std::size_t MAX_MESSAGE_SIZE = 16 * 1024 * 1024;

    using MessageHandler = std::function<void(const Json&, Channel)>;
    using ErrorHandler = std::function<void(Error)>;

    class ServerBridge {
    public:
        ServerBridge() : _transport(_io_context) {}

        ~ServerBridge() { stop(); }

        Result<> start(const std::string& pipe_name, uint32_t expected_pid) {
            if (auto res = _transport.listen(pipe_name); !res) {
                return res;
            }

            if (auto res = perform_handshake(expected_pid); !res) {
                return res;
            }

            _connected.store(true, std::memory_order_release);

            _io_threads.emplace_back([this] { read_loop(); });
            _io_threads.emplace_back([this] { write_loop(); });
            _io_threads.emplace_back([this] { heartbeat_loop(); });

            return {};
        }

        void stop() {
            if (_connected.exchange(false, std::memory_order_acq_rel)) {
                asio::error_code ignored;
                _transport.socket().close(ignored);
            }
            _queue_cv.notify_all();
            for (auto& thread : _io_threads) {
                if (thread.joinable()) {
                    if (thread.get_id() == std::this_thread::get_id()) {
                        thread.detach();
                    } else {
                        thread.join();
                    }
                }
            }
            _io_threads.clear();
        }

        void send(const Json& payload, Channel channel = Channel::Command) {
            std::lock_guard lock(_queue_mutex);
            _send_queue.push({payload, channel});
            _queue_cv.notify_one();
        }

        void on_message(MessageHandler handler) {
            std::lock_guard lock(_handler_mutex);
            _message_handler = std::move(handler);
        }

        void on_error(ErrorHandler handler) {
            std::lock_guard lock(_handler_mutex);
            _error_handler = std::move(handler);
        }

    private:
        Result<> perform_handshake(uint32_t expected_pid) {
            Handshake handshake{};
            asio::error_code ec;

            asio::read(_transport.socket(), asio::buffer(&handshake, sizeof(Handshake)), ec);
            if (ec) {
                return std::unexpected(Error::ReadError);
            }

            if constexpr (std::endian::native == std::endian::little) {
                handshake.magic = std::byteswap(handshake.magic);
                handshake.version = std::byteswap(handshake.version);
                handshake.pid = std::byteswap(handshake.pid);
            }

            if (handshake.magic != 0x59554d49 || handshake.version != PROTOCOL_VERSION) {
                return std::unexpected(Error::HandshakeFailed);
            }

            if (expected_pid != 0 && handshake.pid != expected_pid) {
                return std::unexpected(Error::PidMismatch);
            }

            if (handshake.reserved[0] != 0 || handshake.reserved[1] != 0 || handshake.reserved[2] != 0) {
                return std::unexpected(Error::ProtocolViolation);
            }

            const auto caps = handshake.encoding_caps;
            if ((caps & static_cast<uint8_t>(Encoding::MsgPack)) != 0) {
                _encoding = Encoding::MsgPack;
            } else if ((caps & static_cast<uint8_t>(Encoding::JSON)) != 0) {
                _encoding = Encoding::JSON;
            } else {
                return std::unexpected(Error::HandshakeFailed);
            }

            const std::array<uint8_t, 4> ack = {static_cast<uint8_t>(_encoding), 0, 0, 0};
            asio::write(_transport.socket(), asio::buffer(ack), ec);
            if (ec) {
                return std::unexpected(Error::SendError);
            }

            return {};
        }

        void notify_error(Error error) {
            ErrorHandler handler;
            {
                std::lock_guard lock(_handler_mutex);
                handler = _error_handler;
            }
            if (handler) {
                handler(error);
            }
        }

        void read_loop() {
            while (_connected.load(std::memory_order_acquire)) {
                std::array<std::byte, 6> header{};
                asio::error_code ec;

                asio::read(_transport.socket(), asio::buffer(header), ec);
                if (ec) {
                    break;
                }

                uint32_t payload_size = 0;
                std::memcpy(&payload_size, header.data(), sizeof(payload_size));
                if constexpr (std::endian::native == std::endian::little) {
                    payload_size = std::byteswap(payload_size);
                }

                if (payload_size > MAX_MESSAGE_SIZE) {
                    notify_error(Error::ProtocolViolation);
                    break;
                }

                if (header[5] != std::byte{0}) {
                    notify_error(Error::ProtocolViolation);
                    break;
                }

                const auto channel = static_cast<Channel>(header[4]);
                std::vector<std::byte> body(payload_size);
                asio::read(_transport.socket(), asio::buffer(body), ec);
                if (ec) {
                    break;
                }

                auto decoded = Protocol::decode(body, _encoding);
                if (!decoded) {
                    notify_error(decoded.error());
                    break;
                }

                MessageHandler handler;
                {
                    std::lock_guard lock(_handler_mutex);
                    handler = _message_handler;
                }
                if (handler) {
                    handler(*decoded, channel);
                }
            }

            if (_connected.exchange(false, std::memory_order_acq_rel)) {
                notify_error(Error::ConnectionLost);
                _queue_cv.notify_all();
            }
        }

        void write_loop() {
            while (_connected.load(std::memory_order_acquire)) {
                std::unique_lock lock(_queue_mutex);
                _queue_cv.wait(lock, [this] {
                    return !_connected.load(std::memory_order_acquire) || !_send_queue.empty();
                });

                if (!_connected.load(std::memory_order_acquire)) {
                    break;
                }

                auto [payload, channel] = _send_queue.front();
                _send_queue.pop();
                lock.unlock();

                const auto packet = Protocol::encode(payload, channel, _encoding);
                asio::error_code ec;
                asio::write(_transport.socket(), asio::buffer(packet), ec);
                if (ec) {
                    if (_connected.exchange(false, std::memory_order_acq_rel)) {
                        notify_error(Error::ConnectionLost);
                        _queue_cv.notify_all();
                    }
                    break;
                }
            }
        }

        void heartbeat_loop() {
            while (_connected.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                if (!_connected.load(std::memory_order_acquire)) {
                    break;
                }
                send({{"status", "heartbeat"}}, Channel::Control);
            }
        }

        asio::io_context _io_context;
        Transport _transport;
        Encoding _encoding{Encoding::MsgPack};
        std::atomic<bool> _connected{false};

        std::mutex _queue_mutex;
        std::condition_variable _queue_cv;
        std::queue<std::pair<Json, Channel>> _send_queue;

        std::mutex _handler_mutex;
        MessageHandler _message_handler;
        ErrorHandler _error_handler;
        std::vector<std::thread> _io_threads;
    };

    using Bridge = ServerBridge;
}

/*
 * ServerBridge is the C++ server-side SDK entry point.
 * - start listens on a normalized socket path, validates the client handshake, and starts I/O loops.
 * - send enqueues outbound frames in a thread-safe queue.
 * - on_message dispatches decoded non-control frames to user business logic.
 * - on_error reports transport or protocol failures to callers without swallowing errors.
 * - protocol safety includes 16 MiB payload cap, reserved-byte validation, and strict framing checks.
 */
