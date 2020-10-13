/******************************************************************************
 * usbback/vusb.c
 *
 * Routines for managing virtual usb devices.
 *
 * Copyright (c) Citrix Systems Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation; or, when distributed
 * separately from the Linux kernel or incorporated into other
 * software packages, subject to the following license:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this source file (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <linux/usb.h>
#include <linux/dusb.h>

#include "common.h"

struct vusb_map
{
	int         bus;
	int         device;
	struct vusb *vusb;
};

#define VUSB_MAX_DEVICES 512
static struct vusb_map vusb_map[VUSB_MAX_DEVICES];
static spinlock_t vusb_map_lock;


/* Add or update bus,dev to map to new vusb
   There can be only one of each {bus,device} pair */
static int
vusb_map_device(struct vusb *vusb, int bus, int device)
{
	unsigned long flags;
	int index;
	int ret = -1;

	spin_lock_irqsave(&vusb_map_lock, flags);
	for (index=0; index<VUSB_MAX_DEVICES; index++) {
		struct vusb_map *map = &vusb_map[index];

		if ((map->vusb == NULL) || ((map->bus == bus) && (map->device == device)) ){
			if (map->vusb)
				debug_print(LOG_LVL_ERROR, "%s: removing dup\n",__FUNCTION__);
			map->vusb   = vusb;
			map->bus    = bus;
			map->device = device;
			ret = 0;
			break;
		}
	}
    index++;
	/* flush any remaining dulpicate pairs */
	while (index<VUSB_MAX_DEVICES) {
		struct vusb_map *map = &vusb_map[index];
		if ((map->bus == bus) && (map->device == device)) {
			debug_print(LOG_LVL_ERROR, "%s: removing dup\n",__FUNCTION__);
			map->vusb = NULL;
			map->bus    = 0;
			map->device = 0;
		}
		index++;
	}

	spin_unlock_irqrestore(&vusb_map_lock, flags);

	return ret;
}

static int
vusb_unmap_device(struct vusb *vusb)
{
	unsigned long flags;
	int index;
	int ret = -1;

	spin_lock_irqsave(&vusb_map_lock, flags);
	for (index=0; index<VUSB_MAX_DEVICES; index++) {
		struct vusb_map *map = &vusb_map[index];

		if (map->vusb == vusb) {
			map->vusb   = NULL;
			map->bus    = 0;
			map->device = 0;
			ret = 0;
			break;
		}
	}
	spin_unlock_irqrestore(&vusb_map_lock, flags);

	return ret;
}

static struct vusb *
vusb_find_device(int bus, int device)
{
	unsigned long flags;
	int index;
	struct vusb *vusb = NULL;

	spin_lock_irqsave(&vusb_map_lock, flags);
	for (index=0; index<VUSB_MAX_DEVICES; index++) {
		struct vusb_map *map = &vusb_map[index];

		if (map->vusb && (map->bus == bus) && (map->device == device)) {
			vusb = map->vusb;
			break;
		}
	}
	spin_unlock_irqrestore(&vusb_map_lock, flags);

	return vusb;
}

static void vusb_delete(struct kref *kref)
{
	struct vusb *vusb = KREF_TO_VUSB(kref);

	debug_print(LOG_LVL_ERROR, "%s: vusb %p\n", __FUNCTION__, vusb);
	vusb->active = 0;

	vusb_flush(vusb);
}

static int vusb_probe(struct usb_interface *intf,
			 const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	int bus = udev->bus->busnum;
	int port = udev->portnum;
	int device = udev->devnum;
	struct vusb *vusb = vusb_find_device(bus, device);

	debug_print(LOG_LVL_ERROR, "%s: intf %p vusb %p for %d:%d (port %d)\n",
		__FUNCTION__, intf, vusb, bus, device, port);

	if (vusb) {
		if (!vusb->active) {
			/*
			 * The driver released all of its interfacesi, is now
			 * reprobing. reference counting needs to be restarted
			 * and the device marked active.
			 */
			kref_init(&vusb->kref);
			vusb->active = 1;
		} else {
			kref_get(&vusb->kref);
		}
		usb_set_intfdata(intf, vusb);
		return 0;
	}

	return -ENODEV;
}

