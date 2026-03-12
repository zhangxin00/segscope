cmake_minimum_required(VERSION 3.16)
project(intr_detect LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(HandleLLVMOptions)

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

add_library(AutoInstrumentPass MODULE
  src/AutoInstrumentPass.cpp
  src/IntMonBranchPass.cpp
)

target_include_directories(AutoInstrumentPass PRIVATE include)

if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(AutoInstrumentPass PRIVATE -fno-rtti)
endif()

option(AID_LINK_LLVM_STATIC "Link LLVM static libraries into the pass plugin" OFF)

list(GET LLVM_LIBRARY_DIRS 0 LLVM_LIB_DIR)
set(LLVM_DYLIB "${LLVM_LIB_DIR}/libLLVM.so")

if(EXISTS "${LLVM_DYLIB}")
  target_link_libraries(AutoInstrumentPass PRIVATE LLVM)
elseif(AID_LINK_LLVM_STATIC)
  llvm_map_components_to_libnames(LLVM_LIBS Core IRReader Passes Support)
  target_link_libraries(AutoInstrumentPass PRIVATE ${LLVM_LIBS})
else()
  message(WARNING "未发现 libLLVM.so，默认不链接静态 LLVM 库以避免重复注册命令行选项。若加载失败，请使用带共享库的 LLVM 或设置 -DAID_LINK_LLVM_STATIC=ON。")
endif()

set_target_properties(AutoInstrumentPass PROPERTIES
  PREFIX ""
)

option(AID_BUILD_RUNTIME "Build runtime library (legacy hook demo)" OFF)
if(AID_BUILD_RUNTIME)
  add_library(intr_runtime STATIC
    runtime/intr_runtime.c
    runtime/intmon_runtime.c
  )

  target_include_directories(intr_runtime PUBLIC runtime)
endif()
