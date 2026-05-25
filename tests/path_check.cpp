#include <iostream>
#include <string>

#include <yuumi/transport.hpp>

int main(int argc, char** argv) {
    const std::string pipe_name = (argc > 1) ? argv[1] : "yuumi-bridge";
    std::cout << yuumi::resolve_transport_address(pipe_name);
    return 0;
}
