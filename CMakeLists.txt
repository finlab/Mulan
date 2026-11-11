cmake_minimum_required(VERSION 3.20)

# 版本号唯一来源：仓库根的 VERSION 文件（主.次.build）
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" MUC_VERSION_RAW)
string(STRIP "${MUC_VERSION_RAW}" MUC_VERSION)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

project(muc VERSION ${MUC_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# ---- LLVM 是唯一后端（必需） ----
# 指定方式（任一即可）：
#   - -DLLVM_DIR=<path>/lib/cmake/llvm
#   - 环境变量 LLVM_DIR
#   - MSYS2 默认安装位置（自动探测，仅作提示）
if(NOT LLVM_DIR)
  foreach(_llvm_hint
      "C:/msys64/mingw64/lib/cmake/llvm"
      "C:/msys64/clang64/lib/cmake/llvm"
      "C:/msys64/ucrt64/lib/cmake/llvm")
    if(EXISTS "${_llvm_hint}/LLVMConfig.cmake")
      set(LLVM_DIR "${_llvm_hint}" CACHE PATH "LLVM cmake dir")
      break()
    endif()
  endforeach()
  unset(_llvm_hint)
endif()
find_package(LLVM CONFIG REQUIRED)
message(STATUS "muc: LLVM ${LLVM_PACKAGE_VERSION} (${LLVM_INSTALL_PREFIX})")

add_executable(muc
  src/main.cpp
  src/loader.cpp
  src/lexer.cpp
  src/parser.cpp
  src/parser_unit.cpp
  src/parser_stmt.cpp
  src/parser_expr.cpp
  src/types.cpp
  src/mangle.cpp
  src/sema.cpp
  src/sema_decl.cpp
  src/sema_types.cpp
  src/sema_class1.cpp
  src/sema_class2.cpp
  src/sema_const.cpp
  src/sema_expr.cpp
  src/sema_call.cpp
  src/codegen_llvm.cpp
  src/codegen_support.cpp
  src/codegen_runtime.cpp
  src/codegen_expr.cpp
  src/codegen_expr2.cpp
  src/codegen_stmt.cpp
  src/codegen_class.cpp
  src/codegen_func.cpp
)

separate_arguments(LLVM_DEFS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")
target_compile_definitions(muc PRIVATE MUC_VERSION="${MUC_VERSION}" ${LLVM_DEFS_LIST})
target_include_directories(muc SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})

if(MSVC)
  target_compile_options(muc PRIVATE /W4 /WX-)
else()
  target_compile_options(muc PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(TARGET LLVM)
  target_link_libraries(muc PRIVATE LLVM)
else()
  llvm_map_components_to_libnames(LLVM_LIBS core support target x86codegen x86desc x86info)
  target_link_libraries(muc PRIVATE ${LLVM_LIBS})
endif()
