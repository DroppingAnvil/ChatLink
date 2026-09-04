cmake_minimum_required(VERSION 3.29)
project(ChatLink CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if (NOT WIN32)
    message(FATAL_ERROR "ChatLink targets Windows: the OS-input layer is Win32 SendInput.")
endif ()

# CLion's bundled MinGW is not on the system PATH, so a dynamically linked
# binary would only run from inside the IDE. Folding the runtime in makes
# ChatLink.exe standalone. This covers libgcc, libstdc++ and libwinpthread.
if (MINGW)
    add_link_options(-static-libgcc -static-libstdc++
            -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive)
endif ()

# libusb is only needed by libnspire's legacy WinUSB transport. The default
# 'ti' backend talks to TI's own driver and needs neither.
set(NSPIRE_USB_BACKEND "ti" CACHE STRING "libnspire USB transport (ti or libusb)")
if (NSPIRE_USB_BACKEND STREQUAL "libusb")
    add_subdirectory(third_party/libusb)
endif ()
add_subdirectory(third_party/libnspire)
add_subdirectory(src)
