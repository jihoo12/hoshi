cmake_minimum_required(VERSION 3.20)
project(hoshi LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

add_executable(hoshic
  src/main.cpp
  src/diag.cpp
  src/lexer.cpp
  src/parser.cpp
  src/ast_dump.cpp
  src/types.cpp
  src/sema.cpp
  src/codegen.cpp
  src/driver.cpp
)

target_include_directories(hoshic SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
target_compile_definitions(hoshic PRIVATE ${LLVM_DEFINITIONS_LIST}
  HOSHI_VERSION="0.1.0" HOSHI_LLVM_VERSION="${LLVM_PACKAGE_VERSION}")
target_compile_options(hoshic PRIVATE -Wall -Wextra -Wno-unused-parameter)

if(LLVM_LINK_LLVM_DYLIB)
  set(LLVM_LIBS LLVM)
else()
  llvm_map_components_to_libnames(LLVM_LIBS
    core support passes target targetparser native nativecodegen
    analysis transformutils
  )
endif()
target_link_libraries(hoshic PRIVATE ${LLVM_LIBS})

install(TARGETS hoshic RUNTIME DESTINATION bin)