static void vusb_disconnect(struct usb_interface *intf)
{
	struct vusb *vusb = usb_get_intfdata(intf);

	debug_print(LOG_LVL_ERROR, "%s: intf %p vusb %p\n",
		__FUNCTION__, intf, vusb);

	if (!vusb)
		return;

	/* Mark the interface for later rebinding */
	intf->needs_binding = 1;

	usb_set_intfdata(intf, NULL);
	kref_put(&vusb->kref, vusb_delete);
}

static int vusb_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct vusb *vusb = usb_get_intfdata(intf);

	debug_print(LOG_LVL_ERROR, "%s: intf %p vusb %p\n",
		__FUNCTION__, intf, vusb);

	if (!vusb || !vusb->initted)
		return -1;

	usbback_suspend(usbif_from_vusb(vusb), 1);
	vusb_flush(vusb);
	return 0;
}

static int vusb_resume(struct usb_interface *intf)
{
	struct vusb *vusb = usb_get_intfdata(intf);

	debug_print(LOG_LVL_ERROR, "%s: intf %p vusb %p\n",
		__FUNCTION__, intf, vusb);

	if (!vusb || !vusb->initted)
		return -1;

	usbback_suspend(usbif_from_vusb(vusb), 0);
	return 0;
}

static int vusb_reset_resume(struct usb_interface *intf)
{
	struct vusb *vusb = usb_get_intfdata(intf);

	debug_print(LOG_LVL_ERROR, "%s: intf %p vusb %p\n",
		__FUNCTION__, intf, vusb);

	return vusb_resume(intf);
}

static int vusb_pre_reset(struct usb_interface *intf)
{
	struct vusb *vusb = usb_get_intfdata(intf);

	debug_print(LOG_LVL_ERROR, "%s: intf %p vusb %p\n",
		__FUNCTION__, intf, vusb);

	if (!vusb)
		return -ENODEV;

	vusb->canceling_requests = 1;
	return 0;
}

static int vusb_post_reset(struct usb_interface *intf)
{
	struct vusb *vusb = usb_get_intfdata(intf);

	debug_print(LOG_LVL_ERROR, "%s: intf %p vusb %p\n",
		__FUNCTION__, intf, vusb);

	if (!vusb)
		return -ENODEV;

	vusb->canceling_requests = 0;
	return 0;
}

struct usb_driver vusb_driver = {
	.name                 = "vusb",
	.probe                = vusb_probe,
	.disconnect           = vusb_disconnect,
	.suspend              = vusb_suspend,
	.resume               = vusb_resume,
	.reset_resume         = vusb_reset_resume,
	.pre_reset            = vusb_pre_reset,
	.post_reset           = vusb_post_reset,
	.supports_autosuspend = 1,
	.soft_unbind          = 0,
};

static int
vusb_claim_interface(struct vusb *vusb, struct usb_interface *intf)
{
	struct device *dev = &intf->dev;
	int ret;

	debug_print(LOG_LVL_DEBUG,
		"%s: claim interface if %p, vusb %p\n", __FUNCTION__, intf, vusb);

	if (dev->driver) {
		struct usb_driver *driver = to_usb_driver(dev->driver);

		/* Even if this driver already owns it, its probably with
		 * the wrong vusb, so we still need to release it, and
		 * claim it properly
		 */

		if (driver == &vusb_driver) {
			struct vusb *old_vusb = usb_get_intfdata(intf);
			debug_print(LOG_LVL_ERROR,
				"%s: release ourselves with vusb %p "
				"from interface if %p\n", __FUNCTION__,
				old_vusb,intf);
		} else {
			debug_print(LOG_LVL_ERROR,
				"%s: release old driver from interface if %p\n",
				__FUNCTION__, intf);
		}
		usb_driver_release_interface(driver, intf);
	}

	ret = usb_driver_claim_interface(&vusb_driver, intf, vusb);
	if (ret)
		debug_print(LOG_LVL_ERROR,
			"%s: claim_interface failed for if %p ret %d\n",
			__FUNCTION__, intf, ret);
	else
		usb_set_intfdata(intf, vusb);

	return (ret);
}

