cmake_minimum_required(VERSION 3.24)
project(digiasset_core)

option(BUILD_CLI "Build CLI" OFF)
option(BUILD_WEB "Build Web Server" OFF)
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
set(CMAKE_CXX_STANDARD 11)
if(MSVC)
  # MSVC equivalent flags:
  # -std=c++11 : Not needed as CMAKE_CXX_STANDARD handles this
  # -g : Debug info is controlled by CMAKE_BUILD_TYPE in MSVC
  # -Wall : MSVC uses /W4 for comprehensive warnings
  set(CMAKE_CXX_FLAGS "/W4 /EHsc")

  # Disable Windows features not needed.
  add_definitions(-D_WIN32_WINNT=0x0601)
else()
  SET(CMAKE_CXX_FLAGS "-std=c++11 -g -Wall")
endif()

# Set version number
# Upstream version (keep in sync with DigiAsset-Core/DigiAsset_Core)
SET(MAJOR_VERSION 0)
SET(MINOR_VERSION 3)
SET(PATCH_VERSION 0)
SET(SO_VERSION    0)

# Windows port build number (increment for each Windows-specific release).
# This is the SOURCE OF TRUTH for the version — it is substituted into
# src/Version.h.in -> the generated (gitignored) src/Version.h. Bump this to
# match the win.<N> tag on every release; do NOT hand-edit the generated header.
SET(WIN_BUILD 39)

# Add source directory
include_directories(src)
ADD_SUBDIRECTORY(src)

#create cli
if(BUILD_CLI)
    include_directories(cli)
    ADD_SUBDIRECTORY(cli)
endif()

#create optional pool server (Windows-only for now; depends on real Boost
# from packages/ and winhttp, so it only makes sense to build on Windows).
if(WIN32 AND MSVC)
    ADD_SUBDIRECTORY(pool)
endif()

#create web
if(BUILD_WEB)
    include_directories(web)
    ADD_SUBDIRECTORY(web)
endif()

#build test script
if(BUILD_TEST)
    set(GOOGLETEST_VERSION 1.11.0)
    ADD_SUBDIRECTORY(tests)
endif()
