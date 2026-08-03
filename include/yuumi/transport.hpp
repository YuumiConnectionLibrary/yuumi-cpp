#pragma once

#include <yuumi/protocol.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace yuumi::detail {

using Deadline = std::optional<std::chrono::steady_clock::time_point>;

inline bool valid_endpoint_name(std::string_view value) {
    if (value.empty() || value.size() > 32) {
        return false;
    }
    const auto alphanumeric = [](unsigned char character) {
        return (character >= 'A' && character <= 'Z') ||
               (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9');
    };
    if (!alphanumeric(static_cast<unsigned char>(value.front()))) {
        return false;
    }
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (!alphanumeric(byte) && character != '_' && character != '-') {
            return false;
        }
    }
    return true;
}

inline bool valid_token(std::string_view value) {
    if (value.size() != 32) {
        return false;
    }
    return std::ranges::all_of(value, [](char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
    });
}

inline std::string address_digest(std::string_view endpoint_name, std::string_view token) {
    constexpr std::array<std::uint32_t, 64> constants{
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };
    std::vector<std::uint8_t> input;
    input.reserve(7 + endpoint_name.size() + token.size() + 72);
    input.insert(input.end(), {'y', 'u', 'u', 'm', 'i'});
    input.push_back(0);
    input.insert(input.end(), endpoint_name.begin(), endpoint_name.end());
    input.push_back(0);
    input.insert(input.end(), token.begin(), token.end());
    const auto bit_length = static_cast<std::uint64_t>(input.size()) * 8U;
    input.push_back(0x80);
    while ((input.size() % 64) != 56) {
        input.push_back(0);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        input.push_back(static_cast<std::uint8_t>((bit_length >> shift) & 0xFFU));
    }
    std::array<std::uint32_t, 8> hash{
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    for (std::size_t offset = 0; offset < input.size(); offset += 64) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            const auto base = offset + index * 4;
            words[index] = (static_cast<std::uint32_t>(input[base]) << 24U) |
                           (static_cast<std::uint32_t>(input[base + 1]) << 16U) |
                           (static_cast<std::uint32_t>(input[base + 2]) << 8U) |
                           static_cast<std::uint32_t>(input[base + 3]);
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const auto first = std::rotr(words[index - 15], 7) ^ std::rotr(words[index - 15], 18) ^ (words[index - 15] >> 3U);
            const auto second = std::rotr(words[index - 2], 17) ^ std::rotr(words[index - 2], 19) ^ (words[index - 2] >> 10U);
            words[index] = words[index - 16] + first + words[index - 7] + second;
        }
        auto [a, b, c, d, e, f, g, h] = hash;
        for (std::size_t index = 0; index < words.size(); ++index) {
            const auto upper = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const auto choose = (e & f) ^ (~e & g);
            const auto first = h + upper + choose + constants[index] + words[index];
            const auto lower = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto second = lower + majority;
            h = g;
            g = f;
            f = e;
            e = d + first;
            d = c;
            c = b;
            b = a;
            a = first + second;
        }
        hash[0] += a;
        hash[1] += b;
        hash[2] += c;
        hash[3] += d;
        hash[4] += e;
        hash[5] += f;
        hash[6] += g;
        hash[7] += h;
    }
    constexpr std::array<char, 16> hex{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string result;
    result.reserve(32);
    for (std::size_t index = 0; index < 4; ++index) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            result.push_back(hex[(hash[index] >> shift) & 0x0FU]);
        }
    }
    return result;
}

