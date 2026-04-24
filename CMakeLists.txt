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
# raylib's CMake doesn't auto-detect emscripten — have to pin the
# platform before MakeAvailable or it tries to find X11 and fails.
if(EMSCRIPTEN)
  set(PLATFORM Web        CACHE STRING "" FORCE)
  set(GRAPHICS GRAPHICS_API_OPENGL_ES2 CACHE STRING "" FORCE)
endif()
FetchContent_MakeAvailable(raylib)

option(RBTERM_SSH "Build with embedded SSH support via libssh" ON)
option(RBTERM_WEB "Build the WebAssembly demo (requires emscripten)" OFF)

# Emscripten → WebAssembly demo build. No forked shell, no SSH, no
# signals — just a fake shell backed by an in-memory tree. Auto-detect
# an emcc toolchain so plain `emcmake cmake …` flips the switch.
if(EMSCRIPTEN)
    set(RBTERM_WEB ON CACHE BOOL "" FORCE)
    set(RBTERM_SSH OFF CACHE BOOL "" FORCE)
endif()

set(RBTERM_SOURCES
    src/main.c
    src/screen.c
    src/render.c
    src/input.c
    src/theme.c
    src/sixel.c
    src/kitty.c
    src/pty_dispatch.c
)

# Embedded fonts. Three platform paths share one fonts_embedded.h:
#   * Mac/Linux  → fonts_embedded.S (.incbin into __TEXT/.rodata).
#   * Windows    → fonts_embedded.rc (RCDATA) + fonts_embedded_win.c
#                  (runtime FindResource/LoadResource fill).
#   * Web        → no embed; assets/fonts is preloaded into MEMFS
#                  and main.c uses an empty stub via PLATFORM_WEB.
if(EMSCRIPTEN)
    add_compile_definitions(RBTERM_NO_EMBEDDED_FONTS=1)
elseif(WIN32)
    list(APPEND RBTERM_SOURCES src/fonts_embedded.rc src/fonts_embedded_win.c)
else()
    list(APPEND RBTERM_SOURCES src/fonts_embedded.S)
endif()

# Regenerate src/themes_embedded.h whenever any kfc/dark palette file
# (or the generator script) changes. Theme.c #includes the header, so
# this fires before it compiles.
file(GLOB _THEME_SOURCES
  "${CMAKE_SOURCE_DIR}/third_party/pal/palettes/kfc/dark/*")
add_custom_command(
  OUTPUT  ${CMAKE_SOURCE_DIR}/src/themes_embedded.h
  COMMAND ${CMAKE_SOURCE_DIR}/tools/gen_themes.sh
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/gen_themes.sh ${_THEME_SOURCES}
  COMMENT "Regenerating src/themes_embedded.h from pal/palettes/kfc/dark"
  VERBATIM)
add_custom_target(rbterm_themes DEPENDS ${CMAKE_SOURCE_DIR}/src/themes_embedded.h)

# Regenerate src/fonts_embedded.{h,S} whenever a font in assets/fonts/
# (or the generator script) changes. fonts_embedded.S is then linked
# directly into the executable so the binary carries the font data.
# .incbin keeps the per-font assembler input tiny — compile is instant
# even with ~13 MB of font payload.
file(GLOB _FONT_SOURCES
  "${CMAKE_SOURCE_DIR}/assets/fonts/*.ttf"
  "${CMAKE_SOURCE_DIR}/assets/fonts/*.otf"
  "${CMAKE_SOURCE_DIR}/assets/fonts/*.ttc")

# Invoke the generator via bash explicitly — Git Bash on Windows
# runners doesn't honour the .sh shebang otherwise.
find_program(BASH_EXE bash)
if(NOT BASH_EXE)
    set(BASH_EXE bash)
endif()

add_custom_command(
  OUTPUT  ${CMAKE_SOURCE_DIR}/src/fonts_embedded.h
          ${CMAKE_SOURCE_DIR}/src/fonts_embedded.S
          ${CMAKE_SOURCE_DIR}/src/fonts_embedded.rc
          ${CMAKE_SOURCE_DIR}/src/fonts_embedded_win.c
  COMMAND ${BASH_EXE} ${CMAKE_SOURCE_DIR}/tools/gen_fonts.sh
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/gen_fonts.sh ${_FONT_SOURCES}
  COMMENT "Regenerating src/fonts_embedded.{h,S,rc} + _win.c from assets/fonts/"
  VERBATIM)
add_custom_target(rbterm_fonts
  DEPENDS ${CMAKE_SOURCE_DIR}/src/fonts_embedded.h
          ${CMAKE_SOURCE_DIR}/src/fonts_embedded.S
          ${CMAKE_SOURCE_DIR}/src/fonts_embedded.rc
          ${CMAKE_SOURCE_DIR}/src/fonts_embedded_win.c)
if(NOT WIN32)
    enable_language(ASM)
endif()

# Platform-specific backends: local PTY + emoji rasterizer.
if(RBTERM_WEB)
    list(APPEND RBTERM_SOURCES src/pty_fake.c src/emoji_stub.c)
elseif(WIN32)
    list(APPEND RBTERM_SOURCES src/pty_win.c src/emoji_stub.c)
