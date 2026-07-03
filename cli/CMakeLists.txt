cmake_minimum_required(VERSION 3.21)
project(DigiAssetWindows-cli)

set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake/modules)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/")
set(CMAKE_CXX_STANDARD 11)

include_directories("${CMAKE_SOURCE_DIR}/jsoncpp/install/include")
include_directories("${CMAKE_SOURCE_DIR}/libjson-rpc-cpp/install/include")
include_directories("${CMAKE_SOURCE_DIR}/src")

set(HEADER_FILES
        ../src/Config.h
        ../src/DigiByteCore.h
        )

set(SOURCE_FILES
        ../src/Config.cpp
        ../src/DigiByteCore.cpp
        )

if(WIN32 AND MSVC)
  # On Windows, use the WinHTTP curl stubs and local jsonrpccpp sources
  list(APPEND SOURCE_FILES
        ../src/curl_stubs.cpp
        ../src/jsonrpccpp/common/exception.cpp
        ../src/jsonrpccpp/common/errors.cpp
        ../src/jsonrpccpp/client/client.cpp
        ../src/jsonrpccpp/client/connectors/httpclient.cpp
        )

  add_executable(DigiAssetWindows-cli main.cpp ${SOURCE_FILES} ${HEADER_FILES})

  target_link_libraries(DigiAssetWindows-cli PRIVATE "${CMAKE_SOURCE_DIR}/jsoncpp/install/lib/jsoncpp_static.lib")
  target_link_libraries(DigiAssetWindows-cli PRIVATE "${CMAKE_SOURCE_DIR}/libjson-rpc-cpp/install/lib/jsonrpccpp-common.lib")
  target_link_libraries(DigiAssetWindows-cli PRIVATE "${CMAKE_SOURCE_DIR}/libjson-rpc-cpp/install/lib/jsonrpccpp-client.lib")
  target_link_libraries(DigiAssetWindows-cli PRIVATE winhttp)
else()
  FIND_PACKAGE(JSONCPP REQUIRED)
  FIND_PACKAGE(JSONRPCCPP REQUIRED)
  find_package(CURL REQUIRED)

  add_executable(DigiAssetWindows-cli main.cpp ${SOURCE_FILES} ${HEADER_FILES})

  target_link_libraries(DigiAssetWindows-cli PRIVATE CURL::libcurl)
  target_link_libraries(DigiAssetWindows-cli PRIVATE ssl)
  target_link_libraries(DigiAssetWindows-cli PRIVATE pthread)
  target_link_libraries(DigiAssetWindows-cli PRIVATE /lib/x86_64-linux-gnu/libjsoncpp.so)
  target_link_libraries(DigiAssetWindows-cli PRIVATE jsonrpccpp-common)
  target_link_libraries(DigiAssetWindows-cli PRIVATE jsonrpccpp-client)
endif()

install(TARGETS DigiAssetWindows-cli DESTINATION bin)
