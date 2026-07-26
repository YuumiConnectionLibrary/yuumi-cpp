#include "conformance_cases_d.hpp"

#include <charconv>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "expected one Engine Conformance case number\n";
        return 2;
    }
    int identifier{};
    const std::string_view text(argv[1]);
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), identifier);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || identifier < 1 || identifier > 64) {
        std::cerr << "Engine Conformance case must be in the range 1..64\n";
        return 2;
    }
    try {
        if (identifier <= 11) {
            yuumi::testkit::run_configuration_case(identifier);
        } else if (identifier <= 23) {
            yuumi::testkit::run_handshake_case(identifier);
        } else if (identifier <= 34) {
            yuumi::testkit::run_session_case(identifier);
        } else if (identifier <= 45) {
            yuumi::testkit::run_frame_case(identifier);
        } else if (identifier <= 51) {
            yuumi::testkit::run_control_case(identifier);
        } else if (identifier <= 56) {
            yuumi::testkit::run_correlation_case(identifier);
        } else {
            yuumi::testkit::run_send_case(identifier);
        }
    } catch (const yuumi::testkit::Failure& failure) {
        std::cerr << "EC-" << identifier << " failed: " << failure.what() << '\n';
        return 1;
    } catch (const std::exception& failure) {
        std::cerr << "EC-" << identifier << " raised an unexpected exception: " << failure.what() << '\n';
        return 1;
    }
    return 0;
}

/*
 * CTest invokes this executable once per numbered normative case. The switch
 * preserves the exact EC-001 through EC-064 maintenance boundary from task 04.
 */
