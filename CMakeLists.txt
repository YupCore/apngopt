cmake_minimum_required(VERSION 3.24)

project(apngopt LANGUAGES C CXX)

if(WIN32 AND NOT MSVC)
  message(FATAL_ERROR "Windows builds are supported only with MSVC toolchain. Use vcvars64.bat and CMake/Ninja.")
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(APNGOPT_ENABLE_IMAGEQUANT "Build and link vendored libimagequant via Cargo" ON)
option(APNGOPT_ZLIB_LIT_MEM "Enable zlib LIT_MEM for slight deflate speedup" ON)

set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZLIB_INSTALL OFF CACHE BOOL "" FORCE)
set(ZLIB_CONF_WRITTEN FALSE CACHE BOOL "" FORCE)
add_subdirectory(vendor/zlib EXCLUDE_FROM_ALL)

if(APNGOPT_ZLIB_LIT_MEM)
  if(TARGET zlibstatic)
    target_compile_definitions(zlibstatic PRIVATE LIT_MEM)
  endif()
  if(TARGET zlib)
    target_compile_definitions(zlib PRIVATE LIT_MEM)
  endif()
endif()

if(TARGET zlibstatic AND NOT TARGET ZLIB::ZLIB)
  add_library(ZLIB::ZLIB ALIAS zlibstatic)
endif()

