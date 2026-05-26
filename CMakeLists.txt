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
    VERSION 0.1.0
    DESCRIPTION "Yuumi C++ server SDK"
    LANGUAGES CXX
)

set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(asio CONFIG REQUIRED)
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
    asio::asio
    nlohmann_json::nlohmann_json
)

option(YUUMI_BUILD_TESTS "Build Yuumi test applications" ON)

if(YUUMI_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/path_check.cpp")
    add_executable(yuumi_path_check tests/path_check.cpp)
    target_link_libraries(yuumi_path_check PRIVATE yuumi::cpp)

    if(MSVC)
        target_compile_options(yuumi_path_check PRIVATE /W4 /permissive-)
    else()
        target_compile_options(yuumi_path_check PRIVATE -Wall -Wextra -pedantic)
    endif()

    message(STATUS "Yuumi path-check tool enabled.")
endif()

message(STATUS "yuumi v${PROJECT_VERSION} configured successfully.")
