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

#include <stdlib.h>
#include <string.h>

#include "packet.h"
#include "error.h"
#include "usb.h"

int nspire_init(nspire_handle_t **ptr) {
	int ret;
	struct packet p;
	nspire_handle_t *h = malloc(sizeof(*h));

	if (!h)
		return -NSPIRE_ERR_NOMEM;

	if ( (ret = usb_init()) )
		goto error;

	h->is_cx2 = false;
	if ( (ret = usb_get_device(&h->device, NSP_VID, NSP_PID)) ) {
		h->is_cx2 = true;
		if ( (ret = usb_get_device(&h->device, NSP_VID, NSP_PID_CX2)) )
			goto error;
	}

	h->host_addr = 0x6400;
	h->device_addr = 0x6401;
	h->host_sid = 0x4003;
	h->device_sid = 0x4003;
	h->connected = 0;
	h->seq = 1;
	h->cx2_handshake_complete = false;

	if (!h->is_cx2) {
		/* Wait for an address request.
		   ChatLink patch: this recv discards the packet - the addresses above
		   are hardcoded - so its only job is to drain the single unsolicited
		   address request the calculator emits when the USB connection is
		   established. The libusb backend called libusb_reset_device(), which
		   forced a re-enumeration and hence a fresh request on every open.
		   Through TI's driver there is no reset, so from the second connection
		   onward the packet has already been consumed and none is coming.
		   Treat a timeout as 'already drained' instead of failing. */
		ret = packet_recv(h, NULL);
		if (ret && ret != -NSPIRE_ERR_TIMEOUT)
			goto error_free_usb;
	}

	p = packet_new(h);
	packet_set_data(p, 0x64, 0x01, 0xFF, 0x00);
	if ( (ret = packet_send(h, p)) )
		goto error_free_usb;

	h->host_sid = 0x8000;

	*ptr = h;

	return NSPIRE_ERR_SUCCESS;

error_free_usb:
	usb_free_device(&h->device);
error:
	free(h);
	return ret;
}

void nspire_free(nspire_handle_t *ptr) {
	usb_free_device(&ptr->device);
	free(ptr);
}
