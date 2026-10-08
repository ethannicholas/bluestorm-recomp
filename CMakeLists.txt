cmake_minimum_required(VERSION 3.20)
project(waverace C CXX)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Everything that is not specific to Wave Race lives in gcn-recomp (a submodule): the
# recompiler, the GameCube runtime, the renderer and the desktop frontend. See its README.
# What is left here is the game's tables and its headset frontends.
add_subdirectory(gcn-recomp)

gcn_add_game(
  NAME waverace
  TITLE "Wave Race: Blue Storm"
  DISC_ID GWRE01
  DOL_SHA1 d500d8f5bab39ae5fed67d0a0864095a9ea190d9
  SYMBOLS ${CMAKE_SOURCE_DIR}/analysis/symbols.txt
  TABLES ${CMAKE_SOURCE_DIR}/recomp
  FPS 30)

# ---- Android ----
# Two targets, built from the pieces gcn_add_game() exports, with this game's own frontends
# and the AAudio device in place of SDL's. Neither is built on a desktop.
if(ANDROID AND GCN_HAVE_GAME_ISO)
  # The same core with real audio instead of the stub: the mix in gcn-recomp is portable,
  # only the device belongs to a platform.
  set(CORE_SOURCES_AUDIO ${GCN_CORE_SOURCES} ${GCN_AUDIO_SOURCES}
    ${CMAKE_SOURCE_DIR}/runtime/audio_aaudio.cpp)
  # The starting-light rig detector, which the VR frontend keys stereo off.
  set(GAME_SOURCES ${CMAKE_SOURCE_DIR}/runtime/start_rig.cpp)

  # Headless EGL frontend: the real renderer, offscreen, writing frames as PNGs. Deliberately
  # not an APK -- it validates the GL ES renderer on a device over adb before there is any VR
  # frontend to run it in.
  add_executable(waverace_egl ${CORE_SOURCES_AUDIO} ${GCN_RENDER_SOURCES} ${GCN_GLAD_SOURCE}
    ${GAME_SOURCES} ${CMAKE_SOURCE_DIR}/runtime/egl_main.cpp)
  set_target_properties(waverace_egl PROPERTIES C_STANDARD 11 CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
  target_include_directories(waverace_egl PRIVATE
    ${CMAKE_SOURCE_DIR}/runtime ${GCN_RT} ${GCN_GEN_DIR} ${GCN_GLAD_INCLUDE_DIR})
  target_link_libraries(waverace_egl PRIVATE game_recomp EGL log aaudio)
  target_compile_options(waverace_egl PRIVATE ${GCN_RUNTIME_WARNINGS})
  target_compile_definitions(waverace_egl PRIVATE ${GCN_GAME_DEFS} ${GCN_GL_DEFS})

  # The installable app: a NativeActivity shared library, packaged into an APK by
  # package-apk.ps1. android.app.NativeActivity loads it and calls android_main(), so there
  # is no Java source at all.
  add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
  target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

  # The library must be called libwaverace.so to match the manifest's android.app.lib_name.
  # The OpenXR loader comes from Khronos' official Android AAR on Maven Central: it carries
  # both the headers and a prebuilt arm64 libopenxr_loader.so, so nothing has to be
  # committed as a binary.
  include(FetchContent)
  FetchContent_Declare(openxr_aar
    URL https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/1.1.63/openxr_loader_for_android-1.1.63.aar
    URL_HASH SHA256=622419d2f6741c3443a3beb4779af0764318edd01830de967f24c741ebcded73
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(openxr_aar)
  set(OPENXR_INCLUDE ${openxr_aar_SOURCE_DIR}/prefab/modules/headers/include)
  set(OPENXR_LIB ${openxr_aar_SOURCE_DIR}/prefab/modules/openxr_loader/libs/android.${ANDROID_ABI}/libopenxr_loader.so)

  add_library(waverace SHARED ${CORE_SOURCES_AUDIO} ${GCN_RENDER_SOURCES} ${GCN_GLAD_SOURCE}
    ${GAME_SOURCES}
    ${CMAKE_SOURCE_DIR}/runtime/openxr_main.cpp
    ${CMAKE_SOURCE_DIR}/runtime/vr_config.cpp)
  set_target_properties(waverace PROPERTIES C_STANDARD 11 CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
  target_include_directories(waverace PRIVATE
    ${CMAKE_SOURCE_DIR}/runtime ${GCN_RT} ${GCN_GEN_DIR} ${GCN_GLAD_INCLUDE_DIR} ${OPENXR_INCLUDE})
  target_link_libraries(waverace PRIVATE game_recomp native_app_glue ${OPENXR_LIB} EGL android log aaudio)
  target_compile_options(waverace PRIVATE ${GCN_RUNTIME_WARNINGS})
  # No default image: the app finds game.iso in its own files directory.
  target_compile_definitions(waverace PRIVATE GCN_DEFAULT_ISO="" GCN_GAME_TITLE="Wave Race: Blue Storm"
    GCN_DISC_ID="GWRE01" GCN_GAME_FPS=30 ${GCN_GL_DEFS})
  # NativeActivity resolves this symbol by name; without it the linker drops the glue.
  target_link_options(waverace PRIVATE -u ANativeActivity_onCreate)
endif()