inline Result<std::string> resolve_transport_address(
    std::string_view endpoint_name,
    std::string_view token
) {
    if (!valid_endpoint_name(endpoint_name)) {
        return unexpected(Protocol::error(
            ErrorKind::Configuration,
            "endpoint_name must match [A-Za-z0-9][A-Za-z0-9_-]{0,31}",
            StatusCode::ERR_PROTOCOL_VIOLATION,
            ErrorPhase::Configuration
        ));
    }
    if (!valid_token(token)) {
        return unexpected(Protocol::error(
            ErrorKind::Configuration,
            "token must contain exactly 32 lowercase hexadecimal characters",
            StatusCode::ERR_PROTOCOL_VIOLATION,
            ErrorPhase::Configuration
        ));
    }
#ifdef _WIN32
    return R"(\\.\pipe\yuumi-)" + std::string(endpoint_name) + "-" + std::string(token);
#else
    try {
        const auto filename = "yuumi-" + address_digest(endpoint_name, token) + ".sock";
        const auto address = (std::filesystem::temp_directory_path() / filename).string();
        sockaddr_un native_address{};
        if (address.size() >= sizeof(native_address.sun_path)) {
            return unexpected(Protocol::error(
                ErrorKind::AddressDerivation,
                "canonical Unix socket address exceeds the platform bound",
                StatusCode::ERR_PIPE_FAILED,
                ErrorPhase::AddressDerivation
            ));
        }
        return address;
    } catch (const std::filesystem::filesystem_error& exception) {
        return unexpected(Protocol::error(
            ErrorKind::AddressDerivation,
            exception.what(),
            StatusCode::ERR_PIPE_FAILED,
            ErrorPhase::AddressDerivation
        ));
    }
#endif
}

enum class IoState {
    Complete,
    Closed,
    TimedOut,
    Failed
};

struct IoResult {
    IoState state{IoState::Failed};
    std::string cause;
};

inline std::chrono::milliseconds remaining(Deadline deadline) {
    if (!deadline) {
        return std::chrono::milliseconds::max();
    }
    const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
        *deadline - std::chrono::steady_clock::now()
    );
    return std::max(std::chrono::milliseconds::zero(), value);
}

class Stream {
public:
#ifdef _WIN32
    explicit Stream(HANDLE handle) : handle_(handle) {}
#else
    explicit Stream(int descriptor) : descriptor_(descriptor) {
#ifdef __APPLE__
        int enabled = 1;
        setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
    }
#endif

    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    ~Stream() {
        close();
#ifndef _WIN32
        const auto descriptor = descriptor_.exchange(-1, std::memory_order_acq_rel);
        if (descriptor >= 0) {
            ::close(descriptor);
        }
#endif
    }

