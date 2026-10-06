cmake_minimum_required(VERSION 3.20)
project(waverace C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The game image is supplied by the user (see README): an uncompressed .iso of
# Wave Race: Blue Storm (USA, GWRE01) placed in rom/.
file(GLOB _rom_isos ${CMAKE_SOURCE_DIR}/rom/*.iso)
list(LENGTH _rom_isos _rom_count)
if(_rom_count GREATER 0)
  list(GET _rom_isos 0 _default_iso)
else()
  set(_default_iso "")
endif()
set(GAME_ISO "${_default_iso}" CACHE FILEPATH "Your Wave Race: Blue Storm (USA) disc image (.iso)")

# Without an image we can still compile the runtime, which is enough to check a
# port. Only the recompiled game code and the final link need the disc.
if(GAME_ISO AND EXISTS "${GAME_ISO}")
  set(HAVE_GAME_ISO TRUE)
else()
  set(HAVE_GAME_ISO FALSE)
  if(GAME_ISO)
    message(WARNING "GAME_ISO is set to '${GAME_ISO}' but that file does not exist.")
  endif()
  message(WARNING
    "No game image found, so only the `runtime_check` target (compile the runtime, no link) "
    "is available. Put your own Wave Race: Blue Storm (USA) .iso in ${CMAKE_SOURCE_DIR}/rom/ "
    "or pass -DGAME_ISO=/path/to/game.iso to build the game. See README.md.")
endif()

set(GAME_DOL ${CMAKE_BINARY_DIR}/main.dol)
set(DTK_SYMBOLS ${CMAKE_SOURCE_DIR}/analysis/symbols.txt)
set(RECOMP_GEN_DIR ${CMAKE_BINARY_DIR}/gen)

# ---- Recompiled game code ----
if(HAVE_GAME_ISO)
  execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/recomp/extract_dol.py ${GAME_ISO} ${GAME_DOL}
                  RESULT_VARIABLE EXTRACT_RESULT)
  if(NOT EXTRACT_RESULT EQUAL 0)
    message(FATAL_ERROR "could not extract main.dol from ${GAME_ISO}")
  endif()

  file(GLOB RECOMP_TOOL_SOURCES ${CMAKE_SOURCE_DIR}/recomp/*.py ${CMAKE_SOURCE_DIR}/recomp/*.txt)
  execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/recomp/recomp.py ${GAME_DOL} ${DTK_SYMBOLS} ${RECOMP_GEN_DIR}
                  RESULT_VARIABLE RECOMP_RESULT)
  if(NOT RECOMP_RESULT EQUAL 0)
    message(FATAL_ERROR "recompilation failed")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RECOMP_TOOL_SOURCES} ${GAME_ISO} ${DTK_SYMBOLS})
  include(${RECOMP_GEN_DIR}/sources.cmake)

  add_library(game_recomp STATIC ${RECOMP_SOURCES})
  target_include_directories(game_recomp PUBLIC ${CMAKE_SOURCE_DIR}/runtime ${RECOMP_GEN_DIR})
  if(MSVC)
    # Generated code is enormous and machine-written: keep the optimizer honest about
    # aliasing and floating point, and silence the noise it legitimately produces.
    target_compile_options(game_recomp PRIVATE /O2 /fp:precise /fp:except- /wd4101 /wd4102 /wd4723 /bigobj)
  else()
    target_compile_options(game_recomp PRIVATE -O2 -fno-strict-aliasing -Wno-unused-label -Wno-unused-variable
                           -Wno-parentheses-equality -ffp-contract=off)
  endif()
endif()

# ---- Runtime ----
# WR_BENCH_ONLY builds just the headless benchmark: no renderer, no SDL2, no GL. That
# is what a cross-build for a device (Android/arm64, say) uses, since it only needs a
# C++ toolchain and the recompiled code.
option(WR_BENCH_ONLY "Build only the headless benchmark (no SDL2, no renderer)" OFF)

file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/runtime/*.cpp)

# Alternative entry points and the SDL-free audio stub: none of these may land in a
# target that already has main.cpp or audio.cpp.
list(REMOVE_ITEM RUNTIME_SOURCES
  ${CMAKE_SOURCE_DIR}/runtime/bench_main.cpp
  ${CMAKE_SOURCE_DIR}/runtime/egl_main.cpp
  ${CMAKE_SOURCE_DIR}/runtime/android_main.cpp
  ${CMAKE_SOURCE_DIR}/runtime/openxr_main.cpp
  ${CMAKE_SOURCE_DIR}/runtime/vr_config.cpp
  ${CMAKE_SOURCE_DIR}/runtime/audio_stub.cpp
  ${CMAKE_SOURCE_DIR}/runtime/audio_sdl.cpp
  ${CMAKE_SOURCE_DIR}/runtime/audio_aaudio.cpp)

# The desktop's audio device. Added back here rather than left in the glob so that the
# two backends cannot both land in one target; the Android ones are listed above and
# picked up by CORE_SOURCES_AUDIO instead.
if(NOT ANDROID)
  list(APPEND RUNTIME_SOURCES ${CMAKE_SOURCE_DIR}/runtime/audio_sdl.cpp)
endif()

# Everything below the renderer and the SDL frontend -- the CPU, OS, hardware and GX
# front end, including the batch queue in gx/xf.cpp -- is free of both GL and SDL.
set(CORE_SOURCES ${RUNTIME_SOURCES})
list(REMOVE_ITEM CORE_SOURCES
  ${CMAKE_SOURCE_DIR}/runtime/main.cpp
  ${CMAKE_SOURCE_DIR}/runtime/audio.cpp
  ${CMAKE_SOURCE_DIR}/runtime/audio_sdl.cpp
  ${CMAKE_SOURCE_DIR}/runtime/png.cpp
  ${CMAKE_SOURCE_DIR}/runtime/gx/render_gl.cpp
  ${CMAKE_SOURCE_DIR}/runtime/gx/gl_load.cpp
  ${CMAKE_SOURCE_DIR}/runtime/gx/shadergen.cpp)
list(APPEND CORE_SOURCES ${CMAKE_SOURCE_DIR}/runtime/audio_stub.cpp)

set(BENCH_SOURCES ${CORE_SOURCES} ${CMAKE_SOURCE_DIR}/runtime/bench_main.cpp)

# The same core with real audio instead of the stub, for the Android targets. The mix in
# audio.cpp is portable; only the device belongs to a platform, so Android swaps the SDL
# backend for the AAudio one and is otherwise the desktop's audio exactly.
set(CORE_SOURCES_AUDIO ${CORE_SOURCES})
list(REMOVE_ITEM CORE_SOURCES_AUDIO ${CMAKE_SOURCE_DIR}/runtime/audio_stub.cpp)
list(APPEND CORE_SOURCES_AUDIO
  ${CMAKE_SOURCE_DIR}/runtime/audio.cpp
  ${CMAKE_SOURCE_DIR}/runtime/audio_aaudio.cpp)

# Render target: desktop OpenGL 3.3 core, or OpenGL ES 3.2 for Android and standalone
# headsets. One renderer source serves both; see runtime/gx/gl.h.
if(ANDROID)
  set(_wr_gl_es_default ON)
else()
  set(_wr_gl_es_default OFF)
endif()
option(WR_GL_ES "Target OpenGL ES 3.2 instead of desktop OpenGL 3.3 core" ${_wr_gl_es_default})

# GL entry points are resolved at runtime via a vendored glad loader. macOS links the
# system framework directly and needs neither.
if(WR_GL_ES)
  set(GLAD_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/runtime/gx/glad_es/include)
  set(GLAD_SOURCE ${CMAKE_SOURCE_DIR}/runtime/gx/glad_es/src/gles2.c)
  set(WR_GL_DEFS WR_GL_ES)
else()
  set(GLAD_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/runtime/gx/glad/include)
  set(GLAD_SOURCE ${CMAKE_SOURCE_DIR}/runtime/gx/glad/src/gl.c)
  set(WR_GL_DEFS "")
endif()
if(NOT APPLE)
  list(APPEND RUNTIME_SOURCES ${GLAD_SOURCE})
endif()

# SDL2: pkg-config where it exists; on Windows there are no prebuilt ARM64 binaries,
# so build it from source.
if(WR_BENCH_ONLY)
  set(WR_SDL_TARGET "")
elseif(WIN32)
  include(FetchContent)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL2
    URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz
    URL_HASH SHA256=24b574f71c87a763f50704bbb630cbe38298d544a1f890f099a4696b1d6beba4
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(SDL2)
  set(WR_SDL_TARGET SDL2::SDL2-static)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  set(WR_SDL_TARGET PkgConfig::SDL2)
endif()

if(MSVC)
  set(WR_RUNTIME_WARNINGS /W3 /wd4244 /wd4267)
else()
  set(WR_RUNTIME_WARNINGS -Wall -Wno-unused-function)
endif()

# Platform libraries the runtime needs beyond SDL2. The benchmark links none of this.
set(WR_PLATFORM_LIBS "")
if(WR_BENCH_ONLY)
elseif(APPLE)
  list(APPEND WR_PLATFORM_LIBS "-framework OpenGL")
elseif(WIN32)
  # opengl32 for the GL entry points, dbghelp to symbolize crash backtraces.
  list(APPEND WR_PLATFORM_LIBS opengl32 dbghelp)
else()
  find_package(OpenGL REQUIRED)
  list(APPEND WR_PLATFORM_LIBS OpenGL::GL)
endif()

# Headless benchmark: reports how fast the guest advances, with no graphics or audio
# in the way. Used to compare host CPUs -- see the Benchmarking section in README.md.
if(HAVE_GAME_ISO)
  add_executable(waverace_bench ${BENCH_SOURCES})
  target_include_directories(waverace_bench PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${RECOMP_GEN_DIR})
  target_link_libraries(waverace_bench PRIVATE game_recomp)
  target_compile_options(waverace_bench PRIVATE ${WR_RUNTIME_WARNINGS})
  target_compile_definitions(waverace_bench PRIVATE WR_DEFAULT_ISO="${GAME_ISO}")
  if(WIN32)
    target_link_libraries(waverace_bench PRIVATE dbghelp)
  endif()
endif()

# Headless EGL frontend: the real renderer, offscreen, writing frames as PNGs. Android
# only, and deliberately not an APK -- it validates the GL ES renderer on a device over
# adb before there is any VR frontend to run it in.
if(ANDROID AND HAVE_GAME_ISO)
  set(EGL_SOURCES ${CORE_SOURCES_AUDIO}
    ${CMAKE_SOURCE_DIR}/runtime/egl_main.cpp
    ${CMAKE_SOURCE_DIR}/runtime/png.cpp
    ${CMAKE_SOURCE_DIR}/runtime/gx/render_gl.cpp
    ${CMAKE_SOURCE_DIR}/runtime/gx/shadergen.cpp
    ${CMAKE_SOURCE_DIR}/runtime/gx/gl_load.cpp
    ${GLAD_SOURCE})
  add_executable(waverace_egl ${EGL_SOURCES})
  target_include_directories(waverace_egl PRIVATE
    ${CMAKE_SOURCE_DIR}/runtime ${RECOMP_GEN_DIR} ${GLAD_INCLUDE_DIR})
  target_link_libraries(waverace_egl PRIVATE game_recomp EGL log aaudio)
  target_compile_options(waverace_egl PRIVATE ${WR_RUNTIME_WARNINGS})
  target_compile_definitions(waverace_egl PRIVATE WR_DEFAULT_ISO="${GAME_ISO}" ${WR_GL_DEFS})

  # The installable app: a NativeActivity shared library, packaged into an APK by
  # package-apk.ps1. android.app.NativeActivity loads it and calls android_main(),
  # so there is no Java source at all.
  add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
  target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

  # Same set as waverace_egl, with the NativeActivity entry point instead of the
  # headless one. The library must be called libwaverace.so to match the manifest's
  # android.app.lib_name.
  # The OpenXR loader comes from Khronos' official Android AAR on Maven Central: it
  # carries both the headers and a prebuilt arm64 libopenxr_loader.so, so nothing has
  # to be committed as a binary.
  include(FetchContent)
  FetchContent_Declare(openxr_aar
    URL https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/1.1.63/openxr_loader_for_android-1.1.63.aar
    URL_HASH SHA256=622419d2f6741c3443a3beb4779af0764318edd01830de967f24c741ebcded73
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(openxr_aar)
  set(OPENXR_INCLUDE ${openxr_aar_SOURCE_DIR}/prefab/modules/headers/include)
  set(OPENXR_LIB ${openxr_aar_SOURCE_DIR}/prefab/modules/openxr_loader/libs/android.${ANDROID_ABI}/libopenxr_loader.so)

  set(APP_SOURCES ${CORE_SOURCES_AUDIO}
    ${CMAKE_SOURCE_DIR}/runtime/openxr_main.cpp
    ${CMAKE_SOURCE_DIR}/runtime/vr_config.cpp
    ${CMAKE_SOURCE_DIR}/runtime/png.cpp
    ${CMAKE_SOURCE_DIR}/runtime/gx/render_gl.cpp
    ${CMAKE_SOURCE_DIR}/runtime/gx/shadergen.cpp
    ${CMAKE_SOURCE_DIR}/runtime/gx/gl_load.cpp
    ${GLAD_SOURCE})
  add_library(waverace SHARED ${APP_SOURCES})
  target_include_directories(waverace PRIVATE
    ${CMAKE_SOURCE_DIR}/runtime ${RECOMP_GEN_DIR} ${GLAD_INCLUDE_DIR} ${OPENXR_INCLUDE})
  target_link_libraries(waverace PRIVATE game_recomp native_app_glue ${OPENXR_LIB} EGL android log aaudio)
  target_compile_options(waverace PRIVATE ${WR_RUNTIME_WARNINGS})
  target_compile_definitions(waverace PRIVATE WR_DEFAULT_ISO="" ${WR_GL_DEFS})
  # NativeActivity resolves this symbol by name; without it the linker drops the glue.
  target_link_options(waverace PRIVATE -u ANativeActivity_onCreate)
endif()

# Compile-only target: available even without a disc image, so the runtime and any
# port work can be checked on its own. With an image present `waverace` already
# compiles these sources, so keep it out of the default build rather than doubling
# every build; `ninja runtime_check` still works.
if(NOT WR_BENCH_ONLY)
  if(HAVE_GAME_ISO)
    add_library(runtime_check OBJECT EXCLUDE_FROM_ALL ${RUNTIME_SOURCES})
  else()
    add_library(runtime_check OBJECT ${RUNTIME_SOURCES})
  endif()
  target_include_directories(runtime_check PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${RECOMP_GEN_DIR} ${GLAD_INCLUDE_DIR})
  target_link_libraries(runtime_check PRIVATE ${WR_SDL_TARGET})
  target_compile_options(runtime_check PRIVATE ${WR_RUNTIME_WARNINGS})
  target_compile_definitions(runtime_check PRIVATE WR_DEFAULT_ISO="${GAME_ISO}" ${WR_GL_DEFS})
endif()

if(HAVE_GAME_ISO AND NOT WR_BENCH_ONLY)
  add_executable(waverace ${RUNTIME_SOURCES})
  target_include_directories(waverace PRIVATE ${GLAD_INCLUDE_DIR})
  target_link_libraries(waverace PRIVATE game_recomp ${WR_SDL_TARGET} ${WR_PLATFORM_LIBS})
  target_compile_options(waverace PRIVATE ${WR_RUNTIME_WARNINGS})
  target_compile_definitions(waverace PRIVATE WR_DEFAULT_ISO="${GAME_ISO}" ${WR_GL_DEFS})
endif()
