#pragma once
#include <yuumi/types.hpp>
#include <iostream>
#include <format>
#include <string>

namespace yuumi {
    class Diagnostic {
    public:
        static void log(StatusCode code, const std::string& message) {
            std::cout << std::format("[YUUMI][{}] {}\n", static_cast<uint32_t>(code), message);
        }

        static void error(StatusCode code, const std::string& details = "") {
            std::cout << std::format("[YUUMI_ERR][{}] Error detected. Details: {}\n", static_cast<uint32_t>(code), details);
        }

        static void success(const std::string& message) {
            std::cout << std::format("[YUUMI_SUCCESS] {}\n", message);
        }
    };
}

/*
 * Diagnostic class: Standardized logging utility.
 * - log: Prints informational status codes with English messages.
 * - error: Reports failures using system-wide unique error codes.
 * - success: Confirms successful completion of major bridge operations.
 */
