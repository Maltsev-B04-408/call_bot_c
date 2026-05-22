cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND EXISTS "C:/vcpkg/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "C:/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")
endif()

if(NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "Vcpkg target triplet")
endif()

project(inflbot)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SOURCES
    src/JupiterClient.cpp
    src/SolanaRpcClient.cpp
    src/TelegramClient.cpp
    src/main.cpp
)

find_path(TDLIB_INCLUDE_DIR td/telegram/td_json_client.h
    HINTS
        C:/tdlib/install/include
)

find_library(TDLIB_LIBRARY tdjson
    NAMES tdjson tdjson_static
    HINTS
        C:/tdlib/install/lib
)

if(NOT TDLIB_INCLUDE_DIR)
    message(FATAL_ERROR "TDLib include directory not found")
endif()

if(NOT TDLIB_LIBRARY)
    message(FATAL_ERROR "TDLib library not found")
endif()

find_path(LIBSODIUM_INCLUDE_DIR sodium.h
    HINTS
        C:/vcpkg/installed/x64-windows/include
)

find_library(LIBSODIUM_LIBRARY_RELEASE
    NAMES sodium libsodium
    PATHS
        C:/vcpkg/installed/x64-windows/lib
    NO_DEFAULT_PATH
)

find_library(LIBSODIUM_LIBRARY_DEBUG
    NAMES sodium libsodium
    PATHS
        C:/vcpkg/installed/x64-windows/debug/lib
    NO_DEFAULT_PATH
)

if(NOT LIBSODIUM_INCLUDE_DIR)
    message(FATAL_ERROR "libsodium include directory not found")
endif()

if(NOT LIBSODIUM_LIBRARY_RELEASE AND NOT LIBSODIUM_LIBRARY_DEBUG)
    message(FATAL_ERROR "libsodium library not found")
endif()

if(LIBSODIUM_LIBRARY_RELEASE AND LIBSODIUM_LIBRARY_DEBUG)
    set(LIBSODIUM_LIBRARIES
        optimized "${LIBSODIUM_LIBRARY_RELEASE}"
        debug "${LIBSODIUM_LIBRARY_DEBUG}"
    )
elseif(LIBSODIUM_LIBRARY_RELEASE)
    set(LIBSODIUM_LIBRARIES "${LIBSODIUM_LIBRARY_RELEASE}")
else()
    set(LIBSODIUM_LIBRARIES "${LIBSODIUM_LIBRARY_DEBUG}")
endif()

find_package(nlohmann_json CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

add_executable(inflbot ${SOURCES})

if(MSVC)
    target_compile_options(inflbot PRIVATE /utf-8)
endif()

target_include_directories(inflbot PRIVATE
    ${TDLIB_INCLUDE_DIR}
    ${LIBSODIUM_INCLUDE_DIR}
)

target_link_libraries(inflbot PRIVATE
    ${TDLIB_LIBRARY}
    ${LIBSODIUM_LIBRARIES}
    CURL::libcurl
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
)

set(RUNTIME_DLLS
    "C:/tdlib/install/bin/tdjson.dll"
    "C:/vcpkg/installed/x64-windows/bin/zlib1.dll"
    "C:/vcpkg/installed/x64-windows/bin/legacy.dll"
    "C:/vcpkg/installed/x64-windows/bin/libcrypto-3-x64.dll"
    "C:/vcpkg/installed/x64-windows/bin/libcurl.dll"
    "C:/vcpkg/installed/x64-windows/bin/libsodium.dll"
    "C:/vcpkg/installed/x64-windows/bin/libssl-3-x64.dll"
)

foreach(RUNTIME_DLL IN LISTS RUNTIME_DLLS)
    if(EXISTS "${RUNTIME_DLL}")
        add_custom_command(TARGET inflbot POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${RUNTIME_DLL}"
                $<TARGET_FILE_DIR:inflbot>
            COMMENT "Copying ${RUNTIME_DLL}"
        )
    else()
        message(WARNING "Runtime DLL not found: ${RUNTIME_DLL}")
    endif()
endforeach()

message(STATUS "TDLib include dir: ${TDLIB_INCLUDE_DIR}")
message(STATUS "TDLib library: ${TDLIB_LIBRARY}")
message(STATUS "libsodium include dir: ${LIBSODIUM_INCLUDE_DIR}")
message(STATUS "libsodium release library: ${LIBSODIUM_LIBRARY_RELEASE}")
message(STATUS "libsodium debug library: ${LIBSODIUM_LIBRARY_DEBUG}")
message(STATUS "CURL include dir: ${CURL_INCLUDE_DIRS}")
message(STATUS "nlohmann_json dir: ${nlohmann_json_DIR}")
message(STATUS "OpenSSL include dir: ${OPENSSL_INCLUDE_DIR}")
message(STATUS "OpenSSL libraries: ${OPENSSL_LIBRARIES}")
