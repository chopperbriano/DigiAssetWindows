cmake_minimum_required(VERSION 3.24)
project(digiasset_core)

option(BUILD_CLI "Build CLI" ON)
option(BUILD_WEB "Build Web Server" OFF)  # upstream's STANDALONE web exe; this fork's :8090 console is served by the node's built-in WebServer (src/), so off by default
option(BUILD_QT "Build QT" OFF)   # upstream Qt GUI; off by default on this Windows fork (no Qt toolchain wired)
option(BUILD_TEST "Build Tests" OFF)

set(CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/jsoncpp/install" "${CMAKE_SOURCE_DIR}/libjson-rpc-cpp/install" ${CMAKE_PREFIX_PATH})

# Check if CMAKE_PREFIX_PATH is set.
if (WIN32)
  if(DEFINED CMAKE_PREFIX_PATH)
      foreach(prefix_path ${CMAKE_PREFIX_PATH})
      include_directories("${prefix_path}/include")
      link_directories("${prefix_path}/lib")
    endforeach()
  endif()
endif()

# Set compiler settings
if(MSVC)
  # Upstream (mctrivia) code uses designated initializers, which MSVC accepts only
  # at C++20 (gcc/clang take them as an extension in older modes). Build this fork
  # at C++20 on MSVC. /Zc:char8_t- keeps u8"" literals as const char[] so the
  # C++20 char8_t change doesn't break existing string code.
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_FLAGS "/W4 /EHsc /Zc:char8_t-")

  # Disable Windows features not needed.
  add_definitions(-D_WIN32_WINNT=0x0601)
  add_definitions(-DNOMINMAX)              # from asset_features: avoid Windows.h min/max macro clash with std::min/max
  add_definitions(-DWIN32_LEAN_AND_MEAN)   # from asset_features: trim unused Windows headers
else()
  set(CMAKE_CXX_STANDARD 11)
  SET(CMAKE_CXX_FLAGS "-std=c++11 -g -Wall")
endif()

# Set version number
# Upstream version. Held at 0.3.3 even though this now carries the upstream
# asset_features (v1.0.0) work: per upstream, a real 1.0.0 = asset_features PLUS
# the two new PSPs, which are not done yet. Bump to 1.0.0 once the PSP work lands.
# The fork's WIN_BUILD counter below is the real per-release version.
SET(MAJOR_VERSION 0)
SET(MINOR_VERSION 3)
SET(PATCH_VERSION 3)
SET(SO_VERSION    0)

# Windows port build number (increment for each Windows-specific release).
# This is the SOURCE OF TRUTH for the version — it is substituted into
# src/Version.h.in -> the generated (gitignored) src/Version.h. Bump this to
# match the win.<N> tag on every release; do NOT hand-edit the generated header.
SET(WIN_BUILD 105)
# NOTE: upstream added a CMAKE_*_OUTPUT_DIRECTORY -> bin/ redirect here. Not
# adopted: this fork's release/update scripts expect build/<sub>/Release/. Revisit
# later if we want the bin/ layout (would require updating those scripts).

# Add source directory
include_directories(src)
ADD_SUBDIRECTORY(src)

#create cli
if(BUILD_CLI)
    message(STATUS "\nConfiguring CLI:")
    include_directories(cli)
    ADD_SUBDIRECTORY(cli)
endif()

#create optional pool server (Windows-only for now; depends on real Boost
# from packages/ and winhttp, so it only makes sense to build on Windows).
if(WIN32 AND MSVC)
    ADD_SUBDIRECTORY(pool)
endif()

#create qt (upstream GUI; off by default via BUILD_QT on this fork)
if(BUILD_QT)
    message(STATUS "\nConfiguring QT:")
    include_directories(qt)
    ADD_SUBDIRECTORY(qt)
endif()

#create web
if(BUILD_WEB)
    message(STATUS "\nConfiguring Web:")
    include_directories(web)
    ADD_SUBDIRECTORY(web)
endif()

#build test script
if(BUILD_TEST)
    message(STATUS "\nConfiguring Tests:")
    set(GOOGLETEST_VERSION 1.11.0)
    ADD_SUBDIRECTORY(tests)
endif()
