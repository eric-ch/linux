/*  Xenbus code for usbif backend
    Copyright (C) 2005 Rusty Russell <rusty@rustcorp.com.au>
    Copyright (C) 2005 XenSource Ltd
    Copyright (C) 2008-2012 Virtual Computer Inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include <stdarg.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include "common.h"

#undef DPRINTK
#define DPRINTK(fmt, args...)				\
	pr_debug("usbback/xenbus (%s:%d) " fmt ".\n",	\
		 __FUNCTION__, __LINE__, ##args)

static void connect(struct backend_info *);
static int connect_ring(struct backend_info *);

static void update_usbif_status(usbif_t *usbif)
{
	int err;
	char name[TASK_COMM_LEN];

	/* Not ready to connect? */
	if (!usbif->irq)
		return;

	/* Already connected? */
	if (usbif->be->dev->state == XenbusStateConnected)
		return;

	/* Attempt to connect: exit if we fail to. */
	connect(usbif->be);
	if (usbif->be->dev->state != XenbusStateConnected)
		return;

	snprintf(name, TASK_COMM_LEN, "usbback.%d.%d.%d",
		usbif->domid, usbif->be->bus, usbif->be->device);

	usbif->xenusbd = kthread_run(usbif_schedule, usbif, name);
	if (IS_ERR(usbif->xenusbd)) {
		err = PTR_ERR(usbif->xenusbd);
		usbif->xenusbd = NULL;
		xenbus_dev_error(usbif->be->dev, err, "start xenusbd");
	}
	else
		debug_print(LOG_LVL_DEBUG, "Started xenusbd\n");
}

/****************************************************************
 *  sysfs interface for VUSB I/O requests
 */
