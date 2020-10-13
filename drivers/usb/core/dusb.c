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

#include <linux/module.h>
#include <linux/usb.h>
#include <linux/dusb.h>
#include <linux/hid.h>
#ifdef CONFIG_PROC_FS
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#endif
#include <linux/usb/hcd.h>	/* for usbcore internals */

#include "usb.h"

#if 1
#define dprintk(args...)
#define dprintk2(args...)
#else
#define dprintk printk
#define dprintk2 printk
#endif

#define lock_kernel()
#define unlock_kernel()

static int __match_minor(struct device *dev, const void *data)
{
	const int minor = *((const int *)data);

	if (dev->devt == MKDEV(USB_DEVICE_MAJOR, minor))
		return 1;
	return 0;
}

static struct usb_device *usbdev_lookup_by_minor(int minor)
{
	struct device *dev;

	dev = bus_find_device(&usb_bus_type, NULL, &minor, __match_minor);
	if (!dev)
		return NULL;

	return container_of(dev, struct usb_device, dev);
}

struct usb_device *dusb_open(unsigned bus, unsigned device)
{
	int minor = ((bus - 1) * 128) + (device - 1);
	struct usb_device *dev = NULL;

	dev = usbdev_lookup_by_minor(minor);
	if (NULL == dev)
		goto out;

	usb_lock_device(dev);

	usb_get_dev(dev);
	put_device(&dev->dev);
	usb_unlock_device(dev);

out:

	return dev;
}
EXPORT_SYMBOL(dusb_open);

void dusb_close(struct usb_device *dev)
{
	usb_lock_device(dev);

	/*
	 * Resetting the device will make sure it gets reprobed and
	 * another device driver can claim it.
	 */
	usb_reset_device(dev);

	usb_put_dev(dev);
	usb_unlock_device(dev);
}
EXPORT_SYMBOL(dusb_close);

int dusb_set_configuration(struct usb_device *dev, int configuration)
{
	return usb_set_configuration(dev, configuration);
}
EXPORT_SYMBOL(dusb_set_configuration);

void dusb_flush_endpoint(struct usb_device *udev, struct usb_host_endpoint *ep)
{
	usb_hcd_flush_endpoint(udev, ep);
}
EXPORT_SYMBOL(dusb_flush_endpoint);

int dusb_reenumerate(unsigned bus, unsigned device)
{
	int minor = ((bus - 1) * 128) + (device - 1);
	struct usb_device *udev = NULL;

	udev = usbdev_lookup_by_minor(minor);
	if (udev) {
		printk("Forcing re-enumeration of %s - %s\n",
			udev->product, udev->manufacturer);
		usb_device_reenumerate(udev);
		put_device(&udev->dev);
	}

	return (udev != NULL);
}
EXPORT_SYMBOL(dusb_reenumerate);

int dusb_dev_running(struct usb_device *udev)
{
	struct usb_hcd *hcd = bus_to_hcd(udev->bus);

	return (hcd ? HCD_RH_RUNNING(hcd) : 0);
}
EXPORT_SYMBOL(dusb_dev_running);

static int dusb_hcd_speed_super(struct usb_hcd *hcd)
{
	return (hcd->driver->flags & HCD_USB3);
}

static int dusb_hcd_speed_high(struct usb_hcd *hcd)
{
	return (hcd->driver->flags & HCD_USB2);
}

static int dusb_hcd_speed(struct usb_hcd *hcd)
{
	return (dusb_hcd_speed_super(hcd) ? USB_SPEED_SUPER :
		(dusb_hcd_speed_high(hcd) ? USB_SPEED_HIGH : USB_SPEED_LOW));
}

int dusb_dev_controller_speed(struct usb_device *udev)
{
	struct usb_hcd *hcd = bus_to_hcd(udev->bus);

	return (hcd ? dusb_hcd_speed(hcd) : 0);
}
EXPORT_SYMBOL(dusb_dev_controller_speed);

