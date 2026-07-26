cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "${CMAKE_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
    else()
        message(FATAL_ERROR "VCPKG_ROOT is not set and no local vcpkg toolchain was found. Set VCPKG_ROOT or clone vcpkg into ${CMAKE_SOURCE_DIR}/vcpkg.")
    endif()
endif()

project(yuumi
    VERSION 0.2.0
    DESCRIPTION "Yuumi C++ engine SDK"
    LANGUAGES CXX
)

set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# Header-only library target
add_library(yuumi_cpp INTERFACE)
add_library(yuumi::cpp ALIAS yuumi_cpp)

target_compile_features(yuumi_cpp INTERFACE cxx_std_23)

if(WIN32)
    target_compile_definitions(yuumi_cpp INTERFACE NOMINMAX _WIN32_WINNT=0x0A00)
endif()

target_include_directories(yuumi_cpp INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(yuumi_cpp INTERFACE
    Threads::Threads
    nlohmann_json::nlohmann_json
)

if(WIN32)
    target_link_libraries(yuumi_cpp INTERFACE advapi32)
endif()

option(YUUMI_BUILD_TESTS "Build Yuumi test applications" ON)
option(YUUMI_BUILD_EXAMPLES "Build Yuumi example applications" ON)

if(YUUMI_BUILD_TESTS)
    enable_testing()

    add_executable(yuumi_path_check tests/path_check.cpp)
    target_link_libraries(yuumi_path_check PRIVATE yuumi::cpp)

    if(MSVC)
        target_compile_options(yuumi_path_check PRIVATE /W4 /permissive-)
    else()
        target_compile_options(yuumi_path_check PRIVATE -Wall -Wextra -pedantic)
    endif()

    add_test(
        NAME yuumi_path_check
        COMMAND yuumi_path_check bridge 0123456789abcdef0123456789abcdef
    )

    set(
        YUUMI_SPEC_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/../yuumi-spec"
        CACHE PATH
        "Path to the canonical yuumi-spec checkout"
    )
    if(NOT EXISTS "${YUUMI_SPEC_DIR}/test-vectors/handshake_valid.bin")
        message(FATAL_ERROR "YUUMI_SPEC_DIR must point to a yuumi-spec checkout containing test-vectors")
    endif()

    add_executable(yuumi_engine_conformance tests/engine_conformance.cpp)
    target_link_libraries(yuumi_engine_conformance PRIVATE yuumi::cpp)
    target_compile_definitions(
        yuumi_engine_conformance
        PRIVATE
            YUUMI_ENABLE_TESTKIT=1
            YUUMI_SPEC_VECTOR_DIR="${YUUMI_SPEC_DIR}/test-vectors"
    )
    if(MSVC)
        target_compile_options(yuumi_engine_conformance PRIVATE /W4 /permissive-)
    else()
        target_compile_options(yuumi_engine_conformance PRIVATE -Wall -Wextra -pedantic)
    endif()

    foreach(case_number RANGE 1 64)
        if(case_number LESS 10)
            set(case_id "00${case_number}")
        else()
            set(case_id "0${case_number}")
        endif()
        add_test(
            NAME "yuumi_EC-${case_id}"
            COMMAND yuumi_engine_conformance "${case_number}"
        )
        set_tests_properties("yuumi_EC-${case_id}" PROPERTIES TIMEOUT 15)
    endforeach()

    message(STATUS "Yuumi Engine Conformance EC-001 through EC-064 enabled.")
endif()

if(YUUMI_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/cpp_server/main.cpp")
    add_executable(yuumi_cpp_server examples/cpp_server/main.cpp)
    target_link_libraries(yuumi_cpp_server PRIVATE yuumi::cpp)

    if(MSVC)
        target_compile_options(yuumi_cpp_server PRIVATE /W4 /permissive-)
    else()
        target_compile_options(yuumi_cpp_server PRIVATE -Wall -Wextra -pedantic)
    endif()

    message(STATUS "Yuumi cpp_server example enabled.")
endif()

message(STATUS "yuumi v${PROJECT_VERSION} configured successfully.")
