cmake_minimum_required(VERSION 3.16)
# C is needed only for the glib-compile-resources output (a generated .c file).
project(mb-powermon-gui LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTKMM REQUIRED IMPORTED_TARGET gtkmm-4.0)

# HailoRT C++ runtime (no pkg-config shipped) — headers under /usr/local/include,
# libhailort in /usr/local/lib. Genuinely optional: hosts without a Hailo-8 (e.g.
# the Qualcomm IQ-9075 EVK, whose NPU is the on-SoC Hexagon NSP) build fine
# without it — the Hailo probe is simply compiled out.
find_library(HAILORT_LIB hailort PATHS /usr/local/lib /usr/lib)
find_path(HAILORT_INC hailo/hailort.hpp PATHS /usr/local/include /usr/include)
if(HAILORT_LIB AND HAILORT_INC)
  set(MB_HAVE_HAILO ON)
else()
  set(MB_HAVE_HAILO OFF)
  message(STATUS "libhailort not found — building without Hailo-8 support")
endif()

# Compile assets/ into the binary as a GResource. The About dialog loads its logo
# with Gdk::Texture::create_from_resource(), which finds nothing unless this runs.
find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)
set(GRESOURCE_XML ${CMAKE_CURRENT_SOURCE_DIR}/mb-powermon-gui.gresource.xml)
set(GRESOURCE_C ${CMAKE_CURRENT_BINARY_DIR}/mb-powermon-gui.gresource.c)
execute_process(
  COMMAND ${GLIB_COMPILE_RESOURCES} --generate-dependencies
          --sourcedir=${CMAKE_CURRENT_SOURCE_DIR} ${GRESOURCE_XML}
  OUTPUT_VARIABLE GRESOURCE_DEPS OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REPLACE "\n" ";" GRESOURCE_DEPS "${GRESOURCE_DEPS}")  # already absolute
add_custom_command(
  OUTPUT ${GRESOURCE_C}
  COMMAND ${GLIB_COMPILE_RESOURCES} --target=${GRESOURCE_C} --generate-source
          --sourcedir=${CMAKE_CURRENT_SOURCE_DIR} ${GRESOURCE_XML}
  DEPENDS ${GRESOURCE_XML} ${GRESOURCE_DEPS}
  COMMENT "Compiling GResource bundle"
  VERBATIM)

add_executable(mb-powermon
  src/main.cpp
  src/Probes.cpp
  src/GraphArea.cpp
  src/MainWindow.cpp
  ${GRESOURCE_C}
)

target_include_directories(mb-powermon PRIVATE src)
target_link_libraries(mb-powermon PRIVATE PkgConfig::GTKMM)
if(MB_HAVE_HAILO)
  target_include_directories(mb-powermon PRIVATE ${HAILORT_INC})
  target_link_libraries(mb-powermon PRIVATE ${HAILORT_LIB})
  target_compile_definitions(mb-powermon PRIVATE MB_HAVE_HAILO=1)
endif()
target_compile_options(mb-powermon PRIVATE
  -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

install(TARGETS mb-powermon RUNTIME DESTINATION bin)
