cmake_minimum_required(VERSION 3.21)
project(DigiAssetWindows-cli)

set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake/modules)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/")
if(MSVC)
  set(CMAKE_CXX_STANDARD 20)   # match the lib (upstream designated initializers)
else()
  set(CMAKE_CXX_STANDARD 11)
endif()

include_directories("${CMAKE_SOURCE_DIR}/jsoncpp/install/include")
include_directories("${CMAKE_SOURCE_DIR}/libjson-rpc-cpp/install/include")
include_directories("${CMAKE_SOURCE_DIR}/src")

set(HEADER_FILES
        ../src/Config.h
        ../src/DigiByteCore.h
        ../src/DigiDollar.h
        ../src/Blob.h
        ../src/utils.h
        )

#DigiByteCore.cpp classifies the DigiDollar oracle commitment in coinbase outputs, so it needs
#DigiDollar.cpp, which in turn needs Blob.cpp for its hex decoding.  Keeping that check in one
#place is worth the two extra files here - duplicating the script format would be worse.
set(SOURCE_FILES
        ../src/Config.cpp
        ../src/DigiByteCore.cpp
        ../src/DigiDollar.cpp
        ../src/Blob.cpp
        ../src/utils.cpp
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
