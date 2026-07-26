#pragma once

#include <yuumi/protocol.hpp>

#include <array>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>
#else
#include <cerrno>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace yuumi {

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
    for (const auto character : value) {
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

inline Result<std::string> resolve_transport_address(
    std::string_view endpoint_name,
    std::string_view token
) {
    if (!valid_endpoint_name(endpoint_name)) {
        return unexpected(Protocol::error(
            ErrorCategory::Configuration,
            StatusCode::ERR_PROTOCOL_VIOLATION,
            ErrorPhase::Configuration,
            "endpoint_name must match [A-Za-z0-9][A-Za-z0-9_-]{0,31}"
        ));
    }
    if (!valid_token(token)) {
        return unexpected(Protocol::error(
            ErrorCategory::Configuration,
            StatusCode::ERR_PROTOCOL_VIOLATION,
            ErrorPhase::Configuration,
            "token must contain exactly 32 lowercase hexadecimal characters"
        ));
    }
    const std::string stem = "yuumi-" + std::string(endpoint_name) + "-" + std::string(token);
#ifdef _WIN32
    return R"(\\.\pipe\)" + stem;
#else
    try {
        const auto address = (std::filesystem::temp_directory_path() / (stem + ".sock")).string();
        sockaddr_un native_address{};
        if (address.size() >= sizeof(native_address.sun_path)) {
            return unexpected(Protocol::error(
                ErrorCategory::Configuration,
                StatusCode::ERR_PIPE_FAILED,
                ErrorPhase::Configuration,
                "canonical Unix socket address exceeds the platform bound"
            ));
        }
        return address;
    } catch (const std::filesystem::filesystem_error& exception) {
        return unexpected(Protocol::error(
            ErrorCategory::Configuration,
            StatusCode::ERR_PIPE_FAILED,
            ErrorPhase::Configuration,
            exception.what()
        ));
    }
#endif
}

namespace detail {

enum class IoState {
    Complete,
    Closed,
    Failed
};

struct IoResult {
    IoState state{IoState::Failed};
    std::string cause;
};

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
    }

    IoResult read_exact(std::span<std::byte> output) {
        std::size_t offset = 0;
        while (offset < output.size()) {
#ifdef _WIN32
            const auto handle = handle_.load(std::memory_order_acquire);
            if (handle == INVALID_HANDLE_VALUE) {
                return {IoState::Closed, "pipe is closed"};
            }
            DWORD transferred{};
            const auto remaining = static_cast<DWORD>(
                std::min<std::size_t>(output.size() - offset, MAXDWORD)
            );
            OVERLAPPED operation{};
            operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (operation.hEvent == nullptr) {
                return {IoState::Failed, "could not create a read completion event"};
            }
            const auto started = ReadFile(handle, output.data() + offset, remaining, nullptr, &operation) != FALSE;
            auto code = started ? ERROR_SUCCESS : GetLastError();
            if (!started && code == ERROR_IO_PENDING) {
                WaitForSingleObject(operation.hEvent, INFINITE);
                if (GetOverlappedResult(handle, &operation, &transferred, FALSE)) {
                    code = ERROR_SUCCESS;
                } else {
                    code = GetLastError();
                }
            } else if (started) {
                if (!GetOverlappedResult(handle, &operation, &transferred, TRUE)) {
                    code = GetLastError();
                }
            }
            CloseHandle(operation.hEvent);
            if (code != ERROR_SUCCESS) {
                if (code == ERROR_BROKEN_PIPE || code == ERROR_PIPE_NOT_CONNECTED ||
                    code == ERROR_OPERATION_ABORTED || code == ERROR_INVALID_HANDLE) {
                    return {IoState::Closed, "peer closed the pipe"};
                }
                return {IoState::Failed, "ReadFile failed with error " + std::to_string(code)};
            }
#else
            const auto descriptor = descriptor_.load(std::memory_order_acquire);
            if (descriptor < 0) {
                return {IoState::Closed, "socket is closed"};
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

    IoResult write_exact(std::span<const std::byte> input) {
        std::size_t offset = 0;
        while (offset < input.size()) {
#ifdef _WIN32
            const auto handle = handle_.load(std::memory_order_acquire);
            if (handle == INVALID_HANDLE_VALUE) {
                return {IoState::Closed, "pipe is closed"};
            }
            DWORD transferred{};
            const auto remaining = static_cast<DWORD>(
                std::min<std::size_t>(input.size() - offset, MAXDWORD)
            );
            OVERLAPPED operation{};
            operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (operation.hEvent == nullptr) {
                return {IoState::Failed, "could not create a write completion event"};
            }
            const auto started = WriteFile(handle, input.data() + offset, remaining, nullptr, &operation) != FALSE;
            auto code = started ? ERROR_SUCCESS : GetLastError();
            if (!started && code == ERROR_IO_PENDING) {
                WaitForSingleObject(operation.hEvent, INFINITE);
                if (GetOverlappedResult(handle, &operation, &transferred, FALSE)) {
                    code = ERROR_SUCCESS;
                } else {
                    code = GetLastError();
                }
            } else if (started) {
                if (!GetOverlappedResult(handle, &operation, &transferred, TRUE)) {
                    code = GetLastError();
                }
            }
            CloseHandle(operation.hEvent);
            if (code != ERROR_SUCCESS) {
                if (code == ERROR_BROKEN_PIPE || code == ERROR_PIPE_NOT_CONNECTED ||
                    code == ERROR_OPERATION_ABORTED || code == ERROR_INVALID_HANDLE) {
                    return {IoState::Closed, "peer closed the pipe"};
                }
                return {IoState::Failed, "WriteFile failed with error " + std::to_string(code)};
            }
#else
            const auto descriptor = descriptor_.load(std::memory_order_acquire);
            if (descriptor < 0) {
                return {IoState::Closed, "socket is closed"};
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
        const auto descriptor = descriptor_.exchange(-1, std::memory_order_acq_rel);
        if (descriptor >= 0) {
            shutdown(descriptor, SHUT_RDWR);
            ::close(descriptor);
        }
#endif
    }

    std::optional<std::uint32_t> peer_pid() const {
#ifdef _WIN32
        ULONG process_id{};
        const auto handle = handle_.load(std::memory_order_acquire);
        if (handle != INVALID_HANDLE_VALUE && GetNamedPipeClientProcessId(handle, &process_id)) {
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

#ifdef _WIN32
    HANDLE native_handle() const {
        return handle_.load(std::memory_order_acquire);
    }
#else
    int native_handle() const {
        return descriptor_.load(std::memory_order_acquire);
    }
#endif

private:
#ifdef _WIN32
    std::atomic<HANDLE> handle_{INVALID_HANDLE_VALUE};
#else
    std::atomic<int> descriptor_{-1};
#endif
};

class Listener {
public:
    Listener() = default;
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    ~Listener() {
        close();
    }

    Result<> open(const std::string& address) {
        address_ = address;
#ifdef _WIN32
        wide_address_.assign(address.begin(), address.end());
        const auto probe = CreateFileW(
            wide_address_.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );
        if (probe != INVALID_HANDLE_VALUE) {
            CloseHandle(probe);
            return unexpected(endpoint_error(ErrorPhase::EndpointProbe, "endpoint already has a live owner"));
        }
        const auto probe_error = GetLastError();
        if (probe_error == ERROR_PIPE_BUSY || probe_error == ERROR_ACCESS_DENIED) {
            return unexpected(endpoint_error(ErrorPhase::EndpointProbe, "endpoint already has a live owner"));
        }
        if (probe_error != ERROR_FILE_NOT_FOUND) {
            return unexpected(endpoint_error(
                ErrorPhase::EndpointProbe,
                "Named Pipe probe failed with error " + std::to_string(probe_error)
            ));
        }
        if (auto security = create_security(); !security) {
            return security;
        }
        open_.store(true, std::memory_order_release);
        const auto instance = create_instance(true);
        if (instance == INVALID_HANDLE_VALUE) {
            open_.store(false, std::memory_order_release);
            if (security_descriptor_ != nullptr) {
                LocalFree(security_descriptor_);
                security_descriptor_ = nullptr;
                security_attributes_ = {};
            }
            return unexpected(endpoint_error(
                ErrorPhase::EndpointOpen,
                "CreateNamedPipeW failed with error " + std::to_string(GetLastError())
            ));
        }
        {
            std::lock_guard lock(pending_mutex_);
            pending_ = instance;
        }
#else
        sockaddr_un endpoint{};
        endpoint.sun_family = AF_UNIX;
        std::memcpy(endpoint.sun_path, address.c_str(), address.size() + 1);
        const auto probe = socket(AF_UNIX, SOCK_STREAM, 0);
        if (probe < 0) {
            return unexpected(endpoint_error(ErrorPhase::EndpointProbe, "could not create probe socket"));
        }
        if (connect(probe, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) == 0) {
            ::close(probe);
            return unexpected(endpoint_error(ErrorPhase::EndpointProbe, "endpoint already has a live owner"));
        }
        const auto probe_error = errno;
        ::close(probe);
        if (probe_error != ENOENT && probe_error != ECONNREFUSED) {
            return unexpected(endpoint_error(
                ErrorPhase::EndpointProbe,
                std::error_code(probe_error, std::generic_category()).message()
            ));
        }
        struct stat node_status{};
        if (lstat(address.c_str(), &node_status) == 0) {
            if (!S_ISSOCK(node_status.st_mode)) {
                return unexpected(endpoint_error(ErrorPhase::EndpointOpen, "canonical address is not a socket node"));
            }
            if (unlink(address.c_str()) != 0) {
                return unexpected(endpoint_error(ErrorPhase::EndpointOpen, "could not remove stale socket node"));
            }
        } else if (errno != ENOENT) {
            return unexpected(endpoint_error(ErrorPhase::EndpointOpen, "could not inspect canonical socket node"));
        }
        const auto descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
        if (descriptor < 0) {
            return unexpected(endpoint_error(ErrorPhase::EndpointOpen, "could not create listener socket"));
        }
        if (bind(descriptor, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
            ::close(descriptor);
            return unexpected(endpoint_error(ErrorPhase::EndpointOpen, "could not bind canonical socket"));
        }
        if (chmod(address.c_str(), S_IRUSR | S_IWUSR) != 0) {
            ::close(descriptor);
            unlink(address.c_str());
            return unexpected(endpoint_error(ErrorPhase::EndpointOpen, "could not apply mode 0600"));
        }
        if (listen(descriptor, SOMAXCONN) != 0) {
            ::close(descriptor);
            unlink(address.c_str());
            return unexpected(endpoint_error(ErrorPhase::EndpointOpen, "could not listen on canonical socket"));
        }
        descriptor_.store(descriptor, std::memory_order_release);
        open_.store(true, std::memory_order_release);
#endif
        return {};
    }

    Expected<std::shared_ptr<Stream>, std::string> accept() {
#ifdef _WIN32
        HANDLE instance = INVALID_HANDLE_VALUE;
        {
            std::lock_guard lock(pending_mutex_);
            if (!open_.load(std::memory_order_acquire)) {
                return unexpected("listener is closed");
            }
            if (pending_ == INVALID_HANDLE_VALUE) {
                pending_ = create_instance(false);
            }
            instance = pending_;
        }
        if (instance == INVALID_HANDLE_VALUE) {
            return unexpected("could not create another Named Pipe instance");
        }
        OVERLAPPED operation{};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (operation.hEvent == nullptr) {
            return unexpected("could not create a pipe-accept completion event");
        }
        const auto connected = ConnectNamedPipe(instance, &operation) != FALSE;
        auto connect_error = connected ? ERROR_SUCCESS : GetLastError();
        if (!connected && connect_error == ERROR_IO_PENDING) {
            WaitForSingleObject(operation.hEvent, INFINITE);
            DWORD transferred{};
            if (GetOverlappedResult(instance, &operation, &transferred, FALSE)) {
                connect_error = ERROR_SUCCESS;
            } else {
                connect_error = GetLastError();
            }
        } else if (!connected && connect_error == ERROR_PIPE_CONNECTED) {
            connect_error = ERROR_SUCCESS;
        }
        CloseHandle(operation.hEvent);
        if (connect_error != ERROR_SUCCESS) {
            bool owns_instance{};
            {
                std::lock_guard lock(pending_mutex_);
                if (pending_ == instance) {
                    pending_ = INVALID_HANDLE_VALUE;
                    owns_instance = true;
                }
            }
            if (owns_instance) {
                CloseHandle(instance);
            }
            return unexpected("ConnectNamedPipe failed with error " + std::to_string(connect_error));
        }
        {
            std::lock_guard lock(pending_mutex_);
            if (pending_ == instance) {
                pending_ = INVALID_HANDLE_VALUE;
            }
        }
        if (!open_.load(std::memory_order_acquire)) {
            CloseHandle(instance);
            return unexpected("listener is closed");
        }
        return std::make_shared<Stream>(instance);
#else
        const auto descriptor = descriptor_.load(std::memory_order_acquire);
        if (descriptor < 0) {
            return unexpected("listener is closed");
        }
        const auto connection = ::accept(descriptor, nullptr, nullptr);
        if (connection < 0) {
            return unexpected(std::error_code(errno, std::generic_category()).message());
        }
        return std::make_shared<Stream>(connection);
#endif
    }

    void close() {
        if (!open_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
#ifdef _WIN32
        HANDLE pending = INVALID_HANDLE_VALUE;
        {
            std::lock_guard lock(pending_mutex_);
            pending = pending_;
            pending_ = INVALID_HANDLE_VALUE;
        }
        if (pending != INVALID_HANDLE_VALUE) {
            CancelIoEx(pending, nullptr);
            CloseHandle(pending);
        }
        if (security_descriptor_ != nullptr) {
            LocalFree(security_descriptor_);
            security_descriptor_ = nullptr;
        }
#else
        const auto descriptor = descriptor_.exchange(-1, std::memory_order_acq_rel);
        if (descriptor >= 0) {
            shutdown(descriptor, SHUT_RDWR);
            ::close(descriptor);
        }
        if (!address_.empty()) {
            unlink(address_.c_str());
        }
#endif
    }

private:
    static ErrorInfo endpoint_error(ErrorPhase phase, std::string cause) {
        return Protocol::error(
            ErrorCategory::Endpoint,
            StatusCode::ERR_PIPE_FAILED,
            phase,
            std::move(cause)
        );
    }

#ifdef _WIN32
    Result<> create_security() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            return unexpected(endpoint_error(ErrorPhase::EndpointOpen, "could not open the process token"));
        }
        DWORD required{};
        GetTokenInformation(token, TokenUser, nullptr, 0, &required);
        std::vector<std::byte> buffer(required);
        if (!GetTokenInformation(token, TokenUser, buffer.data(), required, &required)) {
            CloseHandle(token);
            return unexpected(endpoint_error(ErrorPhase::EndpointOpen, "could not read the current user SID"));
        }
        CloseHandle(token);
        auto* token_user = reinterpret_cast<TOKEN_USER*>(buffer.data());
        LPWSTR sid_text = nullptr;
        if (!ConvertSidToStringSidW(token_user->User.Sid, &sid_text)) {
            return unexpected(endpoint_error(ErrorPhase::EndpointOpen, "could not format the current user SID"));
        }
        const std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid_text) + L")";
        LocalFree(sid_text);
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(),
                SDDL_REVISION_1,
                &security_descriptor_,
                nullptr)) {
            return unexpected(endpoint_error(ErrorPhase::EndpointOpen, "could not create the intended-user pipe ACL"));
        }
        security_attributes_.nLength = sizeof(security_attributes_);
        security_attributes_.lpSecurityDescriptor = security_descriptor_;
        security_attributes_.bInheritHandle = FALSE;
        return {};
    }

    HANDLE create_instance(bool first) const {
        DWORD open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
        if (first) {
            open_mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
        }
        return CreateNamedPipeW(
            wide_address_.c_str(),
            open_mode,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES,
            64U * 1024U,
            64U * 1024U,
            0,
            const_cast<SECURITY_ATTRIBUTES*>(&security_attributes_)
        );
    }

    std::wstring wide_address_;
    mutable SECURITY_ATTRIBUTES security_attributes_{};
    PSECURITY_DESCRIPTOR security_descriptor_{nullptr};
    std::mutex pending_mutex_;
    HANDLE pending_{INVALID_HANDLE_VALUE};
#else
    std::atomic<int> descriptor_{-1};
#endif
    std::string address_;
    std::atomic<bool> open_{false};
};

inline Expected<std::shared_ptr<Stream>, std::string> connect(const std::string& address) {
#ifdef _WIN32
    const std::wstring wide_address(address.begin(), address.end());
    const auto handle = CreateFileW(
        wide_address.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        nullptr
    );
    if (handle == INVALID_HANDLE_VALUE) {
        return unexpected("CreateFileW failed with error " + std::to_string(GetLastError()));
    }
    DWORD mode = PIPE_READMODE_BYTE;
    if (!SetNamedPipeHandleState(handle, &mode, nullptr, nullptr)) {
        CloseHandle(handle);
        return unexpected("could not select byte-stream pipe mode");
    }
    return std::make_shared<Stream>(handle);
#else
    sockaddr_un endpoint{};
    endpoint.sun_family = AF_UNIX;
    std::memcpy(endpoint.sun_path, address.c_str(), address.size() + 1);
    const auto descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0) {
        return unexpected("could not create client socket");
    }
    if (::connect(descriptor, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
        const auto cause = std::error_code(errno, std::generic_category()).message();
        ::close(descriptor);
        return unexpected(cause);
    }
    return std::make_shared<Stream>(descriptor);
#endif
}

}
}

/*
 * Linux and macOS use owner-only Unix sockets derived from the OS temp API.
 * Windows uses byte-stream Named Pipe instances with remote clients rejected
 * and a protected ACL granting access only to the current user SID.
 * Endpoint probes connect before stale cleanup, so live or busy owners are
 * never replaced merely because an address object exists.
 */