#define USB_SHOW(name, format, args...)                                 \
        static ssize_t show_##name(struct device *_dev,                 \
                                   struct device_attribute *attr,       \
                                   char *buf)                           \
        {                                                               \
                ssize_t ret = -ENODEV;                                  \
                struct xenbus_device *dev;                              \
                struct backend_info *be;                                \
                                                                        \
                if (!get_device(_dev))                                  \
                        return ret;                                     \
                dev = to_xenbus_device(_dev);                           \
                if ((be = dev_get_drvdata(&dev->dev)) != NULL)          \
                        ret = sprintf(buf, format, ##args);             \
                put_device(_dev);                                       \
                return ret;                                             \
        }                                                               \
        static DEVICE_ATTR(name, S_IRUGO, show_##name, NULL)


USB_SHOW(oo_req,  "%d\n", be->usbif->stats.st_oo_req);
USB_SHOW(in_req,  "%d\n", be->usbif->stats.st_in_req);
USB_SHOW(out_req,  "%d\n", be->usbif->stats.st_out_req);

USB_SHOW(error,  "%d\n", be->usbif->stats.st_error);
USB_SHOW(reset,  "%d\n", be->usbif->stats.st_reset);

USB_SHOW(in_bandwidth,  "%d\n", be->usbif->stats.st_in_bandwidth);
USB_SHOW(out_bandwidth,  "%d\n", be->usbif->stats.st_out_bandwidth);

USB_SHOW(cntrl_req,  "%d\n", be->usbif->stats.st_cntrl_req);
USB_SHOW(isoc_req, "%d\n", be->usbif->stats.st_isoc_req);
USB_SHOW(bulk_req, "%d\n", be->usbif->stats.st_bulk_req);
USB_SHOW(int_req, "%d\n", be->usbif->stats.st_int_req);

static struct attribute *usbstat_attrs[] = {
	&dev_attr_oo_req.attr,
	&dev_attr_in_req.attr,
	&dev_attr_out_req.attr,
	&dev_attr_error.attr,
	&dev_attr_reset.attr,
	&dev_attr_in_bandwidth.attr,
	&dev_attr_out_bandwidth.attr,
	&dev_attr_cntrl_req.attr,
	&dev_attr_isoc_req.attr,
	&dev_attr_bulk_req.attr,
	&dev_attr_int_req.attr,
	NULL
};

static struct attribute_group usbstat_group = {
	.name = "statistics",
	.attrs = usbstat_attrs,
};

USB_SHOW(physical_device, "%x.%x\n", be->bus, be->device);

int xenusb_sysfs_addif(struct xenbus_device *dev)
{
	int error;

	error = device_create_file(&dev->dev, &dev_attr_physical_device);
        if (error)
		goto fail1;

	error = sysfs_create_group(&dev->dev.kobj, &usbstat_group);
	if (error)
		goto fail2;

	return 0;

fail2:	sysfs_remove_group(&dev->dev.kobj, &usbstat_group);
fail1:	device_remove_file(&dev->dev, &dev_attr_physical_device);
	return error;
}

void xenusb_sysfs_delif(struct xenbus_device *dev)
{
	sysfs_remove_group(&dev->dev.kobj, &usbstat_group);
	device_remove_file(&dev->dev, &dev_attr_physical_device);
}

static int usbback_remove(struct xenbus_device *dev)
{
	struct backend_info *be = dev_get_drvdata(&dev->dev);

	debug_print(LOG_LVL_ERROR, "usbback_remove\n");

	if (be->bus || be->device)
		xenusb_sysfs_delif(dev);

	if (be->backend_watch.node) {
		unregister_xenbus_watch(&be->backend_watch);
		kfree(be->backend_watch.node);
		be->backend_watch.node = NULL;
	}

	if (be->autosuspend_watch.node) {
		unregister_xenbus_watch(&be->autosuspend_watch);
		kfree(be->autosuspend_watch.node);
		be->autosuspend_watch.node = NULL;
	}

	if (be->usbif) {
		usbif_t *usbif = be->usbif;

		/*
		 * Disconnect the be and usbif since the call to vusb_free can
		 * cause callbacks like usbback_suspend which dereference
		 * usbif to be and make calls on the be.
		 */
		be->usbif = NULL;
		usbif->be = NULL;
		/*
		 * Kill the per device kthread so we don't process any more
		 * frontend requests.
		 */
		debug_print(LOG_LVL_ERROR, "Disconnecting vusb %p\n", &usbif->vusb);
		usbif_disconnect(usbif, be->dev);
		/* Shutdown the Linux USB class driver */
		debug_print(LOG_LVL_ERROR, "Freeing vusb %p\n", &usbif->vusb);
		vusb_free(&usbif->vusb);
		usbif_free(usbif);
	}

	kfree(be);
	dev_set_drvdata(&dev->dev, NULL);
	return 0;
}

int usbback_barrier(struct xenbus_transaction xbt,
		    struct backend_info *be, int state)
{
	struct xenbus_device *dev = be->dev;
	int err;

	err = xenbus_printf(xbt, dev->nodename, "feature-barrier",
			    "%d", state);
	if (err)
		xenbus_dev_fatal(dev, err, "writing feature-barrier");

	return err;
}

/* tell the frontend that the device's suspend state has changed */
int usbback_suspend(usbif_t *usbif, int suspended)
{
	struct xenbus_device *dev = usbif->be ? usbif->be->dev : NULL;
	int err;

	debug_print(LOG_LVL_ERROR, "%s: usbif %p dev %p node %s\n",
		__FUNCTION__, usbif, dev, dev ? dev->nodename : "");

	if (dev)
		err = 0;
	else
		err = -ENODEV;

	return err;
}

/**
 * Callback received when the hotplug scripts have placed the physical-device
 * node.  Read it and create a vusb.  If the frontend is ready, connect.
 */
static void backend_changed(struct xenbus_watch *watch,
			    const char *path, const char *token)
{
	int err;
	unsigned bus;
	unsigned device;
	struct backend_info *be
		= container_of(watch, struct backend_info, backend_watch);
	struct xenbus_device *dev = be->dev;

	err = xenbus_scanf(XBT_NIL, dev->nodename, "physical-device", "%d.%d",
			   &bus, &device);
	if (XENBUS_EXIST_ERR(err)) {
		/* Since this watch will fire once immediately after it is
		   registered, we expect this.  Ignore it, and wait for the
		   hotplug scripts. */
		return;
	}
	if (err != 2) {
		xenbus_dev_fatal(dev, err, "reading physical-device");
		return;
	}

	if ((be->bus || be->device) && (bus || device) &&
	    ((be->bus != bus) || (be->device != device))) {
		debug_print(LOG_LVL_ERROR,
		       "usbback: changing physical device (from %x.%x to "
		       "%x.%x) not supported.\n", be->bus, be->device,
		       bus, device);
		return;
	}

	if (be->bus == 0 && be->device == 0) {
		/* Front end dir is a number, which is used as the handle. */

		char *p = strrchr(dev->otherend, '/') + 1;
		long handle = simple_strtoul(p, NULL, 0);

		be->bus = bus;
		be->device = device;

		err = vusb_create(be->usbif, handle, bus, device);
		if (err) {
			be->bus = be->device = 0;
			xenbus_dev_fatal(dev, err, "creating vusb structure");
			return;
		}

		err = xenusb_sysfs_addif(dev);
		if (err) {
			vusb_free(&be->usbif->vusb);
			be->bus = be->device = 0;
			xenbus_dev_fatal(dev, err, "creating sysfs entries");
			return;
		}

		/* We're potentially connected now */
		update_usbif_status(be->usbif);
	} else if (bus == 0 && device == 0) {
		/* Device is being unassigned -- simulate hot unplug */
		vusb_cycle_port(&be->usbif->vusb);
	}
}

/**
 * Callback received when the frontend changes the atosuspend element.
 */
static void autosuspend_changed(struct xenbus_watch *watch,
			    const char *path, const char* token)
{
	struct backend_info *be
		= container_of(watch, struct backend_info, autosuspend_watch);
	struct xenbus_device *dev = be->dev;
	unsigned autosuspend;
	int err;

	err = xenbus_scanf(XBT_NIL, dev->otherend, "autosuspend", "%d",
			   &autosuspend);
	if (XENBUS_EXIST_ERR(err)) {
		/* Since this watch will fire once immediately after it is
		   registered, we expect this.  Ignore it, and wait for the
		   hotplug scripts. */
		return;
	}
	if (err != 1) {
		xenbus_dev_error(dev, err, "reading autosuspend");
		return;
	}

	err = xenbus_scanf(XBT_NIL, dev->otherend, "autosuspend", "%d",
			   &autosuspend);
	vusb_pm_autosuspend_control(&be->usbif->vusb, autosuspend);

	debug_print(LOG_LVL_INFO, "Autosuspend changed %d\n", autosuspend);
}


/**
 * Entry point to this code when a new device is created.  Allocate the basic
 * structures, and watch the store waiting for the hotplug scripts to tell us
 * the device's physical major and minor numbers.  Switch to InitWait.
 */
#define VERSION_SZ 4
static int usbback_probe(struct xenbus_device *dev,
			 const struct xenbus_device_id *id)
{
	int err;
	char version[VERSION_SZ];
	struct backend_info *be = kzalloc(sizeof(struct backend_info),
					  GFP_KERNEL);
	if (!be) {
		xenbus_dev_fatal(dev, -ENOMEM,
				 "allocating backend structure");
		return -ENOMEM;
	}
	be->dev = dev;
	dev_set_drvdata(&dev->dev, be);

	be->usbif = usbif_alloc(dev->otherend_id);
	if (IS_ERR(be->usbif)) {
		err = PTR_ERR(be->usbif);
		be->usbif = NULL;
		xenbus_dev_fatal(dev, err, "creating block interface");
		goto fail;
	}

	/* setup back pointer */
	be->usbif->be = be;

	err = xenbus_watch_pathfmt(dev, &be->backend_watch, NULL, backend_changed,
				   "%s/%s", dev->nodename, "physical-device");
	if (err)
		goto fail;

	err = xenbus_watch_pathfmt(dev, &be->autosuspend_watch, NULL,
				   autosuspend_changed, "%s/%s", dev->otherend,
				   "autosuspend");
	if (err)
		goto fail;

	debug_print(LOG_LVL_ERROR, "Setup watch for %s/%s\n", dev->otherend, "autosuspend");

        err = snprintf(version, VERSION_SZ, "%d", USBBCK_VERSION);
        if (err < 0)
                goto fail;

	err = xenbus_write(XBT_NIL, dev->nodename, "version", version);
	if (err)
                goto fail;

	err = xenbus_switch_state(dev, XenbusStateInitWait);
	if (err)
		goto fail;

	return 0;

fail:
	debug_print(LOG_LVL_ERROR, "Probe failed\n");
	usbback_remove(dev);
	return err;
}

/**
 * Callback received when the frontend's state changes.
 */
static void frontend_changed(struct xenbus_device *dev,
			     enum xenbus_state frontend_state)
{
	struct backend_info *be = dev_get_drvdata(&dev->dev);
	int err;

	debug_print(LOG_LVL_INFO, "Frontend state: %s Backend state: %s\n",
		xenbus_strstate(frontend_state), xenbus_strstate(dev->state));

	switch (frontend_state) {
	case XenbusStateInitialising:
		if (dev->state == XenbusStateClosed) {
			printk(KERN_INFO "%s: %s: prepare for reconnect\n",
			       __FUNCTION__, dev->nodename);
			xenbus_switch_state(dev, XenbusStateInitWait);
		}
		break;

	case XenbusStateInitialised:
	case XenbusStateConnected:
		/* Ensure we connect even when two watches fire in
		   close successsion and we miss the intermediate value
		   of frontend_state. */
		if (dev->state == XenbusStateConnected)
			break;

		err = connect_ring(be);
		if (err)
			break;
		update_usbif_status(be->usbif);
		break;

	case XenbusStateClosing:
		usbif_disconnect(be->usbif, be->dev);
		xenbus_switch_state(dev, XenbusStateClosing);
		break;

	case XenbusStateClosed:
		xenbus_switch_state(dev, XenbusStateClosed);
		if (xenbus_dev_is_online(dev))
			break;
		/* fall through if not online */
	case XenbusStateUnknown:
		device_unregister(&dev->dev);
		break;

	default:
		xenbus_dev_fatal(dev, -EINVAL, "saw state %d at frontend",
				 frontend_state);
		break;
	}
}


/* ** Connection ** */


/**
 * Write the physical details regarding the usb device to the store, and
 * switch to Connected state.
 */
static void connect(struct backend_info *be)
{
	struct xenbus_transaction xbt;
	int err;
	struct xenbus_device *dev = be->dev;

	debug_print(LOG_LVL_INFO, "Connect: %s\n", dev->otherend);

	/* Supply the information about the device the frontend needs */
again:
	err = xenbus_transaction_start(&xbt);
	if (err) {
		xenbus_dev_fatal(dev, err, "starting transaction");
		return;
	}

	err = usbback_barrier(xbt, be, 1);
	if (err)
		goto abort;

	err = xenbus_transaction_end(xbt, 0);
	if (err == -EAGAIN)
		goto again;
	if (err)
		xenbus_dev_fatal(dev, err, "ending transaction");

	err = xenbus_switch_state(dev, XenbusStateConnected);
	if (err)
		xenbus_dev_fatal(dev, err, "switching to Connected state");

	return;
 abort:
	xenbus_transaction_end(xbt, 1);
}

static int connect_ring(struct backend_info *be)
{
	struct xenbus_device *dev = be->dev;
	grant_ref_t ring_ref;
	unsigned int evtchn;
	unsigned int version;
	char protocol[64] = "";
	int err;

	debug_print(LOG_LVL_INFO, "Connect ring: %s\n", dev->otherend);

        err = xenbus_scanf(XBT_NIL, dev->otherend, "version", "%d",
                           &version);
        if (XENBUS_EXIST_ERR(err)) {
                debug_print(LOG_LVL_ERROR, "frontend version doesn't exist, must be old\n");
                return -1;
        }
        if (err != 1) {
                xenbus_dev_fatal(dev, err, "reading version");
                return -1;
        }
        debug_print(LOG_LVL_INFO, "frontend version %d\n", version);
        if (version < USBBCK_VERSION) {
                xenbus_dev_fatal(dev, EINVAL, "frontend doesn't match backend (%d)", version);
		return -1;
	}

	err = xenbus_gather(XBT_NIL, dev->otherend, "ring-ref", "%lu", &ring_ref,
			    "event-channel", "%u", &evtchn, NULL);
	if (err) {
		xenbus_dev_fatal(dev, err,
				 "reading %s/ring-ref and event-channel",
				 dev->otherend);
		return err;
	}

	be->usbif->usb_protocol = USBIF_PROTOCOL_NATIVE;
	err = xenbus_gather(XBT_NIL, dev->otherend, "protocol",
			    "%63s", protocol, NULL);
	if (err) {
		strcpy(protocol, "unspecified");
//		be->usbif->usb_protocol = xen_guest_usbif_protocol(be->usbif->domid);
	}
	else if (0 == strcmp(protocol, XEN_IO_PROTO_ABI_NATIVE))
		be->usbif->usb_protocol = USBIF_PROTOCOL_NATIVE;
	else if (0 == strcmp(protocol, XEN_IO_PROTO_ABI_X86_32))
		be->usbif->usb_protocol = USBIF_PROTOCOL_X86_32;
	else if (0 == strcmp(protocol, XEN_IO_PROTO_ABI_X86_64))
		be->usbif->usb_protocol = USBIF_PROTOCOL_X86_64;
	else {
		xenbus_dev_fatal(dev, err, "unknown fe protocol %s", protocol);
		return -1;
	}
	debug_print(LOG_LVL_INFO,
		"usbback: ring-ref %d, event-channel %d, protocol %d (%s)\n",
		ring_ref, evtchn, be->usbif->usb_protocol, protocol);

	/* Map the shared frame, irq etc. */
	err = usbif_map(be->usbif, ring_ref, evtchn);
	if (err) {
		xenbus_dev_fatal(dev, err, "mapping ring-ref %d port %u",
				 ring_ref, evtchn);
		return err;
	}

	return 0;
}


/*
 * Driver Registration
 */
static const struct xenbus_device_id usbback_ids[] = {
	{ "vusb" },
	{ "" }
};

static struct xenbus_driver usbback_driver = {
	.name			= "usbback",
	.ids			= usbback_ids,
	.probe			= usbback_probe,
	.remove			= usbback_remove,
	.otherend_changed	= frontend_changed
};

int usbif_xenbus_init(void)
{
	return xenbus_register_backend(&usbback_driver);
}
