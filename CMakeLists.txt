cmake_minimum_required(VERSION 3.15)
project(rbterm C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(
  raylib
  GIT_REPOSITORY https://github.com/raysan5/raylib.git
  GIT_TAG        5.5
  GIT_SHALLOW    TRUE
)
set(BUILD_EXAMPLES   OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES      OFF CACHE BOOL "" FORCE)
set(SUPPORT_MODULE_RAUDIO OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(raylib)

add_executable(rbterm
  src/main.c
  src/screen.c
  src/render.c
  src/input.c
)
target_include_directories(rbterm PRIVATE src)
target_link_libraries(rbterm PRIVATE raylib)

if(APPLE)
  target_link_libraries(rbterm PRIVATE
    "-framework Cocoa" "-framework IOKit" "-framework CoreVideo" "-framework OpenGL")
endif()
if(UNIX AND NOT APPLE)
  target_link_libraries(rbterm PRIVATE util m pthread dl)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(rbterm PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-unused-function)
endif()
