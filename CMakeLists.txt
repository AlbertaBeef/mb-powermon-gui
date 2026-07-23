cmake_minimum_required(VERSION 3.16)
project(mb-powermon-gui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTKMM REQUIRED IMPORTED_TARGET gtkmm-4.0)

# HailoRT C++ runtime (no pkg-config shipped) — headers under /usr/local/include,
# libhailort in /usr/local/lib. Optional: build without Hailo if not installed.
find_library(HAILORT_LIB hailort PATHS /usr/local/lib /usr/lib)
find_path(HAILORT_INC hailo/hailort.hpp PATHS /usr/local/include /usr/include)
if(NOT HAILORT_LIB OR NOT HAILORT_INC)
  message(FATAL_ERROR "libhailort not found (expected /usr/local/lib/libhailort.so "
                      "and /usr/local/include/hailo/). Install the HailoRT runtime.")
endif()

add_executable(mb-powermon
  src/main.cpp
  src/Probes.cpp
  src/GraphArea.cpp
  src/MainWindow.cpp
)

target_include_directories(mb-powermon PRIVATE src ${HAILORT_INC})
target_link_libraries(mb-powermon PRIVATE PkgConfig::GTKMM ${HAILORT_LIB})
target_compile_options(mb-powermon PRIVATE
  -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

install(TARGETS mb-powermon RUNTIME DESTINATION bin)
