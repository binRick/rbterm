cmake_minimum_required(VERSION 3.15)
project(rbterm C)

if(APPLE)
    enable_language(OBJC)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Fetch raylib. Disable raylib's examples/games/audio to keep builds lean.
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

set(RBTERM_SOURCES
    src/main.c
    src/screen.c
    src/render.c
    src/input.c
)

# Platform-specific backends: PTY + emoji rasterizer.
if(WIN32)
    list(APPEND RBTERM_SOURCES src/pty_win.c src/emoji_stub.c)
elseif(APPLE)
    list(APPEND RBTERM_SOURCES src/pty_unix.c src/emoji_mac.m)
else()
    list(APPEND RBTERM_SOURCES src/pty_unix.c src/emoji_stub.c)
endif()

add_executable(rbterm ${RBTERM_SOURCES})
target_include_directories(rbterm PRIVATE src)
target_link_libraries(rbterm PRIVATE raylib)

if(APPLE)
  target_link_libraries(rbterm PRIVATE
    "-framework Cocoa" "-framework IOKit" "-framework CoreVideo" "-framework OpenGL"
    "-framework CoreText" "-framework CoreGraphics" "-framework Foundation")
  set_source_files_properties(src/emoji_mac.m PROPERTIES LANGUAGE OBJC)
  target_compile_options(rbterm PRIVATE $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)
endif()

if(UNIX AND NOT APPLE)
  target_link_libraries(rbterm PRIVATE util m pthread dl)
endif()

if(WIN32)
  # ConPTY lives in kernel32 on modern Windows (10 1809+); linking
  # explicitly keeps older toolchains happy.
  target_link_libraries(rbterm PRIVATE kernel32 user32 gdi32 winmm)
  target_compile_definitions(rbterm PRIVATE _CRT_SECURE_NO_WARNINGS
                                             NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(rbterm PRIVATE /W3)
  endif()
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(rbterm PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-unused-function)
endif()
