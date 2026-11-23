# NPaintShop OFX plugin. Built on its own, separately from the Nuke plugin:
#   cmake -S ofx -B build-ofx
#   cmake --build build-ofx --config Release
# It needs no Nuke install: the OpenFX SDK is open source and is downloaded here.
cmake_minimum_required(VERSION 3.16)
project(NPaintShopOFX CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(openfx
  GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/openfx.git
  GIT_TAG OFX_Release_1.5.1
  GIT_SHALLOW TRUE)
# Only the headers and the C++ support library are used, so the SDK's own build
# (examples, tests, Conan) is never run.
FetchContent_GetProperties(openfx)
if(NOT openfx_POPULATED)
  FetchContent_Populate(openfx)
endif()
set(OFX ${openfx_SOURCE_DIR})

find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

file(GLOB OFX_SUPPORT_SRC ${OFX}/Support/Library/*.cpp)
set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/../src/core)
add_library(NPaintShopOFX MODULE
  NPaintShopOFX.cpp
  ${OFX_SUPPORT_SRC}
  ${CORE}/Canvas.cpp ${CORE}/Brush.cpp ${CORE}/Descriptor.cpp ${CORE}/PaintEngine.cpp ${CORE}/Preview.cpp)
target_include_directories(NPaintShopOFX PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shim ${OFX}/include ${OFX}/Support/include ${CORE} ${CMAKE_CURRENT_SOURCE_DIR}/../src/nuke)
target_link_libraries(NPaintShopOFX PRIVATE OpenGL::GL Threads::Threads)
set_target_properties(NPaintShopOFX PROPERTIES PREFIX "" SUFFIX ".ofx")
if(MSVC)
  # Same reason as the Nuke plugin: hosts ship an older msvcp140.dll.
  target_compile_definitions(NPaintShopOFX PRIVATE _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR NOMINMAX)
  target_compile_options(NPaintShopOFX PRIVATE /bigobj)
else()
  # Keep the plugin's own symbols private: two OFX plugins in one host that both
  # carry the support library must not resolve each other's copies.
  set_target_properties(NPaintShopOFX PROPERTIES CXX_VISIBILITY_PRESET hidden)
  target_link_options(NPaintShopOFX PRIVATE -static-libstdc++ -static-libgcc)
endif()

# OFX hosts load plugins from a bundle folder: Name.ofx.bundle/Contents/<arch>/Name.ofx
if(WIN32)
  set(OFX_ARCH Win64)
elseif(APPLE)
  set(OFX_ARCH MacOS)
else()
  set(OFX_ARCH Linux-x86-64)
endif()
set(BUNDLE ${CMAKE_BINARY_DIR}/NPaintShopOFX.ofx.bundle)
add_custom_command(TARGET NPaintShopOFX POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUNDLE}/Contents/${OFX_ARCH}
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:NPaintShopOFX> ${BUNDLE}/Contents/${OFX_ARCH}/
  COMMENT "Bundle: ${BUNDLE}")
