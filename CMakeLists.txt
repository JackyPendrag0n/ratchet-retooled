cmake_minimum_required(VERSION 3.20)
project(tod_recomp CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(TOD_RENDERER "Build the Vulkan renderer and SDL2 window (off = headless test build)" ON)
option(TOD_HOST_ONLY "Compile only the host runtime (no generated code needed; used by CI)" OFF)

if(MSVC)
  message(FATAL_ERROR "MSVC is not supported (the code uses GCC/Clang extensions). On Windows, build with "
                      "MinGW-w64 GCC or Clang from MSYS2 (UCRT64), see README.md.")
endif()
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$" AND NOT CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
  message(WARNING "TOD Recomp needs an x86-64 target (SSE4 intrinsics). On Apple Silicon configure with "
                  "-DCMAKE_OSX_ARCHITECTURES=x86_64 and run under Rosetta 2.")
endif()

# Generated code relies on wrapping arithmetic and type punning through memcpy only.
add_compile_options(-fwrapv -fno-strict-aliasing -Wno-unused-label)
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-march=x86-64-v2 HAVE_X86_64_V2)
if(HAVE_X86_64_V2)
  add_compile_options(-march=x86-64-v2)
else()
  add_compile_options(-msse4.2 -mpopcnt -mcx16)
endif()
if(WIN32)
  # C99/POSIX printf formats (%zu, %lld) from MinGW's own stdio, not the old msvcrt one
  add_compile_definitions(__USE_MINGW_ANSI_STDIO=1 _FILE_OFFSET_BITS=64 SDL_MAIN_HANDLED NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

# ---- generated code (python3 ppu_codegen.py EBOOT.elf gen); derived from your own game binary, never committed
set(GEN_DIR ${CMAKE_SOURCE_DIR}/gen CACHE PATH "generated PPU sources")
set(GEN_SPU_DIR ${CMAKE_SOURCE_DIR}/gen_spu CACHE PATH "generated SPU sources")
if(NOT TOD_HOST_ONLY AND NOT EXISTS ${GEN_DIR}/sources.txt)
  message(FATAL_ERROR "No generated code in ${GEN_DIR}. Run first:\n"
                      "  python3 ppu_codegen.py /path/to/EBOOT.elf gen\n"
                      "  python3 spu_codegen.py /path/to/EBOOT.elf gen_spu\n"
                      "using the decrypted EBOOT.elf from your own copy of the game (see README.md).")
endif()

file(GLOB HOST_SOURCES host/*.cpp)
if(NOT TOD_RENDERER)
  list(FILTER HOST_SOURCES EXCLUDE REGEX "(vk_backend|vkn_capture|vkn_render|rsx_fp|rsx_vp|rsx_tex|window|overlay|cheat_menu)\\.cpp$")
endif()

if(TOD_HOST_ONLY)
  # compile check only: everything that doesn't need the game's generated tables
  add_library(tod_host OBJECT ${HOST_SOURCES} runtime/platform.cpp)
  target_include_directories(tod_host PRIVATE runtime host)
  set(TOD_TARGET tod_host)
else()
  file(STRINGS ${GEN_DIR}/sources.txt GEN_SOURCES)
  list(TRANSFORM GEN_SOURCES PREPEND ${GEN_DIR}/)
  add_library(ppu_code STATIC ${GEN_SOURCES} runtime/ppu_runtime.cpp runtime/platform.cpp)
  target_include_directories(ppu_code PUBLIC runtime ${GEN_DIR})
  target_link_libraries(ppu_code PUBLIC Threads::Threads)

  if(NOT WIN32)
    add_executable(startup_check harness/startup_check.cpp)
    target_link_libraries(startup_check ppu_code)
  endif()

  add_executable(tod ${HOST_SOURCES})
  # Recompiled SPU code (spu_codegen.py EBOOT.elf gen_spu); without it SPU code is interpreted.
  if(EXISTS ${GEN_SPU_DIR}/sources.txt)
    file(STRINGS ${GEN_SPU_DIR}/sources.txt GEN_SPU_SOURCES)
    list(TRANSFORM GEN_SPU_SOURCES PREPEND ${GEN_SPU_DIR}/)
    add_library(spu_code STATIC ${GEN_SPU_SOURCES})
    target_include_directories(spu_code PUBLIC host runtime ${GEN_SPU_DIR})
    target_link_libraries(tod PRIVATE spu_code)
    target_compile_definitions(tod PRIVATE TOD_SPU_RECOMP=1)
  endif()
  target_link_libraries(tod PRIVATE ppu_code)
  set(TOD_TARGET tod)
  if(WIN32)
    # no libgcc / libstdc++ DLLs to ship next to tod.exe
    # 8 MiB stacks for every thread, as on Linux (the PE default is 2 MiB; deep guest call chains need more)
    target_link_options(tod PRIVATE -static-libgcc -static-libstdc++ -Wl,--stack,8388608)
  endif()
endif()

target_link_libraries(${TOD_TARGET} PRIVATE Threads::Threads ZLIB::ZLIB)
if(TOD_HOST_ONLY)
  target_include_directories(${TOD_TARGET} PRIVATE ${GEN_DIR})
endif()

if(TOD_RENDERER)
  target_compile_definitions(${TOD_TARGET} PRIVATE TOD_RENDERER=1)
  find_package(Vulkan REQUIRED)
  find_path(SHADERC_INCLUDE_DIR shaderc/shaderc.h HINTS ${Vulkan_INCLUDE_DIRS} $ENV{VULKAN_SDK}/include)
  find_library(SHADERC_LIB NAMES shaderc_shared shaderc_combined shaderc HINTS $ENV{VULKAN_SDK}/lib REQUIRED)
  find_package(SDL2 REQUIRED)
  if(TARGET SDL2::SDL2)
    set(TOD_SDL2 SDL2::SDL2)
  else()
    set(TOD_SDL2 ${SDL2_LIBRARIES})
    target_include_directories(${TOD_TARGET} PRIVATE ${SDL2_INCLUDE_DIRS})
  endif()
  if(SHADERC_INCLUDE_DIR)
    target_include_directories(${TOD_TARGET} PRIVATE ${SHADERC_INCLUDE_DIR})
  endif()
  # Dear ImGui (MIT, third_party/imgui) for the cheat menu overlay
  add_library(imgui STATIC third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp
              third_party/imgui/imgui_widgets.cpp third_party/imgui/imgui_impl_vulkan.cpp)
  target_include_directories(imgui PUBLIC third_party/imgui)
  target_link_libraries(imgui PUBLIC Vulkan::Vulkan)
  target_link_libraries(${TOD_TARGET} PRIVATE imgui Vulkan::Vulkan ${SHADERC_LIB} ${TOD_SDL2})
endif()
