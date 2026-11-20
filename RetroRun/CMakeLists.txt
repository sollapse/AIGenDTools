# RetroRun, a Windows tray program. It builds with Visual Studio (MSVC), MinGW-w64 (GCC, or Clang for MinGW) and
# Clang (clang++ or clang-cl); CMakePresets.json has a preset for each, which takes the compiler found on PATH.
cmake_minimum_required(VERSION 3.21)
project(RetroRun
  VERSION 1.0.0
  DESCRIPTION "Disk names and icons in File Explorer, autorun.inf programs and AutoPlay for removable disks"
  LANGUAGES CXX RC)

if(NOT WIN32)
  message(FATAL_ERROR "RetroRun is a Windows program: configure it with a compiler that targets Windows.")
endif()

option(RETRORUN_BUILD_TESTS "Build the unit tests, which ctest runs" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# The runtime libraries are linked in, so that the program runs on its own from any folder.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# What every target is built with: Unicode, the API of Windows 10 and later, and the warnings.
add_library(retrorun_options INTERFACE)
target_compile_definitions(retrorun_options INTERFACE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)
if(MSVC)
  # cl and clang-cl.
  target_compile_options(retrorun_options INTERFACE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive->")
else()
  target_compile_options(retrorun_options INTERFACE "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra>")
endif()
if(MINGW)
  # The wide entry points, wWinMain and wmain, and the GCC runtime libraries linked in.
  target_link_options(retrorun_options INTERFACE -municode -static)
endif()

# Reading disks, autorun.inf files and the programs they name: shared by the program and its tests.
add_library(retrorun_core STATIC
  src/drives.cpp src/drives.h
  src/inf.cpp src/inf.h
  src/launch.cpp src/launch.h)
target_include_directories(retrorun_core PUBLIC src)
target_link_libraries(retrorun_core PUBLIC retrorun_options advapi32 ole32 shell32 shlwapi user32 uuid)

add_executable(RetroRun WIN32 src/RetroRun.cpp src/RetroRun.rc)
target_link_libraries(RetroRun PRIVATE retrorun_core)
if(MINGW)
  # MinGW-w64's linker makes no manifest, so the resource script holds the program's own.
  set_property(SOURCE src/RetroRun.rc APPEND PROPERTY COMPILE_DEFINITIONS RETRORUN_MANIFEST_RESOURCE)
else()
  # The linkers of the MSVC ABI make a manifest and merge the program's into it.
  target_sources(RetroRun PRIVATE src/RetroRun.manifest)
endif()
# The files the resource script takes in, so that a change to either builds the resources again.
set_property(SOURCE src/RetroRun.rc APPEND PROPERTY OBJECT_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/RetroRun.ico" "${CMAKE_CURRENT_SOURCE_DIR}/src/RetroRun.manifest")
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT RetroRun)

if(RETRORUN_BUILD_TESTS)
  enable_testing()
  add_executable(launch-test tests/launch-test.cpp)
  target_link_libraries(launch-test PRIVATE retrorun_core)
  add_test(NAME launch-test COMMAND launch-test)
endif()
