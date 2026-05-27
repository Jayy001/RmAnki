cmake_minimum_required(VERSION 3.18)
project(anki VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Bundle Qt libs next to the binary under backend/libs/
set(CMAKE_INSTALL_RPATH "$ORIGIN/libs")
set(CMAKE_BUILD_RPATH   "$ORIGIN/libs")
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)

# Qt6 — Gui (QPainter, QImage) + Network (QNetworkAccessManager)
find_package(Qt6 REQUIRED COMPONENTS Gui Network)

qt_add_executable(entry
    src/main.cpp
    src/ankiclient.cpp
    src/ankiclient.h
    src/common.h
    src/qtfb-client.cpp
    src/qtfb-client.h
    src/htmlview.cpp
    src/htmlview.h
    src/penthread.cpp
    src/penthread.h
    src/renderer.cpp
    src/renderer.h
)

target_include_directories(entry PRIVATE src)

target_link_libraries(entry
    PRIVATE
        Qt6::Gui
        Qt6::Network
)

# Install — AppLoad expects the binary at backend/entry (per external.manifest.json)
install(TARGETS entry DESTINATION backend)
