/******************************************************************************
 *
 * usb device interface management.
 *
 * Copyright (c) 2004, Keir Fraser
 * Copyright (c) 2008-2012, Virtual Computer Inc.
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

#include <linux/kthread.h>
#include <xen/interface/xen.h>
#include <xen/evtchn.h>
#include <xen/events.h>
#include <asm/xen/hypercall.h>

#include "common.h"

static struct kmem_cache *usbif_cachep;

usbif_t *usbif_alloc(domid_t domid)
{
	usbif_t *usbif;

	usbif = kmem_cache_alloc(usbif_cachep, GFP_KERNEL);
	if (!usbif)
		return ERR_PTR(-ENOMEM);

	memset(usbif, 0, sizeof(*usbif));
	usbif->domid = domid;
	spin_lock_init(&usbif->usb_ring_lock);
	atomic_set(&usbif->refcnt, 1);
	init_waitqueue_head(&usbif->wq);
	usbif->st_print = jiffies;
	init_waitqueue_head(&usbif->waiting_to_free);

	return usbif;
}

int usbif_map(usbif_t *usbif, grant_ref_t shpage_ref, unsigned int evtchn)
{
	int err;

	/* Already connected through? */
	if (usbif->irq)
		return 0;

	debug_print(LOG_LVL_INFO, "Map shared ring, connect event channel\n");

	/* Call the xenbus function to map the shared page. It handles the case
	 * where alloc_vm_area is done in a process context that is not init
	 * but only the init_mm tables are updated. Normally a fault would
	 * correct this in other processes but the supsequent hypercall blocks
	 * that fault handling. Therefore in the hypercall it sees the PTE's
	 * not populated. The xenbus routine also tracks the vm area allocation
	 * and the op.handle for cleanup.
	 */
	err = xenbus_map_ring_valloc(usbif->be->dev,
			&shpage_ref, 1, &(usbif->usb_ring_addr));
	if (err)
		return err;

	switch (usbif->usb_protocol) {
	case USBIF_PROTOCOL_NATIVE:
	{
		struct usbif_sring *sring;
		sring = (struct usbif_sring *)usbif->usb_ring_addr;
		BACK_RING_INIT(&usbif->usb_rings.native, sring, PAGE_SIZE);
		break;
	}
	case USBIF_PROTOCOL_X86_32:
	{
		struct usbif_x86_32_sring *sring_x86_32;
		sring_x86_32 = (struct usbif_x86_32_sring *)usbif->usb_ring_addr;
		BACK_RING_INIT(&usbif->usb_rings.x86_32, sring_x86_32, PAGE_SIZE);
		break;
	}
	case USBIF_PROTOCOL_X86_64:
	{
		struct usbif_x86_64_sring *sring_x86_64;
		sring_x86_64 = (struct usbif_x86_64_sring *)usbif->usb_ring_addr;
		BACK_RING_INIT(&usbif->usb_rings.x86_64, sring_x86_64, PAGE_SIZE);
		break;
	}
	default:
		BUG();
	}

	err = bind_interdomain_evtchn_to_irqhandler_lateeoi(
		usbif->domid, evtchn, usbif_be_int, 0, "usbif-backend", usbif);
	if (err < 0)
	{
		xenbus_unmap_ring_vfree(usbif->be->dev, usbif->usb_ring_addr);
		usbif->usb_rings.common.sring = NULL;
		usbif->usb_ring_addr = NULL;
		return err;
	}
	usbif->irq = err;

	return 0;
}

void usbif_kill_xenusbd(usbif_t *usbif)
{
	struct task_struct *xenusbd = xchg(&usbif->xenusbd, NULL);

	if (xenusbd && !IS_ERR(xenusbd))
		kthread_stop(xenusbd);
}

void usbif_disconnect(usbif_t *usbif, struct xenbus_device *dev)
{
	debug_print(LOG_LVL_INFO, "Disconnect shared ring and event channel\n");
	usbif_kill_xenusbd(usbif);

	atomic_dec(&usbif->refcnt);
	wait_event(usbif->waiting_to_free, atomic_read(&usbif->refcnt) == 0);
	atomic_inc(&usbif->refcnt);

	if (usbif->irq) {
		unbind_from_irqhandler(usbif->irq, usbif);
		usbif->irq = 0;
	}

	if (usbif->usb_rings.common.sring) {
		xenbus_unmap_ring_vfree(dev, usbif->usb_ring_addr);
		usbif->usb_rings.common.sring = NULL;
		usbif->usb_ring_addr = NULL;
	}
}

void usbif_free(usbif_t *usbif)
{
	if (!atomic_dec_and_test(&usbif->refcnt))
		BUG();
	kmem_cache_free(usbif_cachep, usbif);
}

void __init usbif_interface_init(void)
{
	usbif_cachep = kmem_cache_create("usbif_cache", sizeof(usbif_t),
					 0, 0, NULL);
}
