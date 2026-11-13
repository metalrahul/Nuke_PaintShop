cmake_minimum_required(VERSION 3.16)
project(NPaintShop CXX)

# Configure with the Nuke install you are building for, e.g.
#   cmake -B build -DNUKE_ROOT="C:/Program Files/Nuke17.0v3"
#   cmake --build build --config Release
# Use the compiler Foundry lists for that Nuke version (the NDK is ABI-sensitive):
# Visual Studio 2022 for Nuke 16 and 17.
set(NUKE_ROOT "" CACHE PATH "Nuke install directory (the one containing include/DDImage)")
option(RP_BUILD_TESTS "Build the standalone core tests (no Nuke needed)" ON)
option(RP_USE_NUKE_CMAKE "Link through Nuke's own NukeConfig.cmake instead of locating DDImage directly" OFF)

# NUKE_ROOT collides with CMake's <PACKAGENAME>_ROOT convention for find_package(Nuke).
if(POLICY CMP0144)
  cmake_policy(SET CMP0144 NEW)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(RP_CORE src/core/Canvas.cpp src/core/Brush.cpp src/core/Descriptor.cpp
            src/core/PaintEngine.cpp src/core/Preview.cpp)
find_package(Threads REQUIRED)

if(RP_BUILD_TESTS)
  add_executable(rp_test_core tests/test_core.cpp ${RP_CORE})
  target_include_directories(rp_test_core PRIVATE src/core)
  target_link_libraries(rp_test_core PRIVATE Threads::Threads)
  # The brush library is filesystem work with no NDK in it, so it is tested without Nuke.
  add_executable(rp_test_library tests/test_library.cpp)
  target_include_directories(rp_test_library PRIVATE src/nuke)

  enable_testing()
  add_test(NAME core COMMAND rp_test_core ${CMAKE_CURRENT_BINARY_DIR})
  add_test(NAME library COMMAND rp_test_library)
endif()

if(NOT NUKE_ROOT)
  message(STATUS "NPaintShop: NUKE_ROOT not set, building the core tests only.")
  message(STATUS "NPaintShop: pass -DNUKE_ROOT=<your Nuke install> to build the plugin.")
  return()
endif()

# ---------------------------------------------------------------- locate Nuke
# Catch a wrong path here, with a message that says what is wrong, rather than
# letting it surface later as a confusing find_library failure.
if(NOT IS_DIRECTORY "${NUKE_ROOT}")
  message(FATAL_ERROR
    "NPaintShop: NUKE_ROOT is not a directory:\n    ${NUKE_ROOT}\n"
    "Check the exact version folder name - Nuke installs are named like "
    "Nuke16.0v2 or Nuke17.0v3, and the patch number must match exactly.")
endif()
if(NOT EXISTS "${NUKE_ROOT}/include/DDImage/Iop.h")
  message(FATAL_ERROR
    "NPaintShop: no NDK headers under NUKE_ROOT:\n    ${NUKE_ROOT}\n"
    "Expected to find:\n    ${NUKE_ROOT}/include/DDImage/Iop.h\n"
    "Point NUKE_ROOT at the folder that contains Nuke's 'include' directory. On macOS "
    "that is <...>/NukeXX.YvZ.app/Contents/MacOS.")
endif()

add_library(NPaintShop MODULE src/nuke/NPaintShop.cpp ${RP_CORE})
target_include_directories(NPaintShop PRIVATE src/core)
set_target_properties(NPaintShop PROPERTIES PREFIX "")  # Nuke wants NPaintShop.so, not libNPaintShop.so
if(APPLE)
  set_target_properties(NPaintShop PROPERTIES SUFFIX ".dylib")
