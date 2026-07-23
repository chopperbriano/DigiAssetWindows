# 'Google_test' is the subproject name
project(Google_tests)

# 'lib' is the folder with Google Test sources
add_subdirectory(lib/googletest)
include_directories(${gtest_SOURCE_DIR}/include ${gtest_SOURCE_DIR})

set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake/modules)
if(MSVC)
  set(CMAKE_CXX_STANDARD 20)
else()
  set(CMAKE_CXX_STANDARD 11)
endif()

# Tests include src/RPC/Server.h which in turn pulls in real Boost.Asio
# headers (executor_work_guard.hpp, etc.). The src/ target adds this
# include path, but include_directories() is scoped to its own
# subdirectory in cmake -- sibling subdirectories don't inherit. Add
# the same path here so the test executables can find Boost headers.
# Without this, every test target fails with
#   C1083: Cannot open include file: 'boost/asio/executor_work_guard.hpp'
if(WIN32 AND MSVC)
    include_directories(BEFORE "${CMAKE_SOURCE_DIR}/packages/boost.1.82.0/lib/native/include")
endif()

# Ensure testFiles directory exists (not tracked by git, needed by tests at runtime)
file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/tests/testFiles)  # googletest 1.14+ requires C++14 minimum; C++17 breaks system libjsonrpccpp headers

# Upstream requires OpenSSL/Threads for its cross-platform test linking; this
# Windows fork links winhttp + the vendored static libs instead, so only require
# them off-MSVC (they're not available on the Windows build).
if(NOT MSVC)
    find_package(OpenSSL REQUIRED)
    find_package(Threads REQUIRED)
endif()
find_package(jsoncpp CONFIG QUIET)
find_package(JSONCPP QUIET)

set(TESTS
        AppMainTest.cpp
        BitIO.cpp
        DigiByteDomainTest.cpp
        LocalPoolTest.cpp
        DatabaseChain.cpp
        DigiByteCore.cpp
        IPFS.cpp
        Base58Tests.cpp
        DigiAssetTest.cpp
        DigiAssetRulesTest.cpp
        DigiAssetTransactionTest.cpp
        DigiByteTransactionBuilderTest.cpp
        DigiAssetRulesEncodeTest.cpp
        PermanentStoragePoolTest.cpp
        PermanentStoragePoolListTest.cpp
        ConfigTest.cpp
        KYCTest.cpp
        BlobTest.cpp
        UtilsTest.cpp
        SHA256Test.cpp
        DigiAssetTypesTest.cpp
        UniqueTaskQueueTest.cpp
        ThreadedTest.cpp
        LogTest.cpp
        InstanceLockTest.cpp
        RPC_ResponseTest.cpp
        RPC_CacheTest.cpp
        RPC_ServerTest.cpp
        DigiByteExceptionTest.cpp
        Database_StatementTest.cpp
        DatabaseTest.cpp
        ChainAnalyzerTest.cpp
        RPC_MethodListTest.cpp
        CurlHandlerTest.cpp
        PermanentStoragePoolMetaProcessorTest.cpp
        TestHelpers.cpp TestHelpers.h RPCMethods.cpp RPCMethods.h)

#load RPC tests
file(GLOB RPC_METHOD_TEST_SOURCES RPC_Methods/*.cpp)
foreach(SOURCE_FILE ${RPC_METHOD_TEST_SOURCES})
    list(APPEND TESTS ${SOURCE_FILE})
endforeach()

# 'Google_Tests_run' is the target name
# 'test1.cpp tests2.cpp' are source files with tests
include_directories("${CMAKE_SOURCE_DIR}/jsoncpp/install/include")
include_directories("${CMAKE_SOURCE_DIR}/libjson-rpc-cpp/install/include")
include_directories("${CMAKE_SOURCE_DIR}/src")

add_executable(Google_Tests_run ${TESTS})   #digibyteapi
target_link_libraries(Google_Tests_run digiasset_core_lib)
target_link_libraries(Google_Tests_run gtest gtest_main)

if(WIN32 AND MSVC)
  # Disable C++20 char8_t breaking change (u8"" literals stay as const char[])
  target_compile_options(Google_Tests_run PRIVATE /Zc:char8_t-)
  target_link_libraries(Google_Tests_run "${CMAKE_SOURCE_DIR}/jsoncpp/install/lib/jsoncpp_static.lib")
  target_link_libraries(Google_Tests_run "${CMAKE_SOURCE_DIR}/libjson-rpc-cpp/install/lib/jsonrpccpp-common.lib")
  target_link_libraries(Google_Tests_run "${CMAKE_SOURCE_DIR}/libjson-rpc-cpp/install/lib/jsonrpccpp-client.lib")
  target_link_libraries(Google_Tests_run winhttp)
else()
  target_link_libraries(Google_Tests_run sqlite3 curl)
  target_link_libraries(Google_Tests_run ssl crypto pthread)
  target_link_libraries(Google_Tests_run /lib/x86_64-linux-gnu/libjsoncpp.so jsonrpccpp-common jsonrpccpp-client)
endif()

# ---- Unit Tests (no external services required) ----
set(UNIT_TESTS
    BlobUnitTests.cpp
    BlobEdgeCaseTests.cpp
    ConfigUnitTests.cpp
    ConfigEdgeCaseTests.cpp
    DatabaseUnitTests.cpp
    DatabaseFunctionalTests.cpp
    VersionTests.cpp
    Base58Tests.cpp
    ChainSplitFixtureTest.cpp)

add_executable(Unit_Tests_run ${UNIT_TESTS})
target_link_libraries(Unit_Tests_run digiasset_core_lib)
target_link_libraries(Unit_Tests_run gtest gtest_main)

# Absolute path to the test fixtures so ChainSplitFixtureTest resolves them
# regardless of the working directory it is launched from (ctest, VS, or a raw
# run out of build/tests/Release all differ). CWD-relative lookup stays as a
# fallback inside the test.
target_compile_definitions(Unit_Tests_run PRIVATE
    CHAIN_SPLIT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/chain-split-2026")

if(WIN32 AND MSVC)
  target_compile_options(Unit_Tests_run PRIVATE /Zc:char8_t-)
  target_link_libraries(Unit_Tests_run "${CMAKE_SOURCE_DIR}/jsoncpp/install/lib/jsoncpp_static.lib")
  target_link_libraries(Unit_Tests_run "${CMAKE_SOURCE_DIR}/libjson-rpc-cpp/install/lib/jsonrpccpp-common.lib")
  target_link_libraries(Unit_Tests_run "${CMAKE_SOURCE_DIR}/libjson-rpc-cpp/install/lib/jsonrpccpp-client.lib")
  target_link_libraries(Unit_Tests_run winhttp)
else()
  target_link_libraries(Unit_Tests_run sqlite3 curl)
  target_link_libraries(Unit_Tests_run ssl crypto pthread)
  target_link_libraries(Unit_Tests_run /lib/x86_64-linux-gnu/libjsoncpp.so jsonrpccpp-common jsonrpccpp-client)
endif()