    IoResult read_exact(std::span<std::byte> output, Deadline deadline = std::nullopt) {
        std::size_t offset{};
        while (offset < output.size()) {
#ifdef _WIN32
            const auto handle = handle_.load(std::memory_order_acquire);
            if (handle == INVALID_HANDLE_VALUE) {
                return {IoState::Closed, "pipe is closed"};
            }
            DWORD transferred{};
            const auto count = static_cast<DWORD>(
                std::min<std::size_t>(output.size() - offset, MAXDWORD)
            );
            const auto result = overlapped_io(
                handle,
                deadline,
                [&](OVERLAPPED* operation) {
                    return ReadFile(handle, output.data() + offset, count, nullptr, operation);
                },
                transferred,
                "ReadFile"
            );
            if (result.state != IoState::Complete) {
                return result;
            }
#else
            if (closed_.load(std::memory_order_acquire)) {
                return {IoState::Closed, "socket is closed"};
            }
            const auto descriptor = descriptor_.load(std::memory_order_acquire);
            if (descriptor < 0) {
                return {IoState::Closed, "socket is closed"};
            }
            if (auto waited = wait_ready(descriptor, POLLIN, deadline); waited.state != IoState::Complete) {
                return waited;
            }
            const auto transferred = recv(
                descriptor,
                output.data() + offset,
                output.size() - offset,
                0
            );
            if (transferred < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EBADF || errno == ECONNRESET || errno == ENOTCONN) {
                    return {IoState::Closed, "peer closed the socket"};
                }
                return {IoState::Failed, std::error_code(errno, std::generic_category()).message()};
            }
#endif
            if (transferred == 0) {
                return {IoState::Closed, "peer closed the transport"};
            }
            offset += static_cast<std::size_t>(transferred);
        }
        return {IoState::Complete, {}};
    }

    IoResult write_exact(std::span<const std::byte> input, Deadline deadline = std::nullopt) {
        std::lock_guard lock(write_mutex_);
        std::size_t offset{};
        while (offset < input.size()) {
#ifdef _WIN32
            const auto handle = handle_.load(std::memory_order_acquire);
            if (handle == INVALID_HANDLE_VALUE) {
                return {IoState::Closed, "pipe is closed"};
            }
            DWORD transferred{};
            const auto count = static_cast<DWORD>(
                std::min<std::size_t>(input.size() - offset, MAXDWORD)
            );
            const auto result = overlapped_io(
                handle,
                deadline,
                [&](OVERLAPPED* operation) {
                    return WriteFile(handle, input.data() + offset, count, nullptr, operation);
                },
                transferred,
                "WriteFile"
            );
            if (result.state != IoState::Complete) {
                return result;
            }
#else
            if (closed_.load(std::memory_order_acquire)) {
                return {IoState::Closed, "socket is closed"};
            }
            const auto descriptor = descriptor_.load(std::memory_order_acquire);
            if (descriptor < 0) {
                return {IoState::Closed, "socket is closed"};
            }
            if (auto waited = wait_ready(descriptor, POLLOUT, deadline); waited.state != IoState::Complete) {
                return waited;
            }
#ifdef MSG_NOSIGNAL
            constexpr int send_flags = MSG_NOSIGNAL;
#else
            constexpr int send_flags = 0;
#endif
            const auto transferred = send(
                descriptor,
                input.data() + offset,
                input.size() - offset,
                send_flags
            );
            if (transferred < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EPIPE || errno == EBADF || errno == ECONNRESET) {
                    return {IoState::Closed, "peer closed the socket"};
                }
                return {IoState::Failed, std::error_code(errno, std::generic_category()).message()};
            }
#endif
            if (transferred == 0) {
                return {IoState::Closed, "transport accepted zero output bytes"};
            }
            offset += static_cast<std::size_t>(transferred);
        }
        return {IoState::Complete, {}};
    }

    void close() {
#ifdef _WIN32
        const auto handle = handle_.exchange(INVALID_HANDLE_VALUE, std::memory_order_acq_rel);
        if (handle != INVALID_HANDLE_VALUE) {
            CancelIoEx(handle, nullptr);
            CloseHandle(handle);
        }
#else
        if (!closed_.exchange(true, std::memory_order_acq_rel)) {
            const auto descriptor = descriptor_.load(std::memory_order_acquire);
            shutdown(descriptor, SHUT_RDWR);
        }
#endif
    }

    std::optional<std::uint32_t> peer_pid() const {
#ifdef _WIN32
        ULONG process_id{};
        const auto handle = handle_.load(std::memory_order_acquire);
        if (handle != INVALID_HANDLE_VALUE && GetNamedPipeServerProcessId(handle, &process_id)) {
            return static_cast<std::uint32_t>(process_id);
        }
#elif defined(__linux__)
        ucred credentials{};
        socklen_t size = sizeof(credentials);
        const auto descriptor = descriptor_.load(std::memory_order_acquire);
        if (descriptor >= 0 &&
            getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0) {
            return static_cast<std::uint32_t>(credentials.pid);
        }
#elif defined(__APPLE__) && defined(LOCAL_PEERPID)
        pid_t process_id{};
        socklen_t size = sizeof(process_id);
        const auto descriptor = descriptor_.load(std::memory_order_acquire);
        if (descriptor >= 0 &&
            getsockopt(descriptor, SOL_LOCAL, LOCAL_PEERPID, &process_id, &size) == 0) {
            return static_cast<std::uint32_t>(process_id);
        }
#endif
        return std::nullopt;
    }