static void
vusb_claim_config(struct vusb *vusb, struct usb_host_config *config)
{
	unsigned int ifs = config->desc.bNumInterfaces;
	unsigned int ifnum;

	for (ifnum = 0; ifnum < ifs; ifnum++) {
		struct usb_interface *intf = config->interface[ifnum];

		/*
		 * If there is an interface and we end up with ownership,
		 * count it.
		 */
		if (intf && (vusb_claim_interface(vusb, intf) == 0))
			kref_get(&vusb->kref);
	}
}

/* precondition: usb_lock_device should be called */

static void
vusb_claim_dev(struct vusb *vusb, struct usb_device *udev)
{
	unsigned int confs = udev->descriptor.bNumConfigurations;
	unsigned int confnum;

	debug_print(LOG_LVL_ERROR,
		"%s: claim device %p (%d.%d (port %d)), vusb %p\n", __FUNCTION__,
		udev, udev->bus->busnum, udev->devnum, udev->portnum, vusb);

	for (confnum = 0; confnum < confs; confnum++) {
		struct usb_host_config *config = &udev->config[confnum];

		if (config)
			vusb_claim_config(vusb, config);
	}
	return;
}

static void
vusb_release_config(struct vusb *vusb, struct usb_host_config *config)
{
	unsigned int ifnum;

	debug_print(LOG_LVL_DEBUG, "%s[%d]: vusb %p config %p\n",
		__FUNCTION__, __LINE__, vusb, config);

	for (ifnum = 0; ifnum < config->desc.bNumInterfaces; ifnum++) {
		struct usb_interface *intf = config->interface[ifnum];
		struct device *dev = &intf->dev;
		struct usb_driver *driver = to_usb_driver(dev->driver);

		/*
		 * Only release the interface if we own it. Releasing it will
		 * result in our disconnect handler being called.
		 */
		if (driver == &vusb_driver) {
			struct vusb *old_vusb = usb_get_intfdata(intf);
			if (old_vusb == vusb) {
				debug_print(LOG_LVL_ERROR, "%s[%d]: vusb %p intf %p\n",
				    __FUNCTION__, __LINE__, vusb, intf);
				usb_driver_release_interface(&vusb_driver, intf);
			} else {
				debug_print(LOG_LVL_ERROR, "%s[%d]: not releasing vusb %p config %p\n",
				__FUNCTION__, __LINE__, old_vusb, config);
			}
		}
	}
}

static void vusb_release_dev(struct vusb *vusb, struct usb_device *udev)
{
	debug_print(LOG_LVL_DEBUG, "%s[%d]: vusb %p dev %p (%d.%d (port %d))\n",
		__FUNCTION__, __LINE__, vusb, udev,
		udev->bus->busnum, udev->devnum, udev->portnum);

	if (udev->actconfig)
		vusb_release_config(vusb, udev->actconfig);

	return;
}

int vusb_init(void)
{
	spin_lock_init(&vusb_map_lock);

	return usb_register(&vusb_driver);
}

void vusb_cleanup(void)
{
	usb_deregister(&vusb_driver);
}

int vusb_create(usbif_t *usbif, usbif_vdev_t handle, unsigned bus,
	       unsigned device)
{
	struct vusb *vusb;
	struct usb_device *usbdev;

	vusb = &usbif->vusb;
	vusb->handle             = handle;
	vusb->bus                = bus;
	vusb->device             = device;
	vusb->active             = 1;

	kref_init(&vusb->kref);

	init_usb_anchor(&vusb->anchor);

	usbdev = dusb_open(bus, device);
	if (NULL == usbdev) {
		printk("VUSB: failed to open %d.%d\n", bus, device);
		return -1;
	}

	usb_lock_device(usbdev);
	vusb_map_device(vusb, bus, device);

	/* validate */
	if ((device != usbdev->devnum) || (bus != usbdev->bus->busnum))
		debug_print(LOG_LVL_ERROR, "Device mismatch %d.%d vs %d.%d\n",
			bus, device, usbdev->devnum, usbdev->bus->busnum);

	vusb_claim_dev(vusb, usbdev);
	vusb->usbdev = usbdev;
	vusb->max_sgs = usbdev->bus->sg_tablesize;
	vusb->hcd_speed = dusb_dev_controller_speed(usbdev);
	/* EHCI fails unaligned transfers with BABBLE (EOVERFLOW) */
	vusb->copy_unaligned = (vusb->hcd_speed != USB_SPEED_SUPER);

	/* don't allow the device to suspend until the frontend says so */
	usb_disable_autosuspend(usbdev);

	vusb->initted = 1;

	usb_unlock_device(usbdev);
	kref_put(&vusb->kref, vusb_delete);

	debug_print(LOG_LVL_ERROR,
		"Created vusb %p (%d) device %d.%d (dom=%u) max sgs %u\n",
		vusb, vusb->kref.refcount.refs.counter, bus, device, usbif->domid,
		vusb->max_sgs);
	debug_print(LOG_LVL_ERROR,
		"VUSB: device %s - %s - %s speed %s on %s\n",
		usbdev->product, usbdev->manufacturer, usbdev->serial,
		(usbdev->speed == USB_SPEED_SUPER) ? "super" :
		(usbdev->speed == USB_SPEED_HIGH) ? "high" : "low",
		(vusb->hcd_speed == USB_SPEED_SUPER) ? "super" :
		(vusb->hcd_speed == USB_SPEED_HIGH) ? "high" : "low");
	return 0;
}

