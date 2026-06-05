cmake_minimum_required(VERSION 3.13)
project(midex-fw-upload C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- Dependency: libusb-1.0 -------------------------------------------------
# Preferred path: pkg-config (Linux, macOS/Homebrew, MSYS2). Falls back to a
# manual header/library search so the tool still configures on systems without
# a libusb-1.0.pc file.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(LIBUSB IMPORTED_TARGET libusb-1.0)
endif()

if(LIBUSB_FOUND)
  set(LIBUSB_LINK PkgConfig::LIBUSB)
else()
  message(STATUS "libusb-1.0.pc not found via pkg-config; searching manually")
  find_path(LIBUSB_INCLUDE_DIR libusb.h PATH_SUFFIXES libusb-1.0)
  find_library(LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0)
  if(NOT LIBUSB_INCLUDE_DIR OR NOT LIBUSB_LIBRARY)
    message(FATAL_ERROR
      "libusb-1.0 development files not found.\n"
      "  Debian/Ubuntu : sudo apt install libusb-1.0-0-dev\n"
      "  Fedora        : sudo dnf install libusbx-devel\n"
      "  macOS (brew)  : brew install libusb\n"
      "  Windows/MSYS2 : pacman -S mingw-w64-x86_64-libusb")
  endif()
  add_library(midex_libusb INTERFACE)
  target_include_directories(midex_libusb INTERFACE ${LIBUSB_INCLUDE_DIR})
  target_link_libraries(midex_libusb INTERFACE ${LIBUSB_LIBRARY})
  set(LIBUSB_LINK midex_libusb)
endif()

# ---- Target -----------------------------------------------------------------
add_executable(midex-fw-upload midex-fw-upload.c)
target_link_libraries(midex-fw-upload PRIVATE ${LIBUSB_LINK})

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(midex-fw-upload PRIVATE -Wall -Wextra)
endif()

install(TARGETS midex-fw-upload RUNTIME DESTINATION bin)
