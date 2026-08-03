#include <iostream>
#include <string>

#include <yuumi/transport.hpp>

int main(int argc, char** argv) {
    const std::string endpoint_name = argc > 1 ? argv[1] : "bridge";
    const std::string token = argc > 2 ? argv[2] : "0123456789abcdef0123456789abcdef";
    auto address = yuumi::detail::resolve_transport_address(endpoint_name, token);
    if (!address) {
        std::cerr << address.error().cause;
        return 1;
    }
    std::cout << *address;
    return 0;
}

/*
 * path_check consumes endpoint_name and token separately so it exercises the
 * same internal canonical derivation and validation used by Engine::connect.
 */
