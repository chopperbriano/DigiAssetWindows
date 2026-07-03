cmake_minimum_required(VERSION 3.21)
project(DigiAssetPoolServer)

# Pool server - optional companion to DigiAssetWindows.exe.
#
# Implements the wire protocol mctrivia's original pool server used, so both
# the win.31+ C++ client and the legacy NodeJS digiasset_node client can
# register payout addresses and fetch the permanent asset list from a pool
# this operator controls.
#
# Same C++ toolchain, same Boost, same SQLite as the main DigiAssetWindows
# project. Ships as a standalone DigiAssetPoolServer.exe alongside the main
# exe in each release. Optional to run — only pool operators need it.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Real Boost headers first, ahead of anything else. src/boost/asio.hpp in the
# repo is a historical no-op stub we do NOT want to accidentally pick up.
# (See memory/project_boost_stub_trap.md for why.)
if(WIN32 AND MSVC)
    include_directories(BEFORE "${CMAKE_SOURCE_DIR}/packages/boost.1.82.0/lib/native/include")
endif()

include_directories("${CMAKE_SOURCE_DIR}/jsoncpp/install/include")
include_directories("${CMAKE_SOURCE_DIR}/src")

set(HEADER_FILES
        PoolDatabase.h
        PoolServer.h
        PoolDashboard.h
        PoolVerifier.h
        )

set(SOURCE_FILES
        main.cpp
        PoolDatabase.cpp
        PoolServer.cpp
        PoolDashboard.cpp
        PoolVerifier.cpp
        # Borrow sqlite3.c and curl_stubs.cpp from the main src/ tree so the
        # pool server doesn't need its own copies. sqlite for the pool db,
        # curl_stubs for the first-run HTTP snapshot of mctrivia's /permanent.
        ../src/sqlite3.c
        ../src/curl_stubs.cpp
        ../src/CurlHandler.cpp
        )

add_executable(DigiAssetPoolServer ${SOURCE_FILES} ${HEADER_FILES})

# Link jsoncpp (same static lib the main exe uses)
target_link_libraries(DigiAssetPoolServer PRIVATE
        "${CMAKE_SOURCE_DIR}/jsoncpp/install/lib/jsoncpp_static.lib")

# Windows-specific: winhttp for curl_stubs, winsock for raw TCP
if(WIN32 AND MSVC)
    target_link_libraries(DigiAssetPoolServer PRIVATE winhttp)
    target_link_libraries(DigiAssetPoolServer PRIVATE ws2_32)
    # Boost Beast uses some threading primitives
    target_link_libraries(DigiAssetPoolServer PRIVATE bcrypt)
endif()

install(TARGETS DigiAssetPoolServer DESTINATION bin)
