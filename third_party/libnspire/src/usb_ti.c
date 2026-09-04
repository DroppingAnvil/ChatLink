/*
    ChatLink: a libnspire transport that speaks through TI's own Windows driver
    instead of libusb.

    Background
    ----------
    tinspusb.sys is derived from the OSR USB WDM sample. It registers a device
    interface (GUID below) and its dispatch routines map plain ReadFile and
    WriteFile onto the device's bulk IN and OUT endpoints - the driver's own
    debug strings say as much ("We received IN end point", and a CreateClose
    handler that rejects any non-empty filename, i.e. you open the interface
    itself and read/write it directly).

    That is precisely the surface libnspire needs, so the calculator can be
    driven with no driver replacement, no signing work and no administrator
    rights. Verified by reading a NavNet packet (leading 0x54FD) off a CX.

    Licensing: this file is part of the libnspire build and is covered by the
    same GPL/LGPL terms as the rest of src/.
*/

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <setupapi.h>

#include <stdio.h>
#include <string.h>

#include "usb.h"
#include "error.h"

/* Device interface published by tinspusb.sys. */
static const GUID kTiNspireInterface = {
	0xc5b7f228, 0xcaff, 0x42d5,
	{ 0xa4, 0x72, 0x6b, 0x9e, 0xda, 0x79, 0x82, 0xec }
};

int usb_init(void) {
	/* Nothing to set up: the driver is already loaded and bound. */
	return NSPIRE_ERR_SUCCESS;
}

void usb_finish(void) {
}

/* Builds "vid_xxxx&pid_xxxx" and looks for it in an interface path. Paths come
   back lower-cased from SetupAPI, but compare case-insensitively regardless. */
static int path_matches(const char *path, uint16_t vid, uint16_t pid) {
	char needle[32];
	snprintf(needle, sizeof(needle), "vid_%04x&pid_%04x", vid, pid);

	for (const char *p = path; *p; ++p) {
		size_t i = 0;
		while (needle[i] && p[i]
				&& tolower((unsigned char)p[i]) == needle[i]) {
			++i;
		}
		if (!needle[i])
			return 1;
	}
	return 0;
}

int usb_get_device(usb_device_t *handle, uint16_t vid, uint16_t pid) {
	HDEVINFO info;
	SP_DEVICE_INTERFACE_DATA iface;
	DWORD index = 0;
	int result = -NSPIRE_ERR_NODEVICE;

	handle->dev = NULL;

	info = SetupDiGetClassDevsA(&kTiNspireInterface, NULL, NULL,
			DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (info == INVALID_HANDLE_VALUE)
		return -NSPIRE_ERR_NODEVICE;

	memset(&iface, 0, sizeof(iface));
	iface.cbSize = sizeof(iface);

	while (SetupDiEnumDeviceInterfaces(info, NULL, &kTiNspireInterface,
			index++, &iface)) {
		DWORD needed = 0;
		SP_DEVICE_INTERFACE_DETAIL_DATA_A *detail;
		HANDLE device;

		/* First call only reports the required size. */
		SetupDiGetDeviceInterfaceDetailA(info, &iface, NULL, 0, &needed, NULL);
		if (!needed)
			continue;

		detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)calloc(1, needed);
		if (!detail) {
			result = -NSPIRE_ERR_NOMEM;
			break;
		}
		/* cbSize is the size of the fixed part of the struct, never the size
		   of the allocation - a classic SetupAPI trap. */
		detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

		if (!SetupDiGetDeviceInterfaceDetailA(info, &iface, detail, needed,
				&needed, NULL)) {
			free(detail);
			continue;
		}

		if (!path_matches(detail->DevicePath, vid, pid)) {
			free(detail);
			continue;
		}

		device = CreateFileA(detail->DevicePath,
				GENERIC_READ | GENERIC_WRITE,
				FILE_SHARE_READ | FILE_SHARE_WRITE,
				NULL, OPEN_EXISTING,
				FILE_FLAG_OVERLAPPED, NULL);
		free(detail);

		if (device == INVALID_HANDLE_VALUE) {
			/* The driver permits a single opener; TI's own software holding
			   the device is the usual reason this fails. */
			result = (GetLastError() == ERROR_ACCESS_DENIED
					|| GetLastError() == ERROR_SHARING_VIOLATION)
					? -NSPIRE_ERR_BUSY : -NSPIRE_ERR_NODEVICE;
			continue;
		}

		handle->dev = device;

		/* Drain anything the previous session left queued on the IN endpoint.
		   libusb's backend called libusb_reset_device(), which cleared endpoint
		   state on every open; TI's driver performs no reset, so a packet left
		   unread by an earlier run would desync the very first exchange of this
		   one. Short timeout, discard everything, stop at the first silence. */
		{
			unsigned char scratch[512];
			int drained = 0;
			while (usb_bulk(handle, NSP_EP_IN, scratch, sizeof(scratch),
					&drained, 400) >= 0 && drained > 0) {
				/* keep reading until the endpoint goes quiet */
			}
		}

		result = NSPIRE_ERR_SUCCESS;
		break;
	}

	SetupDiDestroyDeviceInfoList(info);
	return result;
}

