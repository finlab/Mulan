cmake_minimum_required(VERSION 3.20)

# 版本号唯一来源：仓库根的 VERSION 文件（主.次.build）
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" MULANC_VERSION_RAW)
string(STRIP "${MULANC_VERSION_RAW}" MULANC_VERSION)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

project(mulanc VERSION ${MULANC_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# ---- LLVM（可选；找不到时只构建 C 后端） ----
option(MULANC_WITH_LLVM "Build the LLVM backend" ON)
if(MULANC_WITH_LLVM AND NOT LLVM_DIR AND EXISTS "C:/msys64/mingw64/lib/cmake/llvm/LLVMConfig.cmake")
  set(LLVM_DIR "C:/msys64/mingw64/lib/cmake/llvm" CACHE PATH "LLVM cmake dir")
endif()

add_executable(mulanc
  src/main.cpp
  src/loader.cpp
  src/lexer.cpp
  src/parser.cpp
  src/types.cpp
  src/sema.cpp
  src/codegen_c.cpp
  src/codegen_llvm.cpp
)

target_compile_definitions(mulanc PRIVATE MULANC_VERSION="${MULANC_VERSION}")

if(MSVC)
  target_compile_options(mulanc PRIVATE /W4 /WX-)
else()
  target_compile_options(mulanc PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(MULANC_WITH_LLVM)
  find_package(LLVM CONFIG QUIET)
  if(LLVM_FOUND)
    message(STATUS "mulanc: LLVM ${LLVM_PACKAGE_VERSION} (${LLVM_INSTALL_PREFIX})")
    separate_arguments(LLVM_DEFS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")
    target_compile_definitions(mulanc PRIVATE MULANC_HAVE_LLVM ${LLVM_DEFS_LIST})
    target_include_directories(mulanc SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
    if(TARGET LLVM)
      target_link_libraries(mulanc PRIVATE LLVM)
    else()
      llvm_map_components_to_libnames(LLVM_LIBS core support target x86codegen x86desc x86info)
      target_link_libraries(mulanc PRIVATE ${LLVM_LIBS})
    endif()
  else()
    message(STATUS "mulanc: LLVM not found，只构建 C 后端")
  endif()
endif()
