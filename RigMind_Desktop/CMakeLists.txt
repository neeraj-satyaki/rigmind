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
    src/backend/password_hash.cpp
    src/backend/auth_service.cpp
    src/backend/alarm_recorder.cpp
    src/slm/slm_guard.cpp
    src/mcp/mcp_server.cpp
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
    pkg_check_modules(GSTAPP REQUIRED IMPORTED_TARGET gstreamer-app-1.0)
    # OpenCV (resize, Canny, findContours, approxPolyDP) for the platform mask overlay and mask outlines: scripts/fetch_opencv.ps1.
    set(OPENCV_DIR ${CMAKE_SOURCE_DIR}/third_party/opencv)
    file(GLOB OPENCV_LIB ${OPENCV_DIR}/lib/opencv_world*.lib)
    file(GLOB OPENCV_DLL ${OPENCV_DIR}/bin/opencv_world*.dll)
    if(NOT OPENCV_LIB OR NOT OPENCV_DLL)
        message(FATAL_ERROR "OpenCV is missing: run scripts/fetch_opencv.ps1")
    endif()
    add_library(rigmind_overlay STATIC src/pipeline/platform_overlay.cpp src/pipeline/overlay_draw.cpp)
    target_include_directories(rigmind_overlay PUBLIC ${CMAKE_SOURCE_DIR}/src ${OPENCV_DIR}/include)
    target_link_libraries(rigmind_overlay PUBLIC ${OPENCV_LIB})
    add_executable(platform_overlay_test tests/platform_overlay_test.cpp)
    target_link_libraries(platform_overlay_test PRIVATE rigmind_overlay)
    add_custom_command(TARGET platform_overlay_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OPENCV_DLL} $<TARGET_FILE_DIR:platform_overlay_test>)
    add_test(NAME platform_overlay COMMAND platform_overlay_test)
    add_executable(rigmind-pipeline src/pipeline/pipeline_main.cpp src/pipeline/inference.cpp)
    target_link_libraries(rigmind-pipeline PRIVATE rigmind_overlay)
    add_custom_command(TARGET rigmind-pipeline POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OPENCV_DLL} $<TARGET_FILE_DIR:rigmind-pipeline>)
    target_include_directories(rigmind-pipeline PRIVATE ${CMAKE_SOURCE_DIR}/third_party/json ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(rigmind-pipeline PRIVATE rigmind_backend)
    target_compile_definitions(rigmind-pipeline PRIVATE GST_USE_UNSTABLE_API)
    target_link_libraries(rigmind-pipeline PRIVATE PkgConfig::GST PkgConfig::GSTAPP)

    # ONNX Runtime for the PPE / IDSS models (not committed). scripts/fetch_onnxruntime.ps1 -Gpu gives the CUDA 11.8 / TensorRT 8.6
    # build (it also holds the CPU provider) and is preferred; scripts/fetch_onnxruntime.ps1 alone gives the CPU-only package.
    if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/onnxruntime-gpu/lib/onnxruntime.lib)
        set(ORT_DIR ${CMAKE_SOURCE_DIR}/third_party/onnxruntime-gpu)
    else()
        set(ORT_DIR ${CMAKE_SOURCE_DIR}/third_party/onnxruntime)
    endif()
    if(NOT EXISTS ${ORT_DIR}/lib/onnxruntime.lib)
        message(FATAL_ERROR "ONNX Runtime is missing: run scripts/fetch_onnxruntime.ps1 -Gpu")
    endif()
    target_include_directories(rigmind-pipeline PRIVATE ${ORT_DIR}/include)
    target_link_libraries(rigmind-pipeline PRIVATE ${ORT_DIR}/lib/onnxruntime.lib)
    file(GLOB ORT_DLLS ${ORT_DIR}/bin/*.dll)
    add_custom_command(TARGET rigmind-pipeline POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ORT_DLLS} $<TARGET_FILE_DIR:rigmind-pipeline>)

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

# Platform ROI, scale selection and detections -> EWS facts (header-only, no GStreamer or ONNX).
add_executable(roi_facts_test tests/roi_facts_test.cpp)
target_include_directories(roi_facts_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(roi_facts_test PRIVATE rigmind_backend)
add_test(NAME roi_facts COMMAND roi_facts_test)

# ByteTrack and the PPE compliance check (header-only).
add_executable(tracking_test tests/tracking_test.cpp)
target_include_directories(tracking_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tracking_test PRIVATE rigmind_backend)
add_test(NAME tracking COMMAND tracking_test)

# Scene memory for the summary prompt (centroid paths, mask outlines, class names).
add_executable(scene_test tests/scene_test.cpp)
target_include_directories(scene_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(scene_test PRIVATE rigmind_backend)
add_test(NAME scene COMMAND scene_test)

# Local accounts: password hashing and the sign-in service.
add_executable(auth_test tests/auth_test.cpp)
target_include_directories(auth_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(auth_test PRIVATE rigmind_backend)
add_test(NAME auth COMMAND auth_test)

# MCP module: protocol, tools, resources and the local HTTP transport.
if(WIN32)
    add_executable(mcp_test tests/mcp_test.cpp src/mcp/mcp_http.cpp)
    target_include_directories(mcp_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(mcp_test PRIVATE rigmind_backend ws2_32 bcrypt)
    add_test(NAME mcp COMMAND mcp_test)
endif()

if(WIN32)
    set(WEBVIEW2_DIR ${CMAKE_SOURCE_DIR}/third_party/webview2/build/native)

    add_executable(RigMind WIN32 src/main.cpp src/stream/pipeline_process.cpp src/slm/slm_client.cpp src/mcp/mcp_http.cpp resources/app.rc)
    target_include_directories(RigMind PRIVATE ${CMAKE_SOURCE_DIR}/resources ${WEBVIEW2_DIR}/include)
    target_link_libraries(RigMind PRIVATE
        rigmind_backend
        ${WEBVIEW2_DIR}/x64/WebView2LoaderStatic.lib
        version
        winhttp)

    # Run the build from the exe folder: <exe>\web must exist for the WebView2 host mapping.
    add_custom_command(TARGET RigMind POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/web $<TARGET_FILE_DIR:RigMind>/web)

    # Development runs find the models next to the exe, as in an installed app (copied only when newer).
    file(GLOB MODEL_FILES ${CMAKE_SOURCE_DIR}/models/*.onnx ${CMAKE_SOURCE_DIR}/models/inference.cfg)
    if(MODEL_FILES)
        add_custom_command(TARGET RigMind POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:RigMind>/models
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MODEL_FILES} $<TARGET_FILE_DIR:RigMind>/models)
    endif()

    install(TARGETS RigMind RUNTIME DESTINATION .)
    # BitNet runtime for plain-language descriptions: llama-server.exe, its DLLs and the model (staged by scripts\package_windows.ps1).
    if(EXISTS ${CMAKE_SOURCE_DIR}/slm-windows)
        install(DIRECTORY ${CMAKE_SOURCE_DIR}/slm-windows/ DESTINATION slm)
    endif()
    if(RIGMIND_PIPELINE)
        install(TARGETS rigmind-pipeline RUNTIME DESTINATION .)
        install(FILES ${ORT_DLLS} DESTINATION .)
        install(FILES ${OPENCV_DLL} DESTINATION .)
        install(TARGETS rigmindews LIBRARY DESTINATION . RUNTIME DESTINATION .)
    endif()
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/web DESTINATION .)
    install(FILES ${CMAKE_SOURCE_DIR}/../LICENSE DESTINATION . RENAME LICENSE.txt)

    # Detection models (Git LFS): PPE (YOLOv8n) and IDSS (YOLOv8m-seg). ONNX only; the .pt files are training weights.
    if(EXISTS ${CMAKE_SOURCE_DIR}/models/ppe.onnx AND EXISTS ${CMAKE_SOURCE_DIR}/models/idss_model.onnx)
        install(DIRECTORY ${CMAKE_SOURCE_DIR}/models/ DESTINATION models FILES_MATCHING PATTERN "*.onnx" PATTERN "inference.cfg")
    else()
        message(WARNING "models\\ppe.onnx or models\\idss_model.onnx is missing: run 'git lfs pull'. The installer will not carry the detection models.")
    endif()

    # Runtime the app needs on a clean machine (staged by scripts\stage_runtime.ps1): GStreamer with only the plugins
    # in use, the Visual C++ runtime next to the executables, and the WebView2 bootstrapper.
    if(EXISTS ${CMAKE_SOURCE_DIR}/runtime-windows/gst)
        install(DIRECTORY ${CMAKE_SOURCE_DIR}/runtime-windows/gst/ DESTINATION gst)
        install(DIRECTORY ${CMAKE_SOURCE_DIR}/runtime-windows/vcrt/ DESTINATION . FILES_MATCHING PATTERN "*.dll")
        install(DIRECTORY ${CMAKE_SOURCE_DIR}/runtime-windows/redist/ DESTINATION redist)
    else()
        message(WARNING "runtime-windows\ is missing: run scripts\stage_runtime.ps1 or the installer will not run on a clean machine")
    endif()

    # NVIDIA GPU runtime (TensorRT, CUDA, cuDNN) is part of the installer, beside the app in gpu\ (staged by
    # scripts\stage_gpu_runtime.ps1, ~2.5 GB). A build without it fails on purpose: switch it off only for development
    # builds that are never shipped (-DRIGMIND_BUNDLE_GPU=OFF).
    option(RIGMIND_BUNDLE_GPU "Bundle the NVIDIA GPU runtime (TensorRT, CUDA, cuDNN) in the installer" ON)
    if(RIGMIND_BUNDLE_GPU)
        if(EXISTS ${CMAKE_SOURCE_DIR}/runtime-windows/gpu/nvinfer.dll AND EXISTS ${CMAKE_SOURCE_DIR}/runtime-windows/gpu/cudnn64_8.dll)
            install(DIRECTORY ${CMAKE_SOURCE_DIR}/runtime-windows/gpu/ DESTINATION gpu)
        else()
            message(FATAL_ERROR "The GPU runtime is not staged (runtime-windows\\gpu is incomplete): run scripts\\stage_gpu_runtime.ps1, or configure with -DRIGMIND_BUNDLE_GPU=OFF for a development build.")
        endif()
    endif()

    # Installer: cpack -G NSIS (requires NSIS on PATH).
    set(CPACK_GENERATOR "NSIS")
    set(CPACK_PACKAGE_NAME "RigMind")
    set(CPACK_PACKAGE_VENDOR "Satyaki Solutions Private Limited")
    set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_SOURCE_DIR}/../LICENSE)
    set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "RigMind")
    set(CPACK_NSIS_DISPLAY_NAME "RigMind")
    set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "RigMind.exe")
    set(CPACK_NSIS_MUI_ICON ${CMAKE_SOURCE_DIR}/resources/rigmind.ico)
    set(CPACK_NSIS_MUI_UNIICON ${CMAKE_SOURCE_DIR}/resources/rigmind.ico)
    # Microsoft Edge WebView2 Runtime: preinstalled on Windows 11; install it from the bundled bootstrapper if missing.
    set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "
      ReadRegStr $0 HKLM 'SOFTWARE\\\\WOW6432Node\\\\Microsoft\\\\EdgeUpdate\\\\Clients\\\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}' 'pv'
      StrCmp $0 '' 0 +2
        ExecWait '\"$INSTDIR\\\\redist\\\\MicrosoftEdgeWebview2Setup.exe\" /silent /install'
      Nop
    ")

    # Sample (demonstration) data is optional: asked once, default No. Yes writes <install>\demo-data.flag, which the app reads at
    # start; No (or a silent install) removes it, so the app shows only real data. (No double quotes: CPack writes this verbatim.)
    set(_demo_nsis [=[
      ; ---- Sample data option
      Delete `$INSTDIR\demo-data.flag`
      IfSilent rm_demo_done 0
      MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 `Include sample (demonstration) data?$\r$\n$\r$\nYes: the app also shows example alerts, events, analytics and reports.$\r$\nNo: the app shows only real data from your cameras (recommended for live use).` IDNO rm_demo_done
      FileOpen $0 `$INSTDIR\demo-data.flag` w
      FileWrite $0 `sample data`
      FileClose $0
      rm_demo_done:
]=])
    string(REPLACE "\\" "\\\\" _demo_nsis "${_demo_nsis}")
    string(APPEND CPACK_NSIS_EXTRA_INSTALL_COMMANDS "\n${_demo_nsis}\n")
    set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS [=[Delete `$INSTDIR\\demo-data.flag`]=])

    # GPU runtime delivered by the installer itself: a single NSIS installer cannot carry ~1.7 GB of NVIDIA libraries, so it
    # downloads them in parts from public URLs, checks each part's SHA-256, and unpacks it into <install>\gpu.
    # No internet or a failed check is not fatal: RigMind is installed and runs on the CPU (slow), and says so.
    # Each pack is "url|sha256|marker.dll"; when the marker DLL is already in <install>\gpu the pack is skipped.
    set(RIGMIND_GPU_PACKS "" CACHE STRING "Semicolon-separated GPU runtime packs the installer downloads: url|sha256|marker.dll")
    if(NOT RIGMIND_BUNDLE_GPU AND RIGMIND_GPU_PACKS)
        set(_gpu_all "")
        list(LENGTH RIGMIND_GPU_PACKS _gpu_n)
        set(_gpu_i 0)
        foreach(_pack IN LISTS RIGMIND_GPU_PACKS)
            math(EXPR _gpu_i "${_gpu_i} + 1")
            string(REPLACE "|" ";" _f "${_pack}")
            list(GET _f 0 _url)
            list(GET _f 1 _sha)
            list(GET _f 2 _marker)
            set(_gpu_nsis [=[
      ; ---- NVIDIA GPU runtime, part @I@ of @N@: download, verify, unpack into $INSTDIR\gpu
      ; (no double quotes in this block: CPack writes it verbatim inside a double-quoted CMake string)
      ; Windows tools are called by absolute path ($SYSDIR): the installer must not depend on PATH, PowerShell modules or the caller's environment.
      IfFileExists `$INSTDIR\gpu\@MARKER@` rm_gpu_done_@I@ 0
      DetailPrint `Downloading the NVIDIA GPU runtime, part @I@ of @N@. This can take several minutes...`
      nsExec::ExecToLog `$SYSDIR\curl.exe -sS -L --fail --retry 5 --retry-delay 5 --connect-timeout 30 --speed-time 60 --speed-limit 10240 -C - -o @Q@$INSTDIR\gpu-part@I@.zip@Q@ @Q@@GPU_URL@@Q@`
      Pop $0
      StrCmp $0 `0` rm_gpu_verify_@I@ rm_gpu_fail_@I@
      rm_gpu_verify_@I@:
      DetailPrint `Verifying part @I@ of @N@...`
      nsExec::ExecToLog `$SYSDIR\cmd.exe /c @Q@@Q@$SYSDIR\certutil.exe@Q@ -hashfile @Q@$INSTDIR\gpu-part@I@.zip@Q@ SHA256 | @Q@$SYSDIR\findstr.exe@Q@ /i /c:@GPU_SHA@@Q@`
      Pop $0
      StrCmp $0 `0` rm_gpu_unpack_@I@ rm_gpu_badhash_@I@
      rm_gpu_unpack_@I@:
      DetailPrint `Installing part @I@ of @N@...`
      nsExec::ExecToLog `$SYSDIR\tar.exe -xf @Q@$INSTDIR\gpu-part@I@.zip@Q@ -C @Q@$INSTDIR@Q@`
      Pop $0
      Delete `$INSTDIR\gpu-part@I@.zip`
      StrCmp $0 `0` rm_gpu_done_@I@ rm_gpu_fail_@I@
      rm_gpu_badhash_@I@:
      Delete `$INSTDIR\gpu-part@I@.zip`
      DetailPrint `Part @I@ of the GPU runtime failed its integrity check and was removed.`
      IfSilent rm_gpu_done_@I@ 0
      MessageBox MB_OK|MB_ICONEXCLAMATION `Part @I@ of the GPU runtime failed its integrity check and was discarded.$\r$\nRigMind will run on the CPU (slow) until it is installed. Run this installer again to retry.`
      Goto rm_gpu_done_@I@
      rm_gpu_fail_@I@:
      Delete `$INSTDIR\gpu-part@I@.zip`
      DetailPrint `Part @I@ of the GPU runtime could not be downloaded or installed.`
      IfSilent rm_gpu_done_@I@ 0
      MessageBox MB_OK|MB_ICONEXCLAMATION `Part @I@ of the NVIDIA GPU runtime could not be downloaded (no internet, or the download was blocked).$\r$\nRigMind is installed and will run on the CPU, which is slow. Run this installer again with internet access to add the GPU runtime.`
      rm_gpu_done_@I@:
]=])
            string(REPLACE "@I@" "${_gpu_i}" _gpu_nsis "${_gpu_nsis}")
            string(REPLACE "@N@" "${_gpu_n}" _gpu_nsis "${_gpu_nsis}")
            string(REPLACE "@MARKER@" "${_marker}" _gpu_nsis "${_gpu_nsis}")
            string(REPLACE "@GPU_URL@" "${_url}" _gpu_nsis "${_gpu_nsis}")
            string(REPLACE "@GPU_SHA@" "${_sha}" _gpu_nsis "${_gpu_nsis}")
            string(APPEND _gpu_all "${_gpu_nsis}")
        endforeach()
        # CPack writes this text verbatim into a double-quoted CMake string: every backslash must be doubled, and an NSIS
        # escaped quote ($\") must reach the file as $\\\" so that it parses back to $\".
        string(REPLACE "\\" "\\\\" _gpu_all "${_gpu_all}")
        string(REPLACE "@Q@" [=[$\\\"]=] _gpu_all "${_gpu_all}")
        string(APPEND CPACK_NSIS_EXTRA_INSTALL_COMMANDS "\n${_gpu_all}\n")
        string(APPEND CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "\n" [=[RMDir /r `$INSTDIR\\gpu`]=])
    endif()

    # BitNet plain-language summaries: the 2B model (1.19 GB) is downloaded at install time in two parts (like the GPU runtime),
    # SHA-256 checked, joined into <install>\slm\ggml-model-i2_s.gguf. Asked once (default Yes, silent installs download).
    # Without the model the app still runs: summaries use the template sentence. Each pack is "url|sha256".
    set(RIGMIND_SLM_PACKS "" CACHE STRING "Semicolon-separated BitNet model parts the installer downloads: url|sha256")
    if(RIGMIND_SLM_PACKS)
        set(_slm_all "")
        list(LENGTH RIGMIND_SLM_PACKS _slm_n)
        set(_slm_i 0)
        foreach(_pack IN LISTS RIGMIND_SLM_PACKS)
            math(EXPR _slm_i "${_slm_i} + 1")
            string(REPLACE "|" ";" _f "${_pack}")
            list(GET _f 0 _url)
            list(GET _f 1 _sha)
            set(_slm_nsis [=[
      ; ---- BitNet model, part @I@ of @N@: download, verify, unpack into $INSTDIR\slm (no double quotes in this block)
      IfFileExists `$INSTDIR\slm\ggml-model-i2_s.gguf` rm_slm_done_@I@ 0
      StrCmp $R8 `0` rm_slm_done_@I@ 0
      DetailPrint `Downloading the BitNet summary model, part @I@ of @N@. This can take several minutes...`
      nsExec::ExecToLog `$SYSDIR\curl.exe -sS -L --fail --retry 5 --retry-delay 5 --connect-timeout 30 --speed-time 60 --speed-limit 10240 -C - -o @Q@$INSTDIR\slm-part@I@.zip@Q@ @Q@@SLM_URL@@Q@`
      Pop $0
      StrCmp $0 `0` rm_slm_verify_@I@ rm_slm_fail_@I@
      rm_slm_verify_@I@:
      DetailPrint `Verifying part @I@ of @N@...`
      nsExec::ExecToLog `$SYSDIR\cmd.exe /c @Q@@Q@$SYSDIR\certutil.exe@Q@ -hashfile @Q@$INSTDIR\slm-part@I@.zip@Q@ SHA256 | @Q@$SYSDIR\findstr.exe@Q@ /i /c:@SLM_SHA@@Q@`
      Pop $0
      StrCmp $0 `0` rm_slm_unpack_@I@ rm_slm_fail_@I@
      rm_slm_unpack_@I@:
      DetailPrint `Installing part @I@ of @N@...`
      nsExec::ExecToLog `$SYSDIR\tar.exe -xf @Q@$INSTDIR\slm-part@I@.zip@Q@ -C @Q@$INSTDIR@Q@`
      Pop $0
      Delete `$INSTDIR\slm-part@I@.zip`
      StrCmp $0 `0` rm_slm_done_@I@ rm_slm_fail_@I@
      rm_slm_fail_@I@:
      Delete `$INSTDIR\slm-part@I@.zip`
      StrCpy $R8 `0`
      DetailPrint `Part @I@ of the BitNet model could not be downloaded or failed its check.`
      IfSilent rm_slm_done_@I@ 0
      MessageBox MB_OK|MB_ICONEXCLAMATION `The BitNet summary model could not be downloaded (no internet, blocked, or failed its integrity check).$\r$\nRigMind works without it and shows the standard sentence. Run this installer again with internet access to add it.`
      rm_slm_done_@I@:
]=])
            string(REPLACE "@I@" "${_slm_i}" _slm_nsis "${_slm_nsis}")
            string(REPLACE "@N@" "${_slm_n}" _slm_nsis "${_slm_nsis}")
            string(REPLACE "@SLM_URL@" "${_url}" _slm_nsis "${_slm_nsis}")
            string(REPLACE "@SLM_SHA@" "${_sha}" _slm_nsis "${_slm_nsis}")
            string(APPEND _slm_all "${_slm_nsis}")
        endforeach()
        set(_slm_head [=[
      ; ---- BitNet option
      StrCpy $R8 `1`
      IfFileExists `$INSTDIR\slm\ggml-model-i2_s.gguf` rm_slm_asked 0
      IfSilent rm_slm_asked 0
      MessageBox MB_YESNO|MB_ICONQUESTION `Add plain-language alert summaries (BitNet)?$\r$\n$\r$\nYes: downloads a 1.2 GB model now. Summaries are written on this PC; it uses about 1.2 GB of memory while RigMind runs.$\r$\nNo: alerts show the standard sentence. You can run this installer again later to add it.` IDYES rm_slm_asked
      StrCpy $R8 `0`
      rm_slm_asked:
]=])
        set(_slm_tail [=[
      ; join the parts
      IfFileExists `$INSTDIR\slm\ggml-model-i2_s.gguf` rm_slm_joined 0
      IfFileExists `$INSTDIR\slm\ggml-model-i2_s.gguf.part1` 0 rm_slm_joined
      IfFileExists `$INSTDIR\slm\ggml-model-i2_s.gguf.part2` 0 rm_slm_joined
      DetailPrint `Joining the BitNet model...`
      nsExec::ExecToLog `$SYSDIR\cmd.exe /c copy /b @Q@$INSTDIR\slm\ggml-model-i2_s.gguf.part1@Q@+@Q@$INSTDIR\slm\ggml-model-i2_s.gguf.part2@Q@ @Q@$INSTDIR\slm\ggml-model-i2_s.gguf@Q@`
      Pop $0
      rm_slm_joined:
      Delete `$INSTDIR\slm\ggml-model-i2_s.gguf.part1`
      Delete `$INSTDIR\slm\ggml-model-i2_s.gguf.part2`
]=])
        set(_slm_all "${_slm_head}${_slm_all}${_slm_tail}")
        string(REPLACE "\\" "\\\\" _slm_all "${_slm_all}")
        string(REPLACE "@Q@" [=[$\\\"]=] _slm_all "${_slm_all}")
        string(APPEND CPACK_NSIS_EXTRA_INSTALL_COMMANDS "
${_slm_all}
")
        string(APPEND CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "
" [=[RMDir /r `$INSTDIR\\slm`]=])
    endif()

    set(CPACK_NSIS_CREATE_ICONS_EXTRA "CreateShortCut '$SMPROGRAMS\\\\RigMind.lnk' '$INSTDIR\\\\RigMind.exe'\n    CreateShortCut '$DESKTOP\\\\RigMind.lnk' '$INSTDIR\\\\RigMind.exe'")
    set(CPACK_NSIS_DELETE_ICONS_EXTRA "Delete '$SMPROGRAMS\\\\RigMind.lnk'\n    Delete '$DESKTOP\\\\RigMind.lnk'")
    include(CPack)
endif()