endif()
if(MSVC)
  # _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR is required, not optional.
  # VS 2022 17.8 made std::mutex's constructor constexpr, so a mutex is zero-initialised
  # at compile time rather than set up at runtime. Nuke loads its own, older msvcp140.dll,
  # which does not understand that representation, so the FIRST lock of any std::mutex in
  # this plugin dereferences garbage and takes Nuke down with it. The macro restores the
  # old runtime-initialised constructor. Same fix OpenColorIO needed for the same reason.
  target_compile_definitions(NPaintShop PRIVATE
      NOMINMAX _USE_MATH_DEFINES _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
  target_compile_options(NPaintShop PRIVATE /bigobj /Zi)
  # Symbols for the release build, in a separate NPaintShop.pdb next to the DLL. They
  # cost nothing at run time and turn a crash dump from Nuke into function names and
  # line numbers instead of bare offsets. /OPT:REF and /OPT:ICF keep the DLL the size
  # it was, since /DEBUG would otherwise switch them off.
  target_link_options(NPaintShop PRIVATE /DEBUG /OPT:REF /OPT:ICF)
  if(TARGET rp_test_core)
    target_compile_definitions(rp_test_core PRIVATE NOMINMAX _USE_MATH_DEFINES)
  endif()
endif()

# Nuke ships a CMake package, but in several releases NukeConfig.cmake is installed in
# the install root instead of cmake/, which makes it compute the wrong import prefix
# (Foundry issue 502250). Locating DDImage directly avoids that, so it is the default.
set(RP_LINKED_VIA "DDImage located directly")
if(RP_USE_NUKE_CMAKE)
  find_package(Nuke QUIET CONFIG PATHS "${NUKE_ROOT}/cmake" "${NUKE_ROOT}" NO_DEFAULT_PATH)
  if(TARGET Nuke::NDK)
    target_link_libraries(NPaintShop PRIVATE Nuke::NDK)
    set(RP_LINKED_VIA "Nuke::NDK from NukeConfig.cmake")
  else()
    message(FATAL_ERROR
      "NPaintShop: RP_USE_NUKE_CMAKE is ON but no usable NukeConfig.cmake was found under\n"
      "    ${NUKE_ROOT}\nLeave RP_USE_NUKE_CMAKE OFF to link against DDImage directly.")
  endif()
else()
  # DDImage.lib sits in the install root on Windows; libDDImage.so / .dylib likewise on
  # Linux and macOS. The suffixes cover layouts that differ from that.
  find_library(RP_DDIMAGE
    NAMES DDImage libDDImage
    PATHS "${NUKE_ROOT}"
    PATH_SUFFIXES "" lib lib64 bin "lib/x64"
    NO_DEFAULT_PATH)
  if(NOT RP_DDIMAGE)
    file(GLOB RP_FOUND_LIBS "${NUKE_ROOT}/*DDImage*" "${NUKE_ROOT}/lib/*DDImage*")
    message(FATAL_ERROR
      "NPaintShop: could not find the DDImage library under\n    ${NUKE_ROOT}\n"
      "Looked for DDImage[.lib/.so/.dylib] in that folder and in lib, lib64 and bin.\n"
      "Files matching *DDImage* that were found: ${RP_FOUND_LIBS}\n"
      "If it lives somewhere else, pass its full path with -DRP_DDIMAGE=<path to the library>.")
  endif()
  target_include_directories(NPaintShop PRIVATE "${NUKE_ROOT}/include")
  target_link_libraries(NPaintShop PRIVATE "${RP_DDIMAGE}")
  if(UNIX AND NOT APPLE)
    # Nuke 14 and older were built against the old libstdc++ string ABI; 15+ use the new one.
    option(RP_OLD_ABI "Set ON for Nuke 14 and older on Linux" OFF)
    if(RP_OLD_ABI)
      target_compile_definitions(NPaintShop PRIVATE _GLIBCXX_USE_CXX11_ABI=0)
    endif()
  endif()
endif()

# Layout/hit-testing tests for the viewer overlay. Pure geometry - no GL calls are
# executed - but it needs DDImage/gl.h, so it only exists once NUKE_ROOT is known.
# Not built by default:  cmake --build build --config Release --target rp_test_overlay
# Layout/hit-testing for the overlay, and the layer stack's metadata round trip. Both
# are pure logic - no GL calls run and no Op is created - but they need the DDImage
# headers, so they only exist once NUKE_ROOT is known. Not built by default:
#   cmake --build build --config Release --target rp_test_overlay
#   cmake --build build --config Release --target rp_test_layers
foreach(rp_t overlay layers)
  add_executable(rp_test_${rp_t} EXCLUDE_FROM_ALL tests/test_${rp_t}.cpp ${RP_CORE})
  target_include_directories(rp_test_${rp_t} PRIVATE src/core src/nuke "${NUKE_ROOT}/include")
  target_link_libraries(rp_test_${rp_t} PRIVATE Threads::Threads)
  if(MSVC)
    target_compile_definitions(rp_test_${rp_t} PRIVATE
        NOMINMAX _USE_MATH_DEFINES _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
  endif()
endforeach()

find_package(OpenGL QUIET)
if(TARGET OpenGL::GL)
  target_link_libraries(NPaintShop PRIVATE OpenGL::GL)
elseif(WIN32)
  target_link_libraries(NPaintShop PRIVATE opengl32)
endif()
target_link_libraries(NPaintShop PRIVATE Threads::Threads)

message(STATUS "NPaintShop: building against ${NUKE_ROOT} (${RP_LINKED_VIA})")

install(TARGETS NPaintShop LIBRARY DESTINATION . RUNTIME DESTINATION .)
install(FILES python/menu.py DESTINATION .)
