# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.16)
project(kx_mb2020_driver C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(MB2020_BUILD_TESTS "Build tests" ON)

# jbig-kit (GPL) — JBIG-1 кодек
add_library(jbigkit STATIC third_party/jbigkit/jbig.c third_party/jbigkit/jbig_tab.c)
target_include_directories(jbigkit PUBLIC third_party/jbigkit)
target_compile_definitions(jbigkit PUBLIC _LIB)  # статическая сборка, без __declspec(dllimport)
if(NOT MSVC)
  target_compile_options(jbigkit PRIVATE -w)
endif()

# Ядро: переносимая библиотека (Windows, Linux, позже Android NDK)
add_library(mb2020 STATIC
  core/src/bitmap.cpp
  core/src/jbig_band.cpp
  core/src/gdi_print.cpp
  core/src/raster.cpp
  core/src/net.cpp
  core/src/printer.cpp
  core/src/scan.cpp
  core/src/stb_impl.c)
target_include_directories(mb2020 PUBLIC core/include PRIVATE third_party/stb)
target_compile_definitions(mb2020 PRIVATE STBI_WINDOWS_UTF8 STBIW_WINDOWS_UTF8)
set_source_files_properties(core/src/stb_impl.c PROPERTIES COMPILE_OPTIONS "-w")
target_link_libraries(mb2020 PUBLIC jbigkit)
if(WIN32)
  target_link_libraries(mb2020 PUBLIC ws2_32 iphlpapi)
endif()
if(NOT MSVC)
  target_compile_options(mb2020 PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra>)
endif()

add_executable(mbtool tools/mbtool/main.cpp)
target_link_libraries(mbtool PRIVATE mb2020)
if(WIN32)
  target_link_libraries(mbtool PRIVATE shell32)
  if(MINGW)
    target_link_options(mbtool PRIVATE -static)
  endif()
endif()

if(MB2020_BUILD_TESTS)
  enable_testing()
  find_package(Threads REQUIRED)
  add_executable(mb2020_tests tests/test_core.cpp)
  target_link_libraries(mb2020_tests PRIVATE mb2020 Threads::Threads)
  if(MINGW)
    target_link_options(mb2020_tests PRIVATE -static)
  endif()
  add_test(NAME core COMMAND mb2020_tests)
endif()
