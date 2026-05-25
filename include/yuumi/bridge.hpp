#pragma once
#include <yuumi/transport.hpp>
#include <yuumi/protocol.hpp>
#include <queue>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <chrono>
#include <atomic>
#include <iostream>
#include <format>

namespace yuumi {

    // Maximum allowed message body size (16 MB). Any incoming frame exceeding
    // this limit is rejected as a protocol violation to prevent OOM attacks.
    inline constexpr std::size_t MAX_MESSAGE_SIZE = 16 * 1024 * 1024;

    using MessageHandler = std::function<void(const Json&, Channel)>;

    class Bridge {
    public:
        explicit Bridge() : _transport(_io_context) {}

        // Destructor ensures all I/O threads are joined before the object is destroyed,
        // preventing std::terminate() from being called on joinable threads.
        ~Bridge() { stop(); }

        // Gracefully stops the bridge: signals all threads to exit, wakes blocked
        // write_loop via condition variable, and joins all threads before returning.
        void stop() {
            _connected.store(false, std::memory_order_release);
            _queue_cv.notify_all();
            for (auto& t : _io_threads) {
                if (t.joinable()) t.join();
            }
            _io_threads.clear();
        }

        Result<> start(const std::string& pipe_name, uint32_t expected_pid) {
            if (auto res = _transport.listen(pipe_name); !res) return res;

            if (auto res = perform_handshake(expected_pid); !res) return res;

            _connected = true;

            _io_threads.emplace_back([this] { read_loop(); });
            _io_threads.emplace_back([this] { write_loop(); });
            _io_threads.emplace_back([this] { heartbeat_loop(); });

            return {};
        }

        void send(const Json& payload, Channel ch = Channel::Command) {
            std::lock_guard lock(_queue_mutex);
            _send_queue.push({payload, ch});
            _queue_cv.notify_one();
        }

        void on_message(MessageHandler handler) {
            std::lock_guard<std::mutex> lock(_handler_mutex);
            _handler = std::move(handler);
        }

        // Registers a callback invoked when an I/O error occurs on the connection.
        // The callback is invoked from the I/O thread (read_loop or write_loop),
        // NOT from the main thread. The consumer is responsible for synchronization
        // if the callback accesses shared state (e.g. UI updates).
        void on_error(std::function<void(Error)> handler) {
            std::lock_guard<std::mutex> lock(_handler_mutex);
            _error_handler = std::move(handler);
        }

    private:
        Result<> perform_handshake(uint32_t expected_pid) {
            Handshake h;
            asio::error_code ec;
            asio::read(_transport.socket(), asio::buffer(&h, sizeof(Handshake)), ec);

            if (ec) return std::unexpected(Error::ReadError);

            if constexpr (std::endian::native == std::endian::little) {
                h.magic   = std::byteswap(h.magic);
                h.version = std::byteswap(h.version);
                h.pid     = std::byteswap(h.pid);
            }
            
            if (h.magic != 0x59554d49 || h.version != PROTOCOL_VERSION) {
                return std::unexpected(Error::HandshakeFailed);
            }

            if (expected_pid != 0 && h.pid != expected_pid) {
                return std::unexpected(Error::PidMismatch);
            }

            // Select encoding: prefer MsgPack if client supports it, else JSON.
            const auto caps = h.encoding_caps;
            if (caps & static_cast<uint8_t>(Encoding::MsgPack)) {
                _encoding = Encoding::MsgPack;
            } else if (caps & static_cast<uint8_t>(Encoding::JSON)) {
                _encoding = Encoding::JSON;
            } else {
                return std::unexpected(Error::HandshakeFailed);
            }

            // ACK: [1B encoding_selected][3B zeros]
            uint8_t ack[4] = {static_cast<uint8_t>(_encoding), 0, 0, 0};
            asio::write(_transport.socket(), asio::buffer(ack, 4), ec);
            if (ec) return std::unexpected(Error::SendError);

            return {};
        }

        // Invokes the registered error handler (if any) under lock.
        // Called from I/O threads when a non-normal termination occurs.
        void notify_error(Error e) {
            std::function<void(Error)> h;
            {
                std::lock_guard<std::mutex> lock(_handler_mutex);
                h = _error_handler;
            }
            if (h) h(e);
        }

        void read_loop() {
            while (_connected) {
                uint32_t length;
                asio::error_code ec;

                std::vector<std::byte> header(6);
                asio::read(_transport.socket(), asio::buffer(header), ec);
                if (ec) break;

                std::memcpy(&length, header.data(), 4);
                if constexpr (std::endian::native == std::endian::little) length = std::byteswap(length);

                // Reject frames larger than MAX_MESSAGE_SIZE to prevent OOM from malicious/corrupt peers
                if (length > MAX_MESSAGE_SIZE) break;

                Channel ch = static_cast<Channel>(header[4]);

                std::vector<std::byte> body(length);
                asio::read(_transport.socket(), asio::buffer(body), ec);
                if (ec) break;

                if (ch == Channel::Control) continue;

                if (auto json = Protocol::decode(body, _encoding)) {
                    MessageHandler h;
                    {
                        std::lock_guard<std::mutex> lock(_handler_mutex);
                        h = _handler;
                    }
                    if (h) h(*json, ch);
                }
            }
            // Notify consumer only if the loop exited due to an I/O error,
            // not due to a normal stop() call that set _connected to false.
            if (_connected.exchange(false)) {
                notify_error(Error::ConnectionLost);
            }
        }

        void write_loop() {
            while (_connected) {
                std::unique_lock lock(_queue_mutex);
                _queue_cv.wait(lock, [this] { return !_send_queue.empty() || !_connected; });
                if (!_connected) break;

                auto [payload, ch] = _send_queue.front();
                _send_queue.pop();
                lock.unlock();

                auto packet = Protocol::encode(payload, ch, _encoding);
                asio::error_code ec;
                asio::write(_transport.socket(), asio::buffer(packet), ec);
                if (ec) {
                    _connected = false;
                    notify_error(Error::ConnectionLost);
                }
            }
        }

        void heartbeat_loop() {
            while (_connected) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
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
        MessageHandler _handler;
        std::function<void(Error)> _error_handler;
        std::vector<std::thread> _io_threads;
    };
}

/*
 * Bridge class: Main server component of Yuumi.
 * - start: Initializes the server, performs security handshake, and spawns I/O threads.
 * - send: Thread-safe method to queue asynchronous messages for delivery.
 * - on_message: Registers a callback for incoming messages.
 * - perform_handshake: Internal logic to verify protocol compatibility and PIDs.
 * - read_loop: Background thread handling framing and deserialization.
 * - write_loop: Background thread handling transmission of queued messages.
 * - heartbeat_loop: Ensures connection persistence via periodic control packets.
 */
