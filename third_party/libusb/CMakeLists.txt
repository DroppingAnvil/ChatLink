# Minimal CMake build of libusb 1.0.30, Windows/WinUSB backend only.
# Upstream ships autotools/MSVC projects; this builds the same sources with
# whatever compiler CMake has selected so there is no ABI mismatch.

add_library(usb-1.0 STATIC
        libusb/core.c
        libusb/descriptor.c
        libusb/hotplug.c
        libusb/io.c
        libusb/strerror.c
        libusb/sync.c
        libusb/os/events_windows.c
        libusb/os/threads_windows.c
        libusb/os/windows_common.c
        libusb/os/windows_usbdk.c
        libusb/os/windows_winusb.c
)

target_include_directories(usb-1.0
        PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/libusb
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/libusb/os
)

# libusbi.h pulls "config.h" from the include path above.
target_compile_definitions(usb-1.0 PRIVATE HAVE_CONFIG_H)

# windows_hotplug.c is deliberately absent: upstream only builds it when
# LIBUSB_WINDOWS_HOTPLUG is defined (its device-priv fields are guarded by that
# macro), and windows_common.c supplies the non-hotplug fallback. ChatLink polls
# for the calculator rather than subscribing to device-arrival notifications.

# The WinUSB backend resolves SetupAPI/Cfgmgr32/HID entry points at runtime via
# LoadLibrary, so these import libs are only needed for the GUID constants and
# the handful of directly-referenced Win32 calls.
target_link_libraries(usb-1.0 PUBLIC setupapi cfgmgr32 advapi32 ole32 uuid)

if (MINGW)
    target_compile_options(usb-1.0 PRIVATE -Wno-unused-parameter -Wno-unused-variable)
endif ()