void vusb_free(struct vusb *vusb)
{
	struct usb_device *usbdev = vusb->usbdev;

	if (usbdev) {
		usb_lock_device(usbdev);

		debug_print(LOG_LVL_ERROR, "VUSB: close device %s %s %s\n",
			usbdev->product, usbdev->manufacturer, usbdev->serial);

		vusb->usbdev = NULL;
		vusb_unmap_device(vusb);

		/* flush any remaining requests */
		vusb_flush(vusb);

		/*
		 * If we haven't received cleanup callbacks from the USB side
		 * yet, do the USB cleanup.
		 */
		if (vusb->active)
			vusb_release_dev(vusb, usbdev);

		usb_unlock_device(usbdev);
		dusb_close(usbdev);
	}
}

static char * setup_type(int type)
{
	switch((type & USB_TYPE_MASK)) {
		case USB_TYPE_STANDARD:
			return "standard";
		case USB_TYPE_CLASS:
			return "class";
		case USB_TYPE_VENDOR:
			return "reserved";
		case USB_TYPE_RESERVED:
		default:
			return "reserved";
	}
}

static char * setup_recip(int type)
{
	switch((type & USB_RECIP_MASK)) {
		case USB_RECIP_DEVICE:
			return "device";
		case USB_RECIP_INTERFACE:
			return "interface";
		case USB_RECIP_ENDPOINT:
			return "endpoint";
		case USB_RECIP_OTHER:
			return "other";
		case USB_RECIP_PORT:
			return "port";
		case USB_RECIP_RPIPE:
			return "rpipe";
		default:
			return "recip unknown";
	}
}

static int maybe_set_configuration(struct usb_device *dev, int configuration)
{
	struct usb_host_config *cp = NULL;
        int i;

        for (i = 0; i < dev->descriptor.bNumConfigurations; i++) {
                if (dev->config[i].desc.bConfigurationValue ==
                    configuration) {
                        cp = &dev->config[i];
                        break;
                }
        }
        if (cp && cp == dev->actconfig)
                return 0;
        return dusb_set_configuration(dev, configuration);
}

static int setup_control_urb(struct vusb *vusb, usbif_request_t *req,
				struct urb *urb)
{
	struct usb_device *usbdev = vusb->usbdev;
	usbif_stats_t *stats = &(usbif_from_vusb(vusb)->stats);
	struct usb_ctrlrequest *setup =
		(struct usb_ctrlrequest *)urb->setup_packet;
	int value, index, length;
	int ret = 0;

	memcpy(urb->setup_packet, &req->setup, sizeof(struct usb_ctrlrequest));

	value = __le16_to_cpup(&setup->wValue);
	index = __le16_to_cpup(&setup->wIndex);
	length = __le16_to_cpup(&setup->wLength);

	debug_print(LOG_LVL_DEBUG,
		"%s: setup: %s %s %s req %x val %x idx %x len %x\n",
		__FUNCTION__,
		(setup->bRequestType & USB_DIR_IN) ? "IN" : "OUT",
		setup_type(setup->bRequestType),
		setup_recip(setup->bRequestType),
		(int)setup->bRequest,
		value, index, length);

