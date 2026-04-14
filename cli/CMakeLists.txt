cmake_minimum_required(VERSION 3.21)
project(digiasset_core-cli)

set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake/modules)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/")
set(CMAKE_CXX_STANDARD 11)
FIND_PACKAGE(JSONCPP REQUIRED)
FIND_PACKAGE(JSONRPCCPP REQUIRED)
find_package(CURL REQUIRED)

add_library(llibcurl STATIC IMPORTED)


set(HEADER_FILES
        ../src/Config.h
        ../src/DigiByteCore.cpp
        ../src/utils.cpp
        )

set(SOURCE_FILES
        ../src/Config.cpp
        ../src/DigiByteCore.cpp
        )

add_executable(digiasset_core-cli main.cpp ${SOURCE_FILES} ${HEADER_FILES})




target_link_libraries(digiasset_core-cli PRIVATE CURL::libcurl)
target_link_libraries(digiasset_core-cli PRIVATE ssl)
target_link_libraries(digiasset_core-cli PRIVATE pthread)

find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)
target_include_directories(digiasset_core-cli PRIVATE ${JSONCPP_INCLUDE_DIRS})
target_link_libraries(digiasset_core-cli PRIVATE ${JSONCPP_LIBRARIES})
target_link_libraries(digiasset_core-cli PRIVATE jsonrpccpp-common)
target_link_libraries(digiasset_core-cli PRIVATE jsonrpccpp-client)


install(TARGETS digiasset_core-cli DESTINATION bin)