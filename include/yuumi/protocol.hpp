#pragma once
#include <yuumi/types.hpp>
#include <nlohmann/json.hpp>
#include <bit>
#include <span>
#include <vector>
#include <cstring>

namespace yuumi {

    using Json = nlohmann::json;

    class Protocol {
    public:
        enum Flags : uint8_t {
            None       = 0,
            Compressed = 1 << 0,
            Encrypted  = 1 << 1
        };

        static std::vector<std::byte> encode(const Json& j, Channel ch, Encoding enc = Encoding::MsgPack) {
            std::vector<uint8_t> raw;
            if (enc == Encoding::MsgPack) {
                raw = Json::to_msgpack(j);
            } else {
                const std::string s = j.dump();
                raw.assign(s.begin(), s.end());
            }
            uint32_t length = static_cast<uint32_t>(raw.size());
            uint32_t length = static_cast<uint32_t>(raw.size());
            uint32_t wire_length = (std::endian::native == std::endian::little) ? std::byteswap(length) : length;

            std::vector<std::byte> packet(6 + raw.size());
            std::memcpy(packet.data(), &wire_length, 4);
            packet[4] = static_cast<std::byte>(ch);
            packet[5] = static_cast<std::byte>(Flags::None);
            std::memcpy(packet.data() + 6, raw.data(), raw.size());

            return packet;
        }

        static Result<Json> decode(std::span<const std::byte> buffer, Encoding enc = Encoding::MsgPack) {
            try {
                if (enc == Encoding::MsgPack) {
                    return Json::from_msgpack(buffer);
                }
                const std::string s(reinterpret_cast<const char*>(buffer.data()), buffer.size());
                return Json::parse(s);
            } catch (const nlohmann::json::exception&) {
                return std::unexpected(Error::ProtocolViolation);
            }
        }

        static bool validate_schema(const Json& j, std::initializer_list<std::string_view> required_keys) {
            for (auto key : required_keys) {
                if (!j.contains(key)) return false;
            }
            return true;
        }
    };
}

/*
 * protocol.hpp: C++ frame codec utilities for Yuumi.
 * - Encodes JSON objects into framed byte payloads with fixed 6-byte headers.
 * - Decodes payload buffers from JSON or MsgPack into typed Json objects.
 * - Provides lightweight schema-key presence validation for inbound/outbound contracts.
 */