	switch (setup->bRequestType & USB_RECIP_MASK) {
	case USB_RECIP_DEVICE:
		if (setup->bRequest == USB_REQ_CLEAR_FEATURE) {
			debug_print(LOG_LVL_DEBUG, "clear feature\n");
		} else if (setup->bRequest == USB_REQ_SET_CONFIGURATION) {
			int confnum = value;

			debug_print(LOG_LVL_DEBUG, "set config %d\n", confnum);

			usb_lock_device(usbdev);
			ret = maybe_set_configuration(usbdev, confnum);
			usb_unlock_device(usbdev);
			if (ret == 0)
				return (1);
		} else if (setup->bRequest == USB_REQ_GET_DESCRIPTOR) {
			int type = value >> 8;
			int id = value & 0xff;

			if ((type == USB_DT_STRING) && (id > 0)) {
				debug_print(LOG_LVL_DEBUG,
					"get string descriptor index %d language %x\n",
					id, index);
			} else {
				debug_print(LOG_LVL_DEBUG,
					"get descriptor type %d index %d\n",
					type, id);
			}
		}
		break;

	case USB_RECIP_INTERFACE:
		if (setup->bRequest == USB_REQ_CLEAR_FEATURE) {
			debug_print(LOG_LVL_DEBUG, "clear feature\n");
		} else if (setup->bRequest == USB_REQ_SET_INTERFACE) {
			int ifnum = index;
			int alt = value;

			debug_print(LOG_LVL_DEBUG, "set interface %d\n", ifnum);

			ret = usb_set_interface(usbdev, ifnum, alt);
			if (ret == 0)
				return (1);
		}
		break;

	case USB_RECIP_ENDPOINT:
		if (setup->bRequest == USB_REQ_GET_STATUS) {
			debug_print(LOG_LVL_DEBUG, "get status %d\n", index);
		} else if ((setup->bRequest == USB_REQ_CLEAR_FEATURE) &&
				(value == USB_ENDPOINT_HALT)) {
			int ep = index;
			int epnum = ep & 0x7f;
			int pipe;

			debug_print(LOG_LVL_DEBUG, "clear halt %d\n", epnum);
			if (ep & USB_DIR_IN)
				pipe = usb_rcvbulkpipe(usbdev, epnum);
			else
				pipe = usb_sndbulkpipe(usbdev, epnum);
			ret = usb_clear_halt(usbdev, pipe);
			if ((ret == 0) || (ret == -EPIPE))
				return (1);
		}
		break;

	default:
		break;
	}

	urb->interval	       = 1;

	if (usbif_request_dir_in(req)) {
		urb->pipe = usb_rcvctrlpipe(usbdev,
				usbif_request_endpoint_num(req));
		stats->st_in_req++;
	} else {
		urb->pipe = usb_sndctrlpipe(usbdev,
				usbif_request_endpoint_num(req));
		stats->st_out_req++;
	}
	stats->st_cntrl_req++;

	return (ret);
}

static void setup_isoc_urb(struct vusb *vusb, usbif_request_t *req,
				struct urb *urb, struct usb_host_endpoint *ep)
{
	struct usb_device *usbdev = vusb->usbdev;
	usbif_stats_t *stats = &(usbif_from_vusb(vusb)->stats);

	urb->interval          = 1 << min(15, ep->desc.bInterval - 1);
	urb->start_frame       = req->startframe;

	if (usbif_request_asap(req))
		urb->transfer_flags |= URB_ISO_ASAP;

	debug_print(LOG_LVL_DEBUG, "%s: interval %x sf %d packets %d\n",
		__FUNCTION__, urb->interval, urb->start_frame,
		urb->number_of_packets);

	if (usbif_request_dir_in(req)) {
		urb->pipe = usb_rcvisocpipe(usbdev,
				usbif_request_endpoint_num(req));
		stats->st_in_req++;
	} else {
		urb->pipe = usb_sndisocpipe(usbdev,
				usbif_request_endpoint_num(req));
		stats->st_out_req++;
	}
	stats->st_isoc_req++;
}

static void setup_bulk_urb(struct vusb *vusb, usbif_request_t *req,
				struct urb *urb)
{
	struct usb_device *usbdev = vusb->usbdev;
	usbif_stats_t *stats = &(usbif_from_vusb(vusb)->stats);

	debug_print(LOG_LVL_DEBUG, "%s\n", __FUNCTION__);

