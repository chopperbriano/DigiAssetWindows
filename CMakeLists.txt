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

  # --- Release hardening -----------------------------------------------------
  # These binaries are published for the public to download, so the shipped exe
  # has to stand up to someone running dumpbin/strings on it.
  #
  # /guard:cf  Control Flow Guard. ASLR, DEP and high-entropy VA were already on
  #            (MSVC defaults, confirmed via dumpbin: DLL characteristics 0x8160),
  #            but CFG is NOT a default and was missing - 0x4000 absent from that
  #            same field. It has to be passed to BOTH the compiler and the linker
  #            or it silently does nothing.
  # /GS        Stack buffer security cookies. On by default, set explicitly so a
  #            future flag change cannot quietly drop it from a release build.
  # /sdl       Additional security checks + treats certain security warnings as
  #            errors. Cheap for code that parses attacker-supplied chain data.
  # /wd4996    ...but NOT the deprecated-CRT warning, which /sdl promotes to an
  #            error. That fires inside the vendored sqlite3.c amalgamation
  #            (GetVersionExA/W), which is compiled into several targets across
  #            different directories and is not ours to patch. Suppressing just this
  #            one warning keeps every other /sdl check active everywhere, instead of
  #            exempting whole files or dropping /sdl outright. Our own getenv uses
  #            were fixed properly rather than hidden by this - see envFlagSet() in
  #            DigiByteCore.cpp.
  # /d1trimfile: strips the build directory from __FILE__, which otherwise bakes
  #            absolute source paths into the exe. The shipped binary carried 15
  #            "C:\repo\DigiAssetWindows\packages\boost..." strings from Boost's
  #            assert machinery, publishing the build layout to anyone who ran
  #            strings on it. No PDB path or username was leaking (link already
  #            sets GenerateDebugInformation=false), and this closes the rest.
  # NOTE: /Qspectre is deliberately absent. It requires the Spectre-mitigated CRT
  # ("MSB8040: Spectre-mitigated libraries are required"), a separate Visual Studio
  # component every builder would then have to install. Spectre v1 mitigation buys
  # little for a single-user desktop node with no untrusted code sharing its
  # process, so it is not worth making the build harder to reproduce.
  set(DGB_MSVC_HARDENING "/guard:cf /GS /sdl /wd4996 /d1trimfile:${CMAKE_SOURCE_DIR}\\")
  set(CMAKE_CXX_FLAGS_RELEASE        "${CMAKE_CXX_FLAGS_RELEASE} ${DGB_MSVC_HARDENING}")
  set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} ${DGB_MSVC_HARDENING}")

  # /GUARD:CF must also be on the LINK line for the guard tables to be emitted.
  # /OPT:REF + /OPT:ICF drop unreferenced code and fold identical COMDATs: smaller
  # download, and less unused code sitting in the shipped image.
  set(DGB_MSVC_LINK_HARDENING "/GUARD:CF /OPT:REF /OPT:ICF")
  foreach(_t EXE SHARED MODULE)
    set(CMAKE_${_t}_LINKER_FLAGS_RELEASE
        "${CMAKE_${_t}_LINKER_FLAGS_RELEASE} ${DGB_MSVC_LINK_HARDENING}")
    set(CMAKE_${_t}_LINKER_FLAGS_RELWITHDEBINFO
        "${CMAKE_${_t}_LINKER_FLAGS_RELWITHDEBINFO} ${DGB_MSVC_LINK_HARDENING}")
  endforeach()
  # NOTE: whole-program optimisation (/GL + /LTCG) is deliberately NOT enabled here.
  # It is a real speed win, but it re-optimises across the entire binary, and the
  # unit tests do not exercise the sync path where that would matter most. It wants
  # its own release and a soak test rather than riding along with a security change.

  # Disable Windows features not needed.
  add_definitions(-D_WIN32_WINNT=0x0601)
  add_definitions(-DNOMINMAX)              # from asset_features: avoid Windows.h min/max macro clash with std::min/max
  add_definitions(-DWIN32_LEAN_AND_MEAN)   # from asset_features: trim unused Windows headers
else()
  set(CMAKE_CXX_STANDARD 11)
  SET(CMAKE_CXX_FLAGS "-std=c++11 -g -Wall")
endif()

# Set version number
# Upstream version. Deliberately held at 0.3.3 even though this carries the
# upstream asset_features (which upstream calls v1.0.0) and the new PSPs. This
# fork stays on the 0.3.3-win.<N> scheme by choice - the WIN_BUILD counter below
# is the real per-release version and just keeps incrementing. Not adopting a
# 1.0.0 label.
SET(MAJOR_VERSION 0)
SET(MINOR_VERSION 3)
SET(PATCH_VERSION 3)
SET(SO_VERSION    0)

# Windows port build number (increment for each Windows-specific release).
# This is the SOURCE OF TRUTH for the version — it is substituted into
# src/Version.h.in -> the generated (gitignored) src/Version.h. Bump this to
# match the win.<N> tag on every release; do NOT hand-edit the generated header.
SET(WIN_BUILD 133)
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
    enable_testing()   # so `ctest` can run the suites (registered in tests/CMakeLists.txt)
    ADD_SUBDIRECTORY(tests)
endif()
