cmake_minimum_required(VERSION 3.20)
project(RigMindDesktop VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Backend core: alarm state machine, SQLite event store, JSON bridge. Builds on any compiler.
add_library(rigmind_backend STATIC
    src/backend/alarm_state.cpp
    src/backend/event_store.cpp
    src/backend/bridge.cpp
    src/backend/audit_log.cpp
    src/slm/slm_guard.cpp
    third_party/sqlite3/sqlite3.c)
target_include_directories(rigmind_backend PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/json
    ${CMAKE_SOURCE_DIR}/third_party/sqlite3)
target_compile_definitions(rigmind_backend PUBLIC SQLITE_THREADSAFE=1)
find_package(Threads REQUIRED)
target_link_libraries(rigmind_backend PUBLIC Threads::Threads)
if(MSVC)
    target_compile_options(rigmind_backend PRIVATE /W4)
else()
    target_compile_options(rigmind_backend PRIVATE -Wall -Wextra)
endif()

# Linux host backend: same C++ store and bridge, driven over stdin/stdout by host/host.py.
add_executable(rigmind-bridge-cli src/host/bridge_cli.cpp)
target_link_libraries(rigmind-bridge-cli PRIVATE rigmind_backend)

# GStreamer WebRTC child process (RTSP -> RTP passthrough -> webrtcbin). Needs GStreamer dev packages
# with pkg-config (Linux: gstreamer1.0-dev, gstreamer1.0-plugins-bad, gstreamer1.0-nice).
option(RIGMIND_PIPELINE "Build rigmind-pipeline (GStreamer WebRTC)" ON)
if(RIGMIND_PIPELINE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GST REQUIRED IMPORTED_TARGET gstreamer-1.0 gstreamer-sdp-1.0 gstreamer-webrtc-1.0)
    add_executable(rigmind-pipeline src/pipeline/pipeline_main.cpp)
    target_include_directories(rigmind-pipeline PRIVATE ${CMAKE_SOURCE_DIR}/third_party/json ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(rigmind-pipeline PRIVATE rigmind_backend)
    target_compile_definitions(rigmind-pipeline PRIVATE GST_USE_UNSTABLE_API)
    target_link_libraries(rigmind-pipeline PRIVATE PkgConfig::GST)

    # rigmindews: EWS alarm element, loaded as a GStreamer plugin module (librigmindews.so).
    pkg_check_modules(GSTBASE REQUIRED IMPORTED_TARGET gstreamer-base-1.0 gstreamer-app-1.0)
    add_library(rigmindews MODULE src/pipeline/plugins/ews/gstrigmindews.cpp src/backend/alarm_state.cpp)
    target_include_directories(rigmindews PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(rigmindews PRIVATE PkgConfig::GSTBASE)
    set_target_properties(rigmindews PROPERTIES PREFIX "lib")
    add_executable(ews_plugin_test tests/ews/ews_plugin_test.cpp)
    target_include_directories(ews_plugin_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(ews_plugin_test PRIVATE PkgConfig::GSTBASE rigmind_backend)
endif()

enable_testing()
if(RIGMIND_PIPELINE)
    add_test(NAME ews_plugin COMMAND ews_plugin_test $<TARGET_FILE:rigmindews>)
endif()
add_executable(backend_test tests/backend_test.cpp)
target_link_libraries(backend_test PRIVATE rigmind_backend)
add_test(NAME backend COMMAND backend_test)

if(WIN32)
    set(WEBVIEW2_DIR ${CMAKE_SOURCE_DIR}/third_party/webview2/build/native)

    add_executable(RigMind WIN32 src/main.cpp src/stream/pipeline_process.cpp src/slm/slm_client.cpp resources/app.rc)
    target_include_directories(RigMind PRIVATE ${CMAKE_SOURCE_DIR}/resources ${WEBVIEW2_DIR}/include)
    target_link_libraries(RigMind PRIVATE
        rigmind_backend
        ${WEBVIEW2_DIR}/x64/WebView2LoaderStatic.lib
        version
        winhttp)

    # Run the build from the exe folder: <exe>\web must exist for the WebView2 host mapping.
    add_custom_command(TARGET RigMind POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/web $<TARGET_FILE_DIR:RigMind>/web)

    install(TARGETS RigMind RUNTIME DESTINATION .)
    # BitNet runtime for plain-language descriptions: llama-server.exe, its DLLs and the model (staged by scripts\package_windows.ps1).
    if(EXISTS ${CMAKE_SOURCE_DIR}/slm-windows)
        install(DIRECTORY ${CMAKE_SOURCE_DIR}/slm-windows/ DESTINATION slm)
    endif()
    if(RIGMIND_PIPELINE)
        install(TARGETS rigmind-pipeline RUNTIME DESTINATION .)
    endif()
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/web DESTINATION .)

    # Installer: cpack -G NSIS (requires NSIS on PATH).
    set(CPACK_GENERATOR "NSIS")
    set(CPACK_PACKAGE_NAME "RigMind")
    set(CPACK_PACKAGE_VENDOR "Satyaki Solutions Pvt Ltd")
    set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "RigMind")
    set(CPACK_NSIS_DISPLAY_NAME "RigMind")
    set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "RigMind.exe")
    set(CPACK_NSIS_MUI_ICON ${CMAKE_SOURCE_DIR}/resources/rigmind.ico)
    set(CPACK_NSIS_MUI_UNIICON ${CMAKE_SOURCE_DIR}/resources/rigmind.ico)
    set(CPACK_NSIS_CREATE_ICONS_EXTRA "CreateShortCut '$SMPROGRAMS\\\\RigMind.lnk' '$INSTDIR\\\\RigMind.exe'")
    set(CPACK_NSIS_DELETE_ICONS_EXTRA "Delete '$SMPROGRAMS\\\\RigMind.lnk'")
    include(CPack)
endif()
