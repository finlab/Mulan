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
if(NOT LLVM_DIR AND EXISTS "C:/msys64/mingw64/lib/cmake/llvm/LLVMConfig.cmake")
  set(LLVM_DIR "C:/msys64/mingw64/lib/cmake/llvm" CACHE PATH "LLVM cmake dir")
endif()
find_package(LLVM CONFIG REQUIRED)
message(STATUS "muc: LLVM ${LLVM_PACKAGE_VERSION} (${LLVM_INSTALL_PREFIX})")

add_executable(muc
  src/main.cpp
  src/loader.cpp
  src/lexer.cpp
  src/parser.cpp
  src/types.cpp
  src/mangle.cpp
  src/sema.cpp
  src/codegen_llvm.cpp
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
