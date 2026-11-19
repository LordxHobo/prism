# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

cmake_minimum_required(VERSION 3.21)
project(Prism VERSION 0.9.2 LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "Prism currently builds for Windows only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(Prism WIN32
    src/main.cpp
    src/BrowserWindow.cpp
    src/BrowserWindow.h
    src/Dialogs.cpp
    src/Dialogs.h
    src/DnrFilters.cpp
    src/DnrFilters.h
    src/Engine.cpp
    src/Engine.h
    src/ExtensionHost.cpp
    src/ExtensionServer.cpp
    src/ExtensionServer.h
    src/Extensions.cpp
    src/Extensions.h
    src/ExtensionsInternal.h
    src/Blocker.cpp
    src/Bookmarks.cpp
    src/Bookmarks.h
    src/Blocker.h
    src/BlockerProxy.cpp
    src/History.cpp
    src/History.h
    src/Http.cpp
    src/Icons.cpp
    src/Icons.h
    src/Http.h
    src/Json.cpp
    src/Json.h
    src/Permissions.cpp
    src/Permissions.h
    src/Privacy.cpp
    src/Privacy.h
    src/RequestFilter.cpp
    src/RequestFilter.h
    src/Settings.cpp
    src/Settings.h
    src/Tabs.h
    src/UserCSS.cpp
    src/UserCSS.h
    src/Shields.cpp
    src/Shields.h
    src/ShieldsPanel.cpp
    src/ShieldsPanel.h
    src/Suggestions.cpp
    src/Suggestions.h
    src/Util.cpp
    src/Util.h
    src/Version.h
    src/WebKitApi.cpp
    src/WebKitApi.h
    res/Prism.rc
)

target_compile_definitions(Prism PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)

if(MSVC)
    target_compile_options(Prism PRIVATE /W4 /utf-8 /permissive- /Zc:__cplusplus)
    # The manifest is embedded by res/Prism.rc.
    target_link_options(Prism PRIVATE /MANIFEST:NO)
else()
    target_compile_options(Prism PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()

# Prism loads WebKit at runtime (see src/WebKitApi.h), so it only links
# against Windows' own libraries.
target_link_libraries(Prism PRIVATE user32 gdi32 shell32 shlwapi ole32 uuid dwmapi advapi32 winhttp ws2_32)

# Everything needed to run Prism ends up in dist/: Prism.exe, the ui folder,
# and the WebKit runtime (put there by scripts/fetch-webkit.ps1).
# The generator expression stops Visual Studio from adding a Release/ subfolder.
set(PRISM_DIST_DIR "${CMAKE_SOURCE_DIR}/dist")
set_target_properties(Prism PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<1:${PRISM_DIST_DIR}>")

add_custom_command(TARGET Prism POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/ui" "${PRISM_DIST_DIR}/ui"
    COMMENT "Copying Prism's interface files to dist/ui"
)

# PrismBundle.dll: loaded by WebKit into its web content processes, where it
# asks Prism about every request pages make (see bundle/PrismBundle.cpp).
add_library(PrismBundle SHARED
    bundle/PrismBundle.cpp
    bundle/BundleApi.h
    bundle/RequestKind.h
)
target_compile_definitions(PrismBundle PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
if(MSVC)
    target_compile_options(PrismBundle PRIVATE /W4 /utf-8 /permissive- /Zc:__cplusplus)
else()
    target_compile_options(PrismBundle PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()
set_target_properties(PrismBundle PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<1:${PRISM_DIST_DIR}>"
    PREFIX ""
)
target_include_directories(Prism PRIVATE "${CMAKE_SOURCE_DIR}/bundle")
add_dependencies(Prism PrismBundle)
