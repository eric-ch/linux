/*****************************************************************************/

/*
 *      dusb.c  --  Direct communication with USB devices.
 *
 *      Copyright (C) 1999-2000  Thomas Sailer (sailer@ife.ee.ethz.ch)
 *      Copyright (C) 2008-2012  Virtual Computer Inc
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License
 *      along with this program; if not, write to the Free Software
 *      Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *
 *  Derived from usb/core/devio.c
 *
 */

/*****************************************************************************/

#ifndef __LINUX_DUSB_H
#define __LINUX_DUSB_H

extern struct usb_device *dusb_open(unsigned bus, unsigned device);
extern void dusb_close(struct usb_device *dev);
extern int dusb_set_configuration(struct usb_device *dev, int configuration);
extern void dusb_flush_endpoint(struct usb_device *udev, struct usb_host_endpoint *ep);

extern int dusb_dev_running(struct usb_device *udev);
extern int dusb_dev_controller_speed(struct usb_device *udev);

/* hub.c */
extern void usb_device_reenumerate(struct usb_device *udev);

#endif