	urb->interval	       = 1;

	if (usbif_request_dir_in(req)) {
		urb->pipe = usb_rcvbulkpipe(usbdev,
				usbif_request_endpoint_num(req));
		stats->st_in_req++;
	} else {
		urb->pipe = usb_sndbulkpipe(usbdev,
				usbif_request_endpoint_num(req));
		stats->st_out_req++;
	}
	stats->st_bulk_req++;
}

static void setup_int_urb(struct vusb *vusb, usbif_request_t *req,
				struct urb *urb, struct usb_host_endpoint *ep)
{
	struct usb_device *usbdev = vusb->usbdev;
	usbif_stats_t *stats = &(usbif_from_vusb(vusb)->stats);

	switch (usbdev->speed) {
		case USB_SPEED_HIGH:
		case USB_SPEED_SUPER:
			urb->interval = 1 << min(15, ep->desc.bInterval - 1);
			break;
		case USB_SPEED_FULL:
		case USB_SPEED_LOW:
			urb->interval = ep->desc.bInterval;
			break;
		default:
			debug_print(LOG_LVL_ERROR, "%s: bad speed %x\n",
				__FUNCTION__, usbdev->speed);
			break;
	}

	debug_print(LOG_LVL_DEBUG, "%s: interval %x\n", __FUNCTION__,
		urb->interval);

	if (usbif_request_dir_in(req)) {
		urb->pipe = usb_rcvintpipe(usbdev,
				usbif_request_endpoint_num(req));
		stats->st_in_req++;
	} else {
		urb->pipe = usb_sndintpipe(usbdev,
				usbif_request_endpoint_num(req));
		stats->st_out_req++;
	}
	stats->st_int_req++;
}

static struct usb_device *vusb_device(struct vusb *vusb)
{
	return ((vusb->active && vusb->usbdev &&
		dusb_dev_running(vusb->usbdev)) ? vusb->usbdev : NULL);
}

int vusb_setup_urb(struct vusb *vusb, usbif_request_t *req, struct urb *urb)
{
	struct usb_device *usbdev = vusb_device(vusb);
	struct usb_host_endpoint *ep;
	int endpointnum = usbif_request_endpoint_num(req);
	int ret = 0;

	if ((usbdev == NULL) || ((usbdev->state != USB_STATE_ADDRESS) &&
		(usbdev->state != USB_STATE_CONFIGURED))) {
		return -ENODEV;
	}

	if (usbif_request_dir_in(req))
		ep = usbdev->ep_in[endpointnum];
	else
		ep = usbdev->ep_out[endpointnum];
	if (!ep) {
		debug_print(LOG_LVL_ERROR, "endpoint not found (%d)\n", endpointnum);
		return -ENOENT;
	}

	urb->dev = usbdev;
	if (!usbif_request_shortok(req) && usbif_request_dir_in(req))
		urb->transfer_flags |= URB_SHORT_NOT_OK;

	switch((ep->desc.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK)) {
		case USB_ENDPOINT_XFER_CONTROL:
			ret = setup_control_urb(vusb, req, urb);
			break;

		case USB_ENDPOINT_XFER_ISOC:
			setup_isoc_urb(vusb, req, urb, ep);
			break;

		case USB_ENDPOINT_XFER_BULK:
			setup_bulk_urb(vusb, req, urb);
			break;

		default:
		case USB_ENDPOINT_XFER_INT:
			setup_int_urb(vusb, req, urb, ep);
			break;
	}

	return (ret);
}

int vusb_reset_device(struct vusb *vusb)
{
	struct usb_device *usbdev = vusb_device(vusb);
	usbif_stats_t *stats = &(usbif_from_vusb(vusb)->stats);
	int ret;

	if (!usbdev)
		return (-1);

	debug_print(LOG_LVL_ERROR, "%s vusb %p, usbdev %p (%d.%d (port %d)) Start\n",
		    __FUNCTION__, vusb, usbdev,
		    usbdev->bus->busnum, usbdev->devnum, usbdev->portnum);

	/* pre and post reset handlers set and clear canceling_requests */
	usb_lock_device(usbdev);
	ret = usb_reset_device(usbdev);
	usb_unlock_device(usbdev);

	stats->st_reset++;

	debug_print(LOG_LVL_ERROR, "%s vusb %p, usbdev %p (%d.%d (port %d)) Done\n",
		    __FUNCTION__, vusb, usbdev,
		    usbdev->bus->busnum, usbdev->devnum, usbdev->portnum);

	return ret;
}

