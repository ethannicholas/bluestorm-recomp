cmake_minimum_required(VERSION 3.20)
project(waverace C CXX)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Everything that is not specific to Wave Race lives in gcn-recomp (a submodule): the
# recompiler, the GameCube runtime, the renderer and the frontends, the headset's included.
# See its README. What is left here is the game's tables and its answers to the frontends'
# questions: when to be in stereo, where the eye sits on the ski, how big the world is.
add_subdirectory(gcn-recomp)

gcn_add_game(
  NAME waverace
  TITLE "Wave Race: Blue Storm"
  DISC_ID GWRE01
  DOL_SHA1 d500d8f5bab39ae5fed67d0a0864095a9ea190d9
  SYMBOLS ${CMAKE_SOURCE_DIR}/analysis/symbols.txt
  TABLES ${CMAKE_SOURCE_DIR}/recomp
  FPS 30
  # The game's half of the headset: its hooks (runtime/vr_waverace.cpp), the starting-light
  # rig detector that starts stereo, and the renderer's eye hook for first person on the ski.
  SOURCES ${CMAKE_SOURCE_DIR}/runtime/vr_waverace.cpp ${CMAKE_SOURCE_DIR}/runtime/start_rig.cpp
          ${CMAKE_SOURCE_DIR}/runtime/first_person.cpp ${CMAKE_SOURCE_DIR}/runtime/pace.cpp
  # The headset app and its harness, on an Android build: see build-android.ps1 and
  # package-apk.ps1.
  ANDROID_PACKAGE com.example.waverace)
