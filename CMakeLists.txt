cmake_minimum_required(VERSION 3.21)
project(KuangFrameExtrapolation LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")   # static CRT: nothing extra to install

set(DIST ${CMAKE_SOURCE_DIR}/dist)

# --- MinHook ----------------------------------------------------------------
add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC third_party/minhook/include)

# --- kfe.dll (the mod) ---------------------------------------------------------
add_library(kfe SHARED mod/kfe.cpp)
target_link_libraries(kfe PRIVATE minhook)
set_target_properties(kfe PROPERTIES OUTPUT_NAME kfe)

# --- winmm.dll (loader / proxy) ------------------------------------------------
add_library(winmm_proxy SHARED proxy/winmm_proxy.cpp)
set_target_properties(winmm_proxy PROPERTIES OUTPUT_NAME winmm)

# --- the app -----------------------------------------------------------------
add_executable(app WIN32
    app/app.cpp
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp)
target_include_directories(app PRIVATE third_party/imgui)
target_link_libraries(app PRIVATE d3d11 d3dcompiler dxgi dwmapi ole32 uuid shell32 advapi32)
target_compile_definitions(app PRIVATE UNICODE _UNICODE)
set_target_properties(app PROPERTIES OUTPUT_NAME "KuangFrameExtrapolation")


# --- Kuang Optimizer: kopt.dll (in game) ------------------------------------------
add_library(kopt SHARED optimizer/kopt.cpp)
target_link_libraries(kopt PRIVATE minhook powrprof)
set_target_properties(kopt PROPERTIES OUTPUT_NAME kopt)

# --- Kuang Optimizer app (runs as administrator) ------------------------------------
add_executable(optimizer_app WIN32
    optimizer/app_opt.cpp
    optimizer/app.manifest
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp)
target_include_directories(optimizer_app PRIVATE third_party/imgui optimizer)
target_link_libraries(optimizer_app PRIVATE d3d11 d3dcompiler dxgi dwmapi ole32 uuid shell32 advapi32 powrprof setupapi cfgmgr32)
target_compile_definitions(optimizer_app PRIVATE UNICODE _UNICODE)
set_target_properties(optimizer_app PROPERTIES OUTPUT_NAME "KuangOptimizer")

# --- Kuang Menu: kmenu.dll (in-game menu, opens with Tab) ---------------------------
add_library(kmenu SHARED
    menu/kmenu.cpp
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_opengl2.cpp)
target_include_directories(kmenu PRIVATE third_party/imgui third_party)
target_link_libraries(kmenu PRIVATE minhook opengl32 dwmapi shell32 user32 gdi32 comdlg32 winhttp)
target_compile_definitions(kmenu PRIVATE _CRT_SECURE_NO_WARNINGS)
set_target_properties(kmenu PROPERTIES OUTPUT_NAME kmenu)

# --- Kuang Installer: compiles + installs everything (KuangInstaller.exe in kfe-src) -----
add_executable(installer WIN32
    installer/installer.cpp
    installer/installer.rc
    optimizer/app.manifest
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp)
target_include_directories(installer PRIVATE third_party/imgui)
target_link_libraries(installer PRIVATE d3d11 d3dcompiler dxgi dwmapi ole32 uuid shell32 advapi32)
target_compile_definitions(installer PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
set_target_properties(installer PROPERTIES OUTPUT_NAME "KuangInstaller")
# pack every built mod + app inside KuangInstaller.exe, so the .exe alone installs everything
file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/payload_$<CONFIG>.rc CONTENT
"101 RCDATA \"$<TARGET_FILE:kfe>\"
102 RCDATA \"$<TARGET_FILE:kopt>\"
103 RCDATA \"$<TARGET_FILE:kmenu>\"
104 RCDATA \"$<TARGET_FILE:winmm_proxy>\"
105 RCDATA \"$<TARGET_FILE:app>\"
106 RCDATA \"$<TARGET_FILE:optimizer_app>\"
")
target_sources(installer PRIVATE ${CMAKE_BINARY_DIR}/payload_$<CONFIG>.rc)
# build the mods first, and re-pack them every time (touching the .rc makes it compile again)
add_custom_target(installer_payload
    COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/payload_$<CONFIG>.rc)
add_dependencies(installer_payload kfe kopt kmenu winmm_proxy app optimizer_app)
add_dependencies(installer installer_payload)
add_custom_command(TARGET installer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DSRC=$<TARGET_FILE:installer> -DDST=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/cmake/soft_copy.cmake VERBATIM)

# put everything the user needs in dist/
foreach(t kfe winmm_proxy app kopt optimizer_app kmenu)
    add_custom_command(TARGET ${t} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -DSRC=$<TARGET_FILE:${t}> -DDST=${DIST} -P ${CMAKE_SOURCE_DIR}/cmake/soft_copy.cmake VERBATIM)
endforeach()

# --- Kuang Setup: Geode-style wizard (KuangSetup.exe) with every mod packed inside -------------
add_executable(setup WIN32
    setup/setup.cpp
    setup/setup.rc
    setup/setup.manifest)
target_include_directories(setup PRIVATE shared)
target_link_libraries(setup PRIVATE ole32 uuid shell32 advapi32 comctl32 user32 gdi32)
target_compile_definitions(setup PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS KUANG_SETUP_VERSION="0.14.10")
set_target_properties(setup PROPERTIES OUTPUT_NAME "KuangSetup")
target_sources(setup PRIVATE ${CMAKE_BINARY_DIR}/payload_$<CONFIG>.rc)
add_dependencies(setup installer_payload)
add_custom_command(TARGET setup POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DSRC=$<TARGET_FILE:setup> -DDST=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/cmake/soft_copy.cmake VERBATIM)