void vusb_flush(struct vusb *vusb)
{
	debug_print(LOG_LVL_INFO, "%s\n", __FUNCTION__);

	vusb->canceling_requests = 1;

	usb_kill_anchored_urbs(&vusb->anchor);

	vusb->canceling_requests = 0;
}

int vusb_flush_endpoint(struct vusb *vusb, usbif_request_t *req)
{
	int endpointnum = usbif_request_endpoint_num(req);
	struct usb_device *usbdev = vusb_device(vusb);
	struct usb_host_endpoint *ep;

	debug_print(LOG_LVL_DEBUG, "%s udev %p\n", __FUNCTION__, usbdev);

	if (usbdev) {
		if (usbif_request_dir_in(req))
			ep = usbdev->ep_in[endpointnum];
		else
			ep = usbdev->ep_out[endpointnum];
		if (!ep) {
			debug_print(LOG_LVL_ERROR, "endpoint not found (%d)\n", endpointnum);
			return -ENOENT;
		}

		vusb->canceling_requests = 1;

		dusb_flush_endpoint(usbdev, ep);

		vusb->canceling_requests = 0;
	} else {
		vusb_flush(vusb);
	}

	debug_print(LOG_LVL_DEBUG, "%s - udev %p end\n", __FUNCTION__, usbdev);

	return (0);
}

int vusb_get_speed(struct vusb *vusb)
{
	struct usb_device *usbdev = vusb_device(vusb);

	return (usbdev ? usbdev->speed : -1);
}

void vusb_free_coherent(struct vusb *vusb, struct urb *urb)
{
	struct usb_device *usbdev = urb->dev ? urb->dev : vusb->usbdev;

	if (usbdev)
		usb_free_coherent(usbdev, urb->transfer_buffer_length,
		        urb->transfer_buffer, urb->transfer_dma);
	else
		debug_print(LOG_LVL_ERROR, "%s: leaking buffer! no dev!",
			__FUNCTION__);
	urb->transfer_buffer = NULL;
}

void *vusb_alloc_coherent(struct vusb *vusb, size_t size, dma_addr_t *dma)
{
	struct usb_device *usbdev = vusb_device(vusb);

	void *ret = (usbdev ? usb_alloc_coherent(usbdev, size, GFP_KERNEL, dma) : NULL);

	if (!ret) {
		debug_print((usbdev != NULL) ? LOG_LVL_DEBUG : LOG_LVL_ERROR,
			"%s: Failed: vusb:%p, udbdev:%p, "
			"active:%d, running:%s\n",
			__FUNCTION__,
			vusb, vusb->usbdev, vusb->active,
			usbdev && dusb_dev_running(usbdev) ? "yes" : "no");
	}

	return (ret);
}

void vusb_cycle_port(struct vusb *vusb)
{
	struct usb_device *usbdev = vusb_device(vusb);

	if (usbdev) {
		debug_print(LOG_LVL_ERROR, "%s vusb %p, usbdev %p (%d.%d (port %d)) Start\n",
			__FUNCTION__, vusb, usbdev,
			usbdev->bus->busnum, usbdev->devnum, usbdev->portnum);
		usb_device_reenumerate(usbdev);
		debug_print(LOG_LVL_ERROR, "%s vusb %p, usbdev %p (%d.%d (port %d)) Done\n",
			__FUNCTION__, vusb, usbdev,
			usbdev->bus->busnum, usbdev->devnum, usbdev->portnum);
	}
}


/* power management methods */
void vusb_pm_autosuspend_control(struct vusb *vusb, int enable)
{
	struct usb_device *usbdev = vusb_device(vusb);

	if (usbdev && (vusb->autosuspend != enable)) {
		debug_print(LOG_LVL_INFO, "%s vusb %p, udev %p enable %d\n",
			__FUNCTION__, vusb, usbdev, enable);

		vusb->autosuspend = enable;
		if (enable)
			usb_enable_autosuspend(usbdev);
		else
			usb_disable_autosuspend(usbdev);
	}
}

