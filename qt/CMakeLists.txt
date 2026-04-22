cmake_minimum_required(VERSION 3.21)
project(digiasset_core-qt LANGUAGES CXX)

# --- C++ standard ---
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Enable automatic Qt MOC, UIC, and RCC ---
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# --- Extra module paths, if needed ---
set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake/modules)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/")

# --- Find external dependencies ---
find_package(JSONCPP REQUIRED)
find_package(JSONRPCCPP REQUIRED)
find_package(CURL REQUIRED)

# --- Find Qt libraries ---
# Adjust the COMPONENTS list as necessary. Commonly used are:
# Core, Gui, Widgets, Network, Charts, etc.
find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets Charts)

# If you’re on Qt6, you might use:
# find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Charts)

# --- Source files ---
set(HEADER_FILES
        PlotManager.h
        RPCLoader.h
        tabs/SyncTab.h
        ../src/Config.h
        ../src/DigiByteCore.h
        ../src/DigiAssetConstants.h
        ../src/utils.h
        )

set(SOURCE_FILES
        PlotManager.cpp
        RPCLoader.cpp
        tabs/SyncTab.cpp
        ../src/Config.cpp
        ../src/DigiByteCore.cpp
        ../src/utils.cpp
        )

# --- Create the executable ---
add_executable(digiasset_core-qt
        main.cpp
        ${SOURCE_FILES}
        ${HEADER_FILES}
        )

# --- Add application icon AFTER target creation ---
set(APP_ICON_RESOURCE ../bin/images/app_icon.rc)
target_sources(digiasset_core-qt PRIVATE ${APP_ICON_RESOURCE})

# --- Link system libraries (curl, ssl, crypto, pthread) ---
target_link_libraries(digiasset_core-qt PRIVATE CURL::libcurl)
target_link_libraries(digiasset_core-qt PRIVATE ssl)
target_link_libraries(digiasset_core-qt PRIVATE crypto)
target_link_libraries(digiasset_core-qt PRIVATE pthread)

# --- Link JSON libraries ---
target_link_libraries(digiasset_core-qt PRIVATE /lib/x86_64-linux-gnu/libjsoncpp.so)
target_link_libraries(digiasset_core-qt PRIVATE jsonrpccpp-common)
target_link_libraries(digiasset_core-qt PRIVATE jsonrpccpp-client)

# --- Link SQLite (if needed) ---
# target_link_libraries(digiasset_core-qt PUBLIC sqlite3)
# If you have a static library for sqlite3, set it up similarly.

# --- Link Qt libraries ---
# If you’re using Qt5:
target_link_libraries(digiasset_core-qt
        PRIVATE
        Qt5::Core
        Qt5::Gui
        Qt5::Widgets
        Qt5::Charts
        )

# For Qt6, it would look like:
# target_link_libraries(digiasset_core-qt
#     PRIVATE
#         Qt6::Core
#         Qt6::Gui
#         Qt6::Widgets
#         Qt6::Charts
# )



# --- Installation rule ---
install(TARGETS digiasset_core-qt DESTINATION bin)