void usb_free_device(usb_device_t *handle) {
	if (handle->dev && handle->dev != INVALID_HANDLE_VALUE) {
		CancelIo((HANDLE)handle->dev);
		CloseHandle((HANDLE)handle->dev);
	}
	handle->dev = NULL;
}

int usb_bulk(usb_device_t *handle, int direction, void *ptr, int len,
		int *transferred, int timeout_ms) {
	HANDLE device = (HANDLE)handle->dev;
	OVERLAPPED overlapped;
	DWORD moved = 0;
	BOOL ok;
	int result;

	if (transferred)
		*transferred = 0;
	if (!device || device == INVALID_HANDLE_VALUE)
		return -NSPIRE_ERR_NODEVICE;

	memset(&overlapped, 0, sizeof(overlapped));
	/* Manual-reset, initially unsignalled: required for overlapped I/O. */
	overlapped.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
	if (!overlapped.hEvent)
		return -NSPIRE_ERR_NOMEM;

	ok = (direction == NSP_EP_IN)
			? ReadFile(device, ptr, (DWORD)len, &moved, &overlapped)
			: WriteFile(device, ptr, (DWORD)len, &moved, &overlapped);

	if (!ok) {
		if (GetLastError() != ERROR_IO_PENDING) {
			CloseHandle(overlapped.hEvent);
			return -NSPIRE_ERR_LIBUSB;
		}

		switch (WaitForSingleObject(overlapped.hEvent, (DWORD)timeout_ms)) {
		case WAIT_OBJECT_0:
			if (!GetOverlappedResult(device, &overlapped, &moved, FALSE)) {
				CloseHandle(overlapped.hEvent);
				return -NSPIRE_ERR_LIBUSB;
			}
			break;

		case WAIT_TIMEOUT:
			/* Cancel and drain, or the driver keeps writing into a buffer the
			   caller is about to reuse. */
			CancelIo(device);
			GetOverlappedResult(device, &overlapped, &moved, TRUE);
			CloseHandle(overlapped.hEvent);
			return -NSPIRE_ERR_TIMEOUT;

		default:
			CloseHandle(overlapped.hEvent);
			return -NSPIRE_ERR_LIBUSB;
		}
	}

	CloseHandle(overlapped.hEvent);

	if (transferred)
		*transferred = (int)moved;

	/* Match the libusb backend: 0 when everything moved, otherwise the
	   shortfall. */
	result = len - (int)moved;
	return result;
}

int usb_write(usb_device_t *handle, void *ptr, int len) {
	int transferred = 0;
	return usb_bulk(handle, NSP_EP_OUT, ptr, len, &transferred, NSP_TIMEOUT);
}

int usb_read(usb_device_t *handle, void *ptr, int len) {
	int transferred = 0;
	return usb_bulk(handle, NSP_EP_IN, ptr, len, &transferred, NSP_TIMEOUT);
}
