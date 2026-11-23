# x4r's own tools on native Windows (the Linux build is tools/build-tools.sh): the translators against an LLVM 23 release.
#   cmake -S x4r -B x4r/build-win -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang-cl -DLLVM_DIR=<llvm>/lib/cmake/llvm
# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.20)
project(x4r_tools CXX)
# the LLVM release's exports name its optional compression/XML dependencies as imported targets; the tools use none of them
foreach(t ZLIB::ZLIB zstd::libzstd_static zstd::libzstd_shared LibXml2::LibXml2)
  if(NOT TARGET ${t})
    add_library(${t} INTERFACE IMPORTED)
  endif()
endforeach()
find_package(LLVM REQUIRED CONFIG)
llvm_map_components_to_libnames(LLVM_LIBS core irreader support analysis)
foreach(tool x4rwave x4rsim x4rtrans x4rdep x4rpress)
  add_executable(${tool} src/${tool}.cpp)
  target_include_directories(${tool} PRIVATE ${LLVM_INCLUDE_DIRS})
  target_compile_definitions(${tool} PRIVATE ${LLVM_DEFINITIONS_LIST} _CRT_SECURE_NO_WARNINGS NOMINMAX)
  target_link_libraries(${tool} PRIVATE ${LLVM_LIBS})
  set_target_properties(${tool} PROPERTIES CXX_STANDARD 17 MSVC_RUNTIME_LIBRARY "MultiThreaded")
endforeach()
set_target_properties(x4rsim PROPERTIES CXX_STANDARD 20)
