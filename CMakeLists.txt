cmake_minimum_required(VERSION 3.16)
project(link_foe_recomp VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
# static C runtime: the executables run on a clean Windows without the VC++ redistributable
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# GitHub repository whose releases the update check looks at (empty = no update check)
set(LINKFOE_UPDATE_REPO "tools81/link-faces-of-evil-static-recomp" CACHE STRING "owner/name for the update check")

file(GLOB GEN_SOURCES ${CMAKE_SOURCE_DIR}/gen/*.c)
file(GLOB RT_SOURCES ${CMAKE_SOURCE_DIR}/runtime/*.c)
# presentation files differ between the standard and HD editions
set(EDITION_SOURCES ${CMAKE_SOURCE_DIR}/runtime/render_d3d11.c ${CMAKE_SOURCE_DIR}/runtime/host_win32.c)
list(REMOVE_ITEM RT_SOURCES ${EDITION_SOURCES})

if(MSVC)
  set(GAME_OPTS /O2 /W3 /wd4146 /wd4244 /wd4267 /wd4996 /wd4102 /bigobj)
  set(GAME_DEFS _CRT_SECURE_NO_WARNINGS)
else()
  set(GAME_OPTS -O2 -Wno-unused-label)
  set(GAME_DEFS)
endif()

list(APPEND GAME_DEFS LINKFOE_VERSION="${PROJECT_VERSION}" LINKFOE_UPDATE_REPO="${LINKFOE_UPDATE_REPO}")

# recompiled game code + runtime, compiled once and shared by both editions
add_library(linkfoe_core OBJECT ${RT_SOURCES} ${GEN_SOURCES})
target_include_directories(linkfoe_core PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/gen)
target_compile_options(linkfoe_core PRIVATE ${GAME_OPTS})
target_compile_definitions(linkfoe_core PRIVATE ${GAME_DEFS})

# linkfoe: standard edition; linkfoe_hd: adds the Cartoon HD upscaling filters
foreach(ed linkfoe linkfoe_hd)
  add_executable(${ed} ${EDITION_SOURCES} $<TARGET_OBJECTS:linkfoe_core>)
  target_include_directories(${ed} PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/gen)
  target_compile_options(${ed} PRIVATE ${GAME_OPTS})
  target_compile_definitions(${ed} PRIVATE ${GAME_DEFS})
  if(MSVC)
    target_link_libraries(${ed} PRIVATE winmm gdi32 user32 comdlg32 winhttp)
    set_target_properties(${ed} PROPERTIES WIN32_EXECUTABLE OFF)
  endif()
endforeach()
target_compile_definitions(linkfoe_hd PRIVATE HD_BUILD)

# Differential instruction tester (recompiled semantics vs interpreter).
# Generate test/insn_tests.c with: python tools/gen_tests.py
if(EXISTS ${CMAKE_SOURCE_DIR}/test/insn_tests.c)
  add_executable(insn_test test/harness.c test/insn_tests.c runtime/interp.c runtime/alu.c runtime/disc.c)
  target_include_directories(insn_test PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/gen)
  target_compile_definitions(insn_test PRIVATE RECOMP_TEST _CRT_SECURE_NO_WARNINGS)
  if(MSVC)
    target_compile_options(insn_test PRIVATE /O1 /bigobj /wd4146 /wd4244 /wd4267 /wd4996 /wd4102)
  endif()
endif()

# ALU helper check against an independent reference model (python test/gen_alu_vectors.py)
if(EXISTS ${CMAKE_SOURCE_DIR}/test/alu_vectors.h)
  add_executable(alu_test test/alu_test.c runtime/alu.c)
  target_include_directories(alu_test PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/gen ${CMAKE_SOURCE_DIR}/test)
endif()
