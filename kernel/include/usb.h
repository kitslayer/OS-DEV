/* usb.h — UHCI USB controller + an absolute-pointing tablet. */
#pragma once

/* Find the UHCI controller, enumerate the usb-tablet, and start polling.
 * Returns 0 on success, -1 if no controller/tablet was usable. */
int  usb_tablet_init(void);

/* Check for a new HID report and update the pointer position (call often). */
void usb_tablet_poll(void);