set(ZLIB_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/zlib" CACHE PATH "" FORCE)
set(ZLIB_LIBRARY "zlibstatic" CACHE STRING "" FORCE)
set(ZLIB_USE_STATIC_LIBS ON CACHE BOOL "" FORCE)

set(PNG_SHARED OFF CACHE BOOL "" FORCE)
set(PNG_STATIC ON CACHE BOOL "" FORCE)
set(PNG_TESTS OFF CACHE BOOL "" FORCE)
set(PNG_TOOLS OFF CACHE BOOL "" FORCE)
set(PNG_BUILD_ZLIB OFF CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
add_subdirectory(vendor/libpng EXCLUDE_FROM_ALL)

if(TARGET png_static AND NOT TARGET PNG::PNG)
  add_library(PNG::PNG INTERFACE IMPORTED)
  target_link_libraries(PNG::PNG INTERFACE png_static)
endif()

add_library(apngopt_zopfli STATIC
  vendor/zopfli/src/zopfli/blocksplitter.c
  vendor/zopfli/src/zopfli/cache.c
  vendor/zopfli/src/zopfli/deflate.c
  vendor/zopfli/src/zopfli/gzip_container.c
  vendor/zopfli/src/zopfli/hash.c
  vendor/zopfli/src/zopfli/katajainen.c
  vendor/zopfli/src/zopfli/lz77.c
  vendor/zopfli/src/zopfli/squeeze.c
  vendor/zopfli/src/zopfli/tree.c
  vendor/zopfli/src/zopfli/util.c
  vendor/zopfli/src/zopfli/zlib_container.c
  vendor/zopfli/src/zopfli/zopfli_lib.c
)
target_include_directories(apngopt_zopfli PUBLIC vendor/zopfli/src/zopfli)

set(APNGOPT_7Z_DEFLATE_SRCS
  7z/7zdeflate.cc
  vendor/7z2601/CPP/7zip/Compress/DeflateEncoder.cpp
  vendor/7z2601/CPP/7zip/Compress/DeflateDecoder.cpp
  vendor/7z2601/CPP/7zip/Compress/BitlDecoder.cpp
  vendor/7z2601/CPP/7zip/Compress/LzOutWindow.cpp
  vendor/7z2601/CPP/7zip/Common/CWrappers.cpp
  vendor/7z2601/CPP/7zip/Common/InBuffer.cpp
  vendor/7z2601/CPP/7zip/Common/OutBuffer.cpp
  vendor/7z2601/CPP/7zip/Common/StreamObjects.cpp
  vendor/7z2601/CPP/7zip/Common/StreamUtils.cpp
  vendor/7z2601/C/Alloc.c
  vendor/7z2601/C/CpuArch.c
  vendor/7z2601/C/HuffEnc.c
  vendor/7z2601/C/LzFind.c
  vendor/7z2601/C/Sort.c
)

add_library(apngopt_7z_deflate STATIC ${APNGOPT_7Z_DEFLATE_SRCS})
target_include_directories(apngopt_7z_deflate PUBLIC
  7z
  vendor/7z2601/CPP
  vendor/7z2601/C
)
target_link_libraries(apngopt_7z_deflate PUBLIC ZLIB::ZLIB)

if(NOT MSVC)
  target_compile_options(apngopt_7z_deflate PRIVATE
    -Wno-sign-compare
    -Wno-reorder
    -Wno-maybe-uninitialized
    -Wno-parentheses
  )
endif()

if(APNGOPT_ENABLE_IMAGEQUANT)
  find_program(CARGO_EXECUTABLE cargo REQUIRED)

  set(APNGOPT_IMAGEQUANT_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/vendor/libimagequant/Cargo.toml")
  set(APNGOPT_IMAGEQUANT_TARGET_DIR "${CMAKE_BINARY_DIR}/imagequant-target")
  set(APNGOPT_IMAGEQUANT_CARGO_ARGS build --manifest-path ${APNGOPT_IMAGEQUANT_MANIFEST} -p imagequant-sys --release)
  set(APNGOPT_IMAGEQUANT_OUTPUT_DIR "${APNGOPT_IMAGEQUANT_TARGET_DIR}/release")

  if(WIN32)
    if(NOT MSVC)
      message(FATAL_ERROR "imagequant build on Windows requires MSVC toolchain.")
    endif()
    set(APNGOPT_IMAGEQUANT_CARGO_TARGET "x86_64-pc-windows-msvc")
    list(APPEND APNGOPT_IMAGEQUANT_CARGO_ARGS --target ${APNGOPT_IMAGEQUANT_CARGO_TARGET})
    set(APNGOPT_IMAGEQUANT_OUTPUT_DIR "${APNGOPT_IMAGEQUANT_TARGET_DIR}/${APNGOPT_IMAGEQUANT_CARGO_TARGET}/release")
    set(APNGOPT_IMAGEQUANT_LIB "${APNGOPT_IMAGEQUANT_OUTPUT_DIR}/imagequant_sys.lib")
  else()
    set(APNGOPT_IMAGEQUANT_LIB "${APNGOPT_IMAGEQUANT_OUTPUT_DIR}/libimagequant_sys.a")
  endif()

  add_custom_target(imagequant_sys_build
    COMMAND ${CMAKE_COMMAND} -E env
      CARGO_TARGET_DIR=${APNGOPT_IMAGEQUANT_TARGET_DIR}
      ${CARGO_EXECUTABLE} ${APNGOPT_IMAGEQUANT_CARGO_ARGS}
    BYPRODUCTS ${APNGOPT_IMAGEQUANT_LIB}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/vendor/libimagequant
    COMMENT "Building vendored imagequant-sys with Cargo"
    VERBATIM
  )

  add_library(imagequant_sys STATIC IMPORTED GLOBAL)
  set_target_properties(imagequant_sys PROPERTIES
    IMPORTED_LOCATION ${APNGOPT_IMAGEQUANT_LIB}
    INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/vendor/libimagequant/imagequant-sys
  )
  add_dependencies(imagequant_sys imagequant_sys_build)
endif()

add_executable(apngopt apngopt.cpp)
target_include_directories(apngopt PRIVATE 7z vendor/libimagequant/imagequant-sys)
target_link_libraries(apngopt PRIVATE
  PNG::PNG
  ZLIB::ZLIB
  apngopt_7z_deflate
  apngopt_zopfli
)

if(APNGOPT_ENABLE_IMAGEQUANT)
  target_link_libraries(apngopt PRIVATE imagequant_sys)
endif()

if(WIN32)
  target_link_libraries(apngopt PRIVATE ws2_32 userenv ntdll)
endif()
