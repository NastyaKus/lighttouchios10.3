cmake_minimum_required(VERSION 3.22.1)
project(inferno_jni C)

# The bridge between the app and the engine. The engine itself
# (libqemu-aarch64-softmmu.so) is not linked here: it is opened with dlopen at
# run time, so the app builds and starts without one and can say what is wrong.
add_library(inferno_jni SHARED inferno_jni.c gles_renderer.c)
target_compile_options(inferno_jni PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(inferno_jni PRIVATE android log dl EGL GLESv3)
target_link_options(inferno_jni PRIVATE -Wl,-z,max-page-size=16384)

# Loaded only by the isolated :gpu bound service, never by HWUI or QEMU.
set(GPU_BRIDGE "${CMAKE_CURRENT_SOURCE_DIR}/../../../../gpu-bridge")
add_library(ember_guest_gpu_probe SHARED gpu_probe_jni.c gles_renderer.c
    ${GPU_BRIDGE}/protocol.c ${GPU_BRIDGE}/session.c ${GPU_BRIDGE}/executor.c
    ${GPU_BRIDGE}/gles-host.c ${GPU_BRIDGE}/file-host.c ${GPU_BRIDGE}/presentation.c)
target_include_directories(ember_guest_gpu_probe PRIVATE ${GPU_BRIDGE} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(ember_guest_gpu_probe PRIVATE -O2 -Wall -Wextra -Werror)
target_link_libraries(ember_guest_gpu_probe PRIVATE android EGL GLESv3)
target_link_options(ember_guest_gpu_probe PRIVATE -Wl,-z,max-page-size=16384)

# Compile the independent full-panel transfer/presentation component for
# Android too. It is not wired into the working compositor or probe library.
add_library(ember_system_surface STATIC ${GPU_BRIDGE}/system/surface.c)
target_include_directories(ember_system_surface PRIVATE ${GPU_BRIDGE} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(ember_system_surface PRIVATE -O2 -Wall -Wextra -Werror)

# Persistent EGP2 backend. Its own service process owns EGL; ordinary launch
# and the existing GPU probe retain their current libraries and renderer.
add_library(ember_system_gpu SHARED system_gpu_jni.c gles_renderer.c
    ${GPU_BRIDGE}/system/host.c ${GPU_BRIDGE}/system/surface.c)
target_include_directories(ember_system_gpu PRIVATE ${GPU_BRIDGE} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(ember_system_gpu PRIVATE -O2 -Wall -Wextra -Werror)
target_link_libraries(ember_system_gpu PRIVATE android EGL GLESv3)
target_link_options(ember_system_gpu PRIVATE -Wl,-z,max-page-size=16384)
