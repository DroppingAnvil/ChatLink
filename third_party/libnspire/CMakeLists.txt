# libnspire (Lights0123) — speaks the TI-Nspire NavNet protocol.
# Upstream uses autotools; this is an equivalent CMake build of src/.
# NOTE: LGPL-3.0 (see COPYING.LESSER). Built as a shared library so ChatLink
# links against it dynamically and stays relinkable, per LGPL §4.

# Transport backend. See src/usb.h for the full explanation.
#   ti     - TI's own tinspusb.sys driver. No driver replacement, no signing,
#            no administrator rights. This is why it is the default.
#   libusb - upstream's backend; needs the device rebound to WinUSB, which
#            takes an admin driver install and disables TI's own software.
set(NSPIRE_USB_BACKEND "ti" CACHE STRING "libnspire USB transport (ti or libusb)")
set_property(CACHE NSPIRE_USB_BACKEND PROPERTY STRINGS ti libusb)

if (NSPIRE_USB_BACKEND STREQUAL "ti")
    set(NSPIRE_TRANSPORT_SOURCE src/usb_ti.c)
elseif (NSPIRE_USB_BACKEND STREQUAL "libusb")
    set(NSPIRE_TRANSPORT_SOURCE src/usb.c)
else ()
    message(FATAL_ERROR "NSPIRE_USB_BACKEND must be 'ti' or 'libusb', got '${NSPIRE_USB_BACKEND}'")
endif ()

add_library(nspire SHARED
        src/init.c
        ${NSPIRE_TRANSPORT_SOURCE}
        src/data.c
        src/error.c
        src/packet.c
        src/service.c
        src/cx2.cpp
        src/services/devinfo.c
        src/services/dir.c
        src/services/file.c
        src/services/os.c
        src/services/screenshot.c
)

target_include_directories(nspire
        # api/nspire.h includes "usb.h", which lives in src/, so consumers
        # need both directories on the include path.
        PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/src/api
        PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ws2_32 supplies htons/htonl/ntohs, which the NavNet packet code uses for the
# protocol's big-endian fields.
target_link_libraries(nspire PUBLIC ws2_32)

if (NSPIRE_USB_BACKEND STREQUAL "ti")
    # setupapi enumerates the device interface tinspusb.sys publishes.
    target_compile_definitions(nspire PUBLIC NSP_BACKEND_TI)
    target_link_libraries(nspire PUBLIC setupapi)
else ()
    target_link_libraries(nspire PUBLIC usb-1.0)
endif ()

set_target_properties(nspire PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
        C_STANDARD 11
        CXX_STANDARD 17
)

if (MINGW)
    target_compile_options(nspire PRIVATE
            -Wno-unused-parameter -Wno-unused-variable -Wno-sign-compare)
endif ()
