/*
    This file is part of libnspire.

    libnspire is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    libnspire is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with libnspire.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
    ChatLink patch: the transport is now selectable at build time.

    NSP_BACKEND_TI  - talk to the calculator through TI's own driver
                      (tinspusb.sys), which publishes a device interface that
                      accepts CreateFile plus overlapped ReadFile/WriteFile
                      straight onto the bulk endpoints. Needs no libusb, no
                      driver replacement and no administrator rights.

    default         - the original libusb backend, which requires the device to
                      be rebound to WinUSB.

    Both expose the same six calls plus usb_bulk(), so nothing above this layer
    knows which one is in use.
*/

#ifndef _USB_H
#define _USB_H

#include <stdint.h>

#define NSP_VID 0x0451
#define NSP_PID 0xe012
#define NSP_PID_CX2 0xe022

/* Backend-independent endpoint selector for usb_bulk(). */
#define NSP_EP_OUT 0
#define NSP_EP_IN  1

/* Default transfer timeout, milliseconds. */
#define NSP_TIMEOUT 10000

#ifdef NSP_BACKEND_TI

typedef struct {
	/* Win32 HANDLE onto TI's device interface, opened FILE_FLAG_OVERLAPPED.
	   Declared void* so this header stays free of <windows.h>. */
	void *dev;
} usb_device_t;

#else

#include <libusb.h>

typedef struct {
	libusb_device_handle *dev;
	unsigned char ep_in, ep_out;
} usb_device_t;

#endif

/* cx2.cpp is C++ and includes this header, so the declarations need C
   linkage or the transport symbols come out mangled and fail to link. */
#ifdef __cplusplus
extern "C" {
#endif

int usb_init(void);
void usb_finish(void);
int usb_get_device(usb_device_t *handle, uint16_t vid, uint16_t pid);
void usb_free_device(usb_device_t *handle);
int usb_write(usb_device_t *handle, void *ptr, int len);
int usb_read(usb_device_t *handle, void *ptr, int len);

/* Bulk transfer with an explicit timeout. Returns 0 on a complete transfer,
   a positive shortfall when fewer bytes moved than asked, or -NSPIRE_ERR_*. */
int usb_bulk(usb_device_t *handle, int direction, void *ptr, int len,
		int *transferred, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
