cmake_minimum_required(VERSION 3.20)
project(waverace C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
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
if(NOT GAME_ISO OR NOT EXISTS "${GAME_ISO}")
  message(FATAL_ERROR "No game image found. Put your own Wave Race: Blue Storm (USA) .iso in ${CMAKE_SOURCE_DIR}/rom/ "
                      "(or pass -DGAME_ISO=/path/to/game.iso). See README.md.")
endif()
set(GAME_DOL ${CMAKE_BINARY_DIR}/main.dol)
set(DTK_SYMBOLS ${CMAKE_SOURCE_DIR}/analysis/symbols.txt)
set(RECOMP_GEN_DIR ${CMAKE_BINARY_DIR}/gen)

execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/recomp/extract_dol.py ${GAME_ISO} ${GAME_DOL}
                RESULT_VARIABLE EXTRACT_RESULT)
if(NOT EXTRACT_RESULT EQUAL 0)
  message(FATAL_ERROR "could not extract main.dol from ${GAME_ISO}")
endif()

# ---- Recompiled game code ----
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
target_compile_options(game_recomp PRIVATE -O2 -fno-strict-aliasing -Wno-unused-label -Wno-unused-variable
                       -Wno-parentheses-equality -ffp-contract=off)

# ---- Runtime ----
file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/runtime/*.cpp)
add_executable(waverace ${RUNTIME_SOURCES})
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
target_link_libraries(waverace PRIVATE game_recomp PkgConfig::SDL2)
if(APPLE)
  target_link_libraries(waverace PRIVATE "-framework OpenGL")
endif()
target_compile_options(waverace PRIVATE -Wall -Wno-unused-function)
target_compile_definitions(waverace PRIVATE WR_DEFAULT_ISO="${GAME_ISO}")
