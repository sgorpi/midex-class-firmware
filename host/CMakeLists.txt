cmake_minimum_required(VERSION 3.13)
project(midex-fw-upload C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Statically link libusb-1.0 so the resulting binary is self-contained (used by
# the release CI to ship standalone executables). Requires pkg-config to be
# available, as the static link line carries the platform-specific private
# dependencies (e.g. IOKit/CoreFoundation frameworks on macOS, udev on Linux).
option(MIDEX_STATIC_LIBUSB "Link libusb-1.0 statically into the executable" OFF)

# ---- Dependency: libusb-1.0 -------------------------------------------------
# Preferred path: pkg-config (Linux, macOS/Homebrew, MSYS2). Falls back to a
# manual header/library search so the tool still configures on systems without
# a libusb-1.0.pc file.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(LIBUSB IMPORTED_TARGET libusb-1.0)
endif()

if(LIBUSB_FOUND)
  if(MIDEX_STATIC_LIBUSB)
    # Consume pkg-config's static link line (LIBUSB_STATIC_LDFLAGS) so the
    # archive plus its private deps (frameworks, udev, ...) are all linked. With
    # a static-only libusb prefix the bare -lusb-1.0 resolves to the .a.
    add_library(midex_libusb INTERFACE)
    target_include_directories(midex_libusb INTERFACE ${LIBUSB_STATIC_INCLUDE_DIRS})
    target_link_directories(midex_libusb INTERFACE ${LIBUSB_STATIC_LIBRARY_DIRS})
    target_link_libraries(midex_libusb INTERFACE ${LIBUSB_STATIC_LDFLAGS})
    set(LIBUSB_LINK midex_libusb)
  else()
    set(LIBUSB_LINK PkgConfig::LIBUSB)
  endif()
else()
  if(MIDEX_STATIC_LIBUSB)
    message(WARNING
      "MIDEX_STATIC_LIBUSB requires pkg-config to find libusb-1.0; "
      "falling back to a plain library search that may link dynamically.")
  endif()
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