elseif(APPLE)
    list(APPEND RBTERM_SOURCES src/pty_unix.c src/emoji_mac.m)
else()
    list(APPEND RBTERM_SOURCES src/pty_unix.c src/emoji_stub.c)
endif()

# SSH backend is optional — libssh + mbedTLS integration via FetchContent
# is fiddly on Windows so CI builds pass -DRBTERM_SSH=OFF and ship a stub
# that reports "not built in this binary". Linux / macOS builds pick it
# up normally.
if(RBTERM_SSH)
    list(APPEND RBTERM_SOURCES src/pty_ssh.c)

    find_package(libssh QUIET)
    if(NOT libssh_FOUND)
        message(STATUS "libssh not installed — fetching via FetchContent")

        FetchContent_Declare(
          mbedtls
          GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
          GIT_TAG        v3.6.2
          GIT_SHALLOW    TRUE
        )
        set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
        set(ENABLE_TESTING  OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(mbedtls)

        FetchContent_Declare(
          libssh
          GIT_REPOSITORY https://gitlab.com/libssh/libssh-mirror.git
          GIT_TAG        libssh-0.11.1
          GIT_SHALLOW    TRUE
        )
        set(WITH_MBEDTLS   ON  CACHE BOOL "" FORCE)
        set(WITH_GSSAPI    OFF CACHE BOOL "" FORCE)
        set(WITH_SERVER    OFF CACHE BOOL "" FORCE)
        set(WITH_EXAMPLES  OFF CACHE BOOL "" FORCE)
        set(WITH_ZLIB      OFF CACHE BOOL "" FORCE)
        set(WITH_NACL      OFF CACHE BOOL "" FORCE)
        set(UNIT_TESTING   OFF CACHE BOOL "" FORCE)
        set(CLIENT_TESTING OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(libssh)
    endif()
else()
    list(APPEND RBTERM_SOURCES src/pty_ssh_stub.c)
endif()

if(WIN32)
    # Embed the icon so Explorer / Alt-Tab / taskbar use rbterm.ico
    # instead of the default generic exe icon.
    list(APPEND RBTERM_SOURCES assets/rbterm.rc)
endif()

add_executable(rbterm ${RBTERM_SOURCES})
add_dependencies(rbterm rbterm_themes)
if(NOT EMSCRIPTEN)
    add_dependencies(rbterm rbterm_fonts)
endif()
target_include_directories(rbterm PRIVATE src)
target_link_libraries(rbterm PRIVATE raylib)
if(RBTERM_SSH)
    target_link_libraries(rbterm PRIVATE ssh)
endif()

if(APPLE)
  target_link_libraries(rbterm PRIVATE
    "-framework Cocoa" "-framework IOKit" "-framework CoreVideo" "-framework OpenGL"
    "-framework CoreText" "-framework CoreGraphics" "-framework Foundation")
  set_source_files_properties(src/emoji_mac.m PROPERTIES LANGUAGE OBJC)
  target_compile_options(rbterm PRIVATE $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)
endif()

if(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
  target_link_libraries(rbterm PRIVATE util m pthread dl)
endif()

if(EMSCRIPTEN)
  # raylib's CMake autodetects PLATFORM=Web when EMSCRIPTEN is set,
  # but we still need to hand emcc the right flags for the final link.
  # ASYNCIFY lets us keep the regular blocking main loop; font files
  # get preloaded into emscripten's virtual FS so fonts_load finds them.
  set_target_properties(rbterm PROPERTIES SUFFIX ".html")
  target_link_options(rbterm PRIVATE
    "SHELL:-s USE_GLFW=3"
    "SHELL:-s ASYNCIFY=1"
    "SHELL:-s ALLOW_MEMORY_GROWTH=1"
    "SHELL:-s INITIAL_MEMORY=64MB"
    "SHELL:-s STACK_SIZE=8MB"
    "SHELL:-s FORCE_FILESYSTEM=1"
    # Wasm doesn't link the embedded-fonts .S (Mach-O syntax + bundle
    # bloat), so preload assets/fonts into MEMFS at /fonts/ so
    # renderer_find_default_font can still locate a startup font.
    "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/assets/fonts@fonts"
    "SHELL:--shell-file ${CMAKE_SOURCE_DIR}/web/shell.html"
  )
  target_compile_definitions(rbterm PRIVATE PLATFORM_WEB=1)
endif()

if(WIN32)
  # ConPTY lives in kernel32 on modern Windows (10 1809+); linking
  # explicitly keeps older toolchains happy.
  target_link_libraries(rbterm PRIVATE kernel32 user32 gdi32 winmm shell32)
  target_compile_definitions(rbterm PRIVATE _CRT_SECURE_NO_WARNINGS
                                             NOMINMAX WIN32_LEAN_AND_MEAN)
  # Ship as a GUI-subsystem binary so Windows doesn't open the black
  # console window beside ours. Keep `main()` as the entry point via
  # /ENTRY:mainCRTStartup (MSVC) or -mwindows (MinGW).
  if(MSVC)
    set_target_properties(rbterm PROPERTIES
      LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
    target_compile_options(rbterm PRIVATE /W3)
  else()
    target_link_options(rbterm PRIVATE -mwindows)
  endif()
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(rbterm PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-unused-function)
endif()