private:
#ifdef _WIN32
    template <typename Start>
    static IoResult overlapped_io(
        HANDLE handle,
        Deadline deadline,
        Start start,
        DWORD& transferred,
        std::string_view operation_name
    ) {
        OVERLAPPED operation{};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (operation.hEvent == nullptr) {
            return {IoState::Failed, "could not create an I/O completion event"};
        }
        auto code = start(&operation) ? ERROR_SUCCESS : GetLastError();
        if (code == ERROR_SUCCESS) {
            if (!GetOverlappedResult(handle, &operation, &transferred, TRUE)) {
                code = GetLastError();
            }
        } else if (code == ERROR_IO_PENDING) {
            const auto wait_time = deadline
                ? static_cast<DWORD>(std::min<std::int64_t>(remaining(deadline).count(), MAXDWORD - 1ULL))
                : INFINITE;
            const auto wait_result = WaitForSingleObject(operation.hEvent, wait_time);
            if (wait_result == WAIT_TIMEOUT) {
                CancelIoEx(handle, &operation);
                WaitForSingleObject(operation.hEvent, INFINITE);
                CloseHandle(operation.hEvent);
                return {IoState::TimedOut, std::string(operation_name) + " timed out"};
            }
            if (!GetOverlappedResult(handle, &operation, &transferred, FALSE)) {
                code = GetLastError();
            } else {
                code = ERROR_SUCCESS;
            }
        }
        CloseHandle(operation.hEvent);
        if (code == ERROR_SUCCESS) {
            return {IoState::Complete, {}};
        }
        if (code == ERROR_BROKEN_PIPE || code == ERROR_PIPE_NOT_CONNECTED ||
            code == ERROR_OPERATION_ABORTED || code == ERROR_INVALID_HANDLE) {
            return {IoState::Closed, "peer closed the pipe"};
        }
        return {IoState::Failed, std::string(operation_name) + " failed with error " + std::to_string(code)};
    }

    std::atomic<HANDLE> handle_{INVALID_HANDLE_VALUE};
#else
    static IoResult wait_ready(int descriptor, short events, Deadline deadline) {
        if (!deadline) {
            return {IoState::Complete, {}};
        }
        pollfd item{descriptor, events, 0};
        for (;;) {
            const auto wait_time = static_cast<int>(
                std::min<std::int64_t>(remaining(deadline).count(), 50)
            );
            if (wait_time <= 0) {
                return {IoState::TimedOut, "transport I/O timed out"};
            }
            const auto result = poll(&item, 1, wait_time);
            if (result > 0) {
                return {IoState::Complete, {}};
            }
            if (result < 0 && errno != EINTR) {
                return {IoState::Failed, std::error_code(errno, std::generic_category()).message()};
            }
        }
    }

    std::atomic<int> descriptor_{-1};
    std::atomic_bool closed_{false};
#endif
    std::mutex write_mutex_;
};

