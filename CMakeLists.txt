cmake_minimum_required(VERSION 3.20)

# Single source of truth for the version: the VERSION file at the repository root.
# It records two independent numbers on labelled lines (see docs/versions.md):
#   version = <major.minor.patch>  release version; changes only on an explicit release
#   build   = <n>                  monotonic build counter; +1 per compiler-code change
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" MUC_VERSION_FILE)
string(REGEX MATCH "version[ \t]*=[ \t]*([0-9]+\\.[0-9]+\\.[0-9]+)" _muc_v "${MUC_VERSION_FILE}")
if(NOT CMAKE_MATCH_1)
  message(FATAL_ERROR "muc: VERSION has no `version = major.minor.patch` line")
endif()
set(MUC_VERSION "${CMAKE_MATCH_1}")
string(REGEX MATCH "build[ \t]*=[ \t]*([0-9]+)" _muc_b "${MUC_VERSION_FILE}")
if(NOT CMAKE_MATCH_1)
  message(FATAL_ERROR "muc: VERSION has no `build = <n>` line")
endif()
set(MUC_BUILD "${CMAKE_MATCH_1}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

project(muc VERSION ${MUC_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# ---- LLVM is the only backend (required) ----
# Ways to point at it (any one is enough):
#   - -DLLVM_DIR=<path>/lib/cmake/llvm
#   - the LLVM_DIR environment variable
#   - the default MSYS2 install location (probed automatically, for convenience only)
if(NOT LLVM_DIR)
  foreach(_llvm_hint
      "C:/msys64/ucrt64/lib/cmake/llvm"
      "C:/msys64/mingw64/lib/cmake/llvm"
      "C:/msys64/clang64/lib/cmake/llvm")
    if(EXISTS "${_llvm_hint}/LLVMConfig.cmake")
      set(LLVM_DIR "${_llvm_hint}" CACHE PATH "LLVM cmake dir")
      break()
    endif()
  endforeach()
  unset(_llvm_hint)
endif()
find_package(LLVM CONFIG REQUIRED)
# LLVM's config-version file only accepts the same major version, so the minimum
# requirement is checked explicitly instead of via find_package(VERSION ...).
if(LLVM_PACKAGE_VERSION VERSION_LESS 17)
  message(FATAL_ERROR "muc: LLVM >= 17 is required, found ${LLVM_PACKAGE_VERSION}")
endif()
message(STATUS "muc: LLVM ${LLVM_PACKAGE_VERSION} (${LLVM_INSTALL_PREFIX})")

add_executable(muc
  src/main.cpp
  src/loader.cpp
  src/directive.cpp
  src/ast/ast_clone.cpp
  src/ast/ast_subst.cpp
  src/types/types.cpp
  src/types/mangle.cpp
  src/parser/lexer.cpp
  src/parser/parser.cpp
  src/parser/parser_unit.cpp
  src/parser/parser_type_decl.cpp
  src/parser/parser_stmt.cpp
  src/parser/parser_expr.cpp
  src/parser/parser_type.cpp
  src/parser/parser_directive.cpp
  src/sema/sema.cpp
  src/sema/sema_run.cpp
  src/sema/sema_decl.cpp
  src/sema/sema_resolve_types.cpp
  src/sema/sema_layout.cpp
  src/sema/sema_directive.cpp
  src/sema/sema_types.cpp
  src/sema/sema_generic.cpp
  src/sema/sema_constraint.cpp
  src/sema/sema_class1.cpp
  src/sema/sema_iface.cpp
  src/sema/sema_stmt.cpp
  src/sema/sema_class2.cpp
  src/sema/sema_const.cpp
  src/sema/sema_expr.cpp
  src/sema/sema_member.cpp
  src/sema/sema_lambda.cpp
  src/sema/sema_call.cpp
  src/sema/sema_resolve.cpp
  src/codegen/codegen_llvm.cpp
  src/codegen/codegen_abi.cpp
  src/codegen/codegen_support.cpp
  src/codegen/codegen_runtime.cpp
  src/codegen/codegen_cleanup.cpp
  src/codegen/codegen_eh.cpp
  src/codegen/may_throw.cpp
  src/codegen/codegen_expr.cpp
  src/codegen/codegen_expr2.cpp
  src/codegen/codegen_stmt.cpp
  src/codegen/codegen_mem.cpp
  src/codegen/codegen_class.cpp
  src/codegen/codegen_func.cpp
  src/codegen/codegen_unit.cpp
  src/codegen/abi.cpp
)

# Headers are included flat (`#include "sema_impl.hpp"`), so every source directory is on the include
# path. Moving a file between directories therefore needs no include edits.
target_include_directories(muc PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ast
  ${CMAKE_CURRENT_SOURCE_DIR}/src/types
  ${CMAKE_CURRENT_SOURCE_DIR}/src/parser
  ${CMAKE_CURRENT_SOURCE_DIR}/src/sema
  ${CMAKE_CURRENT_SOURCE_DIR}/src/codegen
)

separate_arguments(LLVM_DEFS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")
target_compile_definitions(muc PRIVATE MUC_VERSION="${MUC_VERSION}" MUC_BUILD="${MUC_BUILD}" ${LLVM_DEFS_LIST})
target_include_directories(muc SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})

if(MSVC)
  target_compile_options(muc PRIVATE /W4 /WX-)
  # The parser and the AST walkers recurse, but the input is capped in the parser (kMaxNestDepth =
  # 320 levels, ~0.9 MB of frames). Keep the reserve at 8 MB for headroom on the AST walkers.
  target_link_options(muc PRIVATE /STACK:8388608)
else()
  target_compile_options(muc PRIVATE -Wall -Wextra -Wno-unused-parameter)
  if(WIN32)
    # Same reason as /STACK above (see kMaxNestDepth in src/parser_impl.hpp).
    target_link_options(muc PRIVATE -Wl,--stack,8388608)
  endif()
endif()

if(TARGET LLVM)
  target_link_libraries(muc PRIVATE LLVM)
else()
  # `passes` carries PassBuilder / buildPerModuleDefaultPipeline (codegen_func.cpp). Without it a
  # static LLVM build fails to link those symbols.
  llvm_map_components_to_libnames(LLVM_LIBS passes core support target x86codegen x86desc x86info)
  target_link_libraries(muc PRIVATE ${LLVM_LIBS})
endif()

# Register only the targets this LLVM build contains. `InitializeAllTargets` would reference every
# target LLVM knows about; an x86-only static build then misses `LLVMInitializeLanai/Mips/...`.
if("X86" IN_LIST LLVM_TARGETS_TO_BUILD)
  target_compile_definitions(muc PRIVATE MUC_TARGET_X86)
endif()
if("AArch64" IN_LIST LLVM_TARGETS_TO_BUILD)
  target_compile_definitions(muc PRIVATE MUC_TARGET_AARCH64)
endif()
