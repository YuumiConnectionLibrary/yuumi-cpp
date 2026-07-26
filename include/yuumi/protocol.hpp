#pragma once

#include <yuumi/types.hpp>

#include <bit>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace yuumi {

class Protocol {
public:
    enum Flags : std::uint8_t {
        None = 0x00,
        Fragment = 0x01,
        LastFragment = 0x02,
        Correlated = 0x04
    };

    static std::uint32_t read_u32(std::span<const std::byte, 4> bytes) {
        std::uint32_t value{};
        std::memcpy(&value, bytes.data(), sizeof(value));
        if constexpr (std::endian::native == std::endian::little) {
            value = std::byteswap(value);
        }
        return value;
    }

    static void append_u32(std::vector<std::byte>& output, std::uint32_t value) {
        if constexpr (std::endian::native == std::endian::little) {
            value = std::byteswap(value);
        }
        const auto* first = reinterpret_cast<const std::byte*>(&value);
        output.insert(output.end(), first, first + sizeof(value));
    }

    static Result<std::vector<std::byte>> encode_payload(const Json& payload, Encoding encoding) {
        try {
            std::vector<std::uint8_t> raw;
            if (encoding == Encoding::MsgPack) {
                raw = Json::to_msgpack(payload);
            } else if (encoding == Encoding::JSON) {
                const auto text = payload.dump();
                raw.assign(text.begin(), text.end());
            } else {
                return unexpected(error(
                    ErrorCategory::Serialization,
                    StatusCode::ERR_ENCODING_UNSUPPORTED,
                    ErrorPhase::ApplicationSend,
                    "selected encoding is not supported"
                ));
            }
            std::vector<std::byte> bytes(raw.size());
            if (!raw.empty()) {
                std::memcpy(bytes.data(), raw.data(), raw.size());
            }
            return bytes;
        } catch (const nlohmann::json::exception& exception) {
            return unexpected(error(
                ErrorCategory::Serialization,
                StatusCode::ERR_PROTOCOL_VIOLATION,
                ErrorPhase::ApplicationSend,
                exception.what()
            ));
        }
    }

    static Result<Json> decode_payload(std::span<const std::byte> payload, Encoding encoding) {
        try {
            if (encoding == Encoding::MsgPack) {
                return Json::from_msgpack(payload.begin(), payload.end(), true, true);
            }
            if (encoding == Encoding::JSON) {
                return Json::parse(
                    reinterpret_cast<const char*>(payload.data()),
                    reinterpret_cast<const char*>(payload.data() + payload.size())
                );
            }
            return unexpected(error(
                ErrorCategory::Protocol,
                StatusCode::ERR_ENCODING_UNSUPPORTED,
                ErrorPhase::FrameDecode,
                "selected encoding is not supported"
            ));
        } catch (const nlohmann::json::exception& exception) {
            return unexpected(error(
                ErrorCategory::Protocol,
                StatusCode::ERR_PROTOCOL_VIOLATION,
                ErrorPhase::FrameDecode,
                exception.what()
            ));
        }
    }

    static Result<Json> decode_control(std::span<const std::byte> payload) {
        return decode_payload(payload, Encoding::JSON);
    }

    static Result<std::vector<std::byte>> frame(
        Channel channel,
        std::uint8_t flags,
        std::span<const std::byte> payload
    ) {
        if (payload.size() > MAX_MESSAGE_SIZE) {
            return unexpected(error(
                ErrorCategory::Serialization,
                StatusCode::ERR_PAYLOAD_TOO_LARGE,
                ErrorPhase::ApplicationSend,
                "frame payload exceeds 16 MiB"
            ));
        }
        std::vector<std::byte> packet;
        packet.reserve(6 + payload.size());
        append_u32(packet, static_cast<std::uint32_t>(payload.size()));
        packet.push_back(static_cast<std::byte>(channel));
        packet.push_back(static_cast<std::byte>(flags));
        packet.insert(packet.end(), payload.begin(), payload.end());
        return packet;
    }

    static Result<std::vector<std::byte>> control_frame(const Json& payload) {
        auto encoded = encode_payload(payload, Encoding::JSON);
        if (!encoded) {
            return unexpected(encoded.error());
        }
        return frame(Channel::Control, Flags::None, *encoded);
    }

    static ErrorInfo error(
        ErrorCategory category,
        StatusCode status,
        ErrorPhase phase,
        std::string cause,
        std::optional<SessionHandle> session = std::nullopt
    ) {
        return ErrorInfo{category, status, phase, std::move(cause), std::move(session)};
    }
};

}

/*
 * Protocol performs strict codec selection and never substitutes raw strings
 * for malformed JSON or MessagePack. Frame sizes are checked before allocation,
 * and Control payloads always use JSON independently of application encoding.
 */