inline Expected<std::shared_ptr<Stream>, ErrorInfo> dial(
    const std::string& address,
    Deadline deadline,
    const std::atomic_bool& cancelled
) {
#ifdef _WIN32
    const std::wstring wide_address(address.begin(), address.end());
    for (;;) {
        if (cancelled.load(std::memory_order_acquire)) {
            return unexpected(Protocol::error(
                ErrorKind::SessionClosed,
                "connection attempt was closed locally",
                StatusCode::ERR_CONNECTION_LOST,
                ErrorPhase::Close
            ));
        }
        const auto handle = CreateFileW(
            wide_address.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr
        );
        if (handle != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_BYTE;
            if (!SetNamedPipeHandleState(handle, &mode, nullptr, nullptr)) {
                const auto code = GetLastError();
                CloseHandle(handle);
                return unexpected(Protocol::error(
                    ErrorKind::Dial,
                    "could not select byte-stream pipe mode: " + std::to_string(code),
                    StatusCode::ERR_PIPE_FAILED,
                    ErrorPhase::Dial
                ));
            }
            return std::make_shared<Stream>(handle);
        }
        const auto code = GetLastError();
        if (code != ERROR_PIPE_BUSY) {
            return unexpected(Protocol::error(
                ErrorKind::Dial,
                "CreateFileW failed with error " + std::to_string(code),
                StatusCode::ERR_PIPE_FAILED,
                ErrorPhase::Dial
            ));
        }
        const auto wait_time = static_cast<DWORD>(std::min<std::int64_t>(remaining(deadline).count(), 50));
        if (wait_time == 0) {
            return unexpected(Protocol::error(
                ErrorKind::Timeout,
                "dial timed out while the Named Pipe was busy",
                StatusCode::ERR_READ_TIMEOUT,
                ErrorPhase::Dial
            ));
        }
        WaitNamedPipeW(wide_address.c_str(), wait_time);
    }
#else
    sockaddr_un endpoint{};
    endpoint.sun_family = AF_UNIX;
    std::memcpy(endpoint.sun_path, address.c_str(), address.size() + 1);
    const auto descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0) {
        return unexpected(Protocol::error(
            ErrorKind::Dial,
            "could not create client socket",
            StatusCode::ERR_PIPE_FAILED,
            ErrorPhase::Dial
        ));
    }
    const auto flags = fcntl(descriptor, F_GETFL, 0);
    if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0) {
        const auto cause = std::error_code(errno, std::generic_category()).message();
        ::close(descriptor);
        return unexpected(Protocol::error(
            ErrorKind::Dial,
            cause,
            StatusCode::ERR_PIPE_FAILED,
            ErrorPhase::Dial
        ));
    }
    if (::connect(descriptor, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) != 0 &&
        errno != EINPROGRESS) {
        const auto cause = std::error_code(errno, std::generic_category()).message();
        ::close(descriptor);
        return unexpected(Protocol::error(
            ErrorKind::Dial,
            cause,
            StatusCode::ERR_PIPE_FAILED,
            ErrorPhase::Dial
        ));
    }
    for (;;) {
        if (cancelled.load(std::memory_order_acquire)) {
            ::close(descriptor);
            return unexpected(Protocol::error(
                ErrorKind::SessionClosed,
                "connection attempt was closed locally",
                StatusCode::ERR_CONNECTION_LOST,
                ErrorPhase::Close
            ));
        }
        const auto wait_time = static_cast<int>(std::min<std::int64_t>(remaining(deadline).count(), 50));
        if (wait_time <= 0) {
            ::close(descriptor);
            return unexpected(Protocol::error(
                ErrorKind::Timeout,
                "dial timed out",
                StatusCode::ERR_READ_TIMEOUT,
                ErrorPhase::Dial
            ));
        }
        pollfd item{descriptor, POLLOUT, 0};
        const auto polled = poll(&item, 1, wait_time);
        if (polled == 0 || (polled < 0 && errno == EINTR)) {
            continue;
        }
        if (polled < 0) {
            const auto cause = std::error_code(errno, std::generic_category()).message();
            ::close(descriptor);
            return unexpected(Protocol::error(
                ErrorKind::Dial,
                cause,
                StatusCode::ERR_PIPE_FAILED,
                ErrorPhase::Dial
            ));
        }
        int socket_error{};
        socklen_t size = sizeof(socket_error);
        getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &socket_error, &size);
        if (socket_error != 0) {
            const auto cause = std::error_code(socket_error, std::generic_category()).message();
            ::close(descriptor);
            return unexpected(Protocol::error(
                ErrorKind::Dial,
                cause,
                StatusCode::ERR_PIPE_FAILED,
                ErrorPhase::Dial
            ));
        }
        break;
    }
    if (fcntl(descriptor, F_SETFL, flags) != 0) {
        const auto cause = std::error_code(errno, std::generic_category()).message();
        ::close(descriptor);
        return unexpected(Protocol::error(
            ErrorKind::Dial,
            cause,
            StatusCode::ERR_PIPE_FAILED,
            ErrorPhase::Dial
        ));
    }
    return std::make_shared<Stream>(descriptor);
#endif
}

}

/*
 * Engines only derive and dial the platform-native local address. Endpoint
 * creation, liveness probing, stale cleanup, permissions, and pipe security
 * remain exclusively owned by the Go listener.
 */
