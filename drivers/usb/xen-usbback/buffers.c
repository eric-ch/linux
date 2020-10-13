/******************************************************************************
 * usbback/buffers.c
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

#include <linux/scatterlist.h>

#include "common.h"

#if (DUMP_URB_SZ > 0)
void dump(uint8_t *buffer, int len)
{
	int i;

	if ((buffer != NULL) && (len > 0)) {
		printk("    data: ");
		for (i=0; (i < len) && (i < DUMP_URB_SZ); i++) {
			printk("%02x ", buffer[i]);
			if ((i & 0x3f) == 0x20)
				printk("\n");
		}
		printk("\n");
	} else {
		printk("    data: none\n");
	}
}

static void dump_iso_urb(struct urb *urb)
{
	struct usb_iso_packet_descriptor *desc = urb->iso_frame_desc;
	uint8_t *buffer = urb->transfer_buffer;
	int i;

	for (i=0; i<urb->number_of_packets; i++)
		dump(&buffer[desc[i].offset], desc[i].length);
}
#endif

/*
 * Move data between DomU and URB buffers
 */
static int copy_first_chunk(uint8_t *dst, uint8_t *src, int offset, int remaining)
{
	int len;

	if ((PAGE_SIZE - offset) > remaining)
		len = remaining;
	else
		len = (PAGE_SIZE - offset);

	memcpy(dst, src, len);

	return len;
}

static int copy_chunk(uint8_t *dst, uint8_t *src, int remaining)
{
	return copy_first_chunk(dst, src, 0, remaining);
}

static void copy_out_req(pending_req_t *pending_req)
{
	uint8_t *dst, *src;
	int i, len, remaining, nr_pages;

	remaining = pending_req->urb->transfer_buffer_length;
	nr_pages = data_pages(pending_req);

	if (!nr_pages)
		return;

	/* copy first seg */
	dst = pending_req->urb->transfer_buffer;
	src = (uint8_t *)vaddr(pending_req, 0) + pending_req->offset;

	len = copy_first_chunk(dst, src, pending_req->offset, remaining);

	dst += len;
	remaining -= len;

	/* copy remaining segs */
	for (i = 1; i < nr_pages; i++) {
		src = (uint8_t *)vaddr(pending_req, i);

		len = copy_chunk(dst, src, remaining);

		dst += len;
		remaining -= len;
	}

#if (DUMP_URB_SZ > 0)
	if (usbback_debug_lvl() >= LOG_LVL_DUMP)
		dump(pending_req->urb->transfer_buffer,
			pending_req->urb->transfer_buffer_length);
#endif
}

static int setup_sg(struct scatterlist *sg, uint8_t *src, int offset, int remaining)
{
	int len;

	if ((PAGE_SIZE - offset) > remaining)
		len = remaining;
	else
		len = (PAGE_SIZE - offset);

	debug_print(LOG_LVL_DEBUG, "  sg: ptr %p len %d\n", src + offset, len);

	sg_set_buf(sg, src + offset, len);

#if (DUMP_URB_SZ > 0)
	if (usbback_debug_lvl() >= LOG_LVL_DUMP)
		dump(src + offset, len);
#endif

	return len;
}

static void setup_sgs(pending_req_t *pending_req, int iso)
{
	struct urb *urb = pending_req->urb;
	int i, len, remaining;
	uint8_t *src;

	remaining = urb->transfer_buffer_length;
	if (iso)
		urb->num_sgs = data_pages(pending_req) - 1;
	else
		urb->num_sgs = data_pages(pending_req);

	sg_init_table(urb->sg, urb->num_sgs);

	/* setup first seg */
	if (iso)
		src = (uint8_t *)vaddr(pending_req, 1);
	else
		src = (uint8_t *)vaddr(pending_req, 0);

	len = setup_sg(&urb->sg[0], src, pending_req->offset, remaining);
	debug_print(LOG_LVL_DEBUG, "%d: sg: off %d len %d\n",
			0, pending_req->offset, len);

	remaining -= len;

	/* setup remaining segs */
	for (i = 1; i < urb->num_sgs; i++) {
		if (iso)
			src = (uint8_t *)vaddr(pending_req, i + 1);
		else
			src = (uint8_t *)vaddr(pending_req, i);

		len = setup_sg(&urb->sg[i], src, 0, remaining);
		debug_print(LOG_LVL_DEBUG, "%d: sg: off %d len %d\n",
			i, 0, len);

		remaining -= len;
	}
}

static int copy_out_iso_descriptors(pending_req_t *pending_req)
{
	struct usb_iso_packet_descriptor *desc;
	usbif_iso_packet_info_t *info;
	int i, length = 0;

	if (!data_pages(pending_req))
		return (length);

	/* copy ISO packet descriptors */
	info = (usbif_iso_packet_info_t *)vaddr(pending_req, 0);
	desc = pending_req->urb->iso_frame_desc;

	for (i=0; i<pending_req->nr_packets; i++) {
		int end = info[i].offset + info[i].length;

		debug_print(LOG_LVL_DEBUG, "  %d: iso desc: off %d len %d\n",
			i, info[i].offset, info[i].length);
		desc[i].offset        = info[i].offset;
		desc[i].length        = info[i].length;
		desc[i].actual_length = 0;
		desc[i].status        = 0;

		if (end > length)
			length = end;
	}

	return (length);
}

static int copy_out_iso(pending_req_t *pending_req)
{
	uint8_t *dst, *src;
	int i, len, remaining, nr_pages;

	remaining = pending_req->urb->transfer_buffer_length;
	nr_pages = data_pages(pending_req);

	if (nr_pages < 2)
		return (0);

	/* copy first seg */
	dst = pending_req->urb->transfer_buffer;
	src = (uint8_t *)vaddr(pending_req, 1) + pending_req->offset;

	len = copy_first_chunk(dst, src, pending_req->offset, remaining);

	dst       += len;
	remaining -= len;

	/* copy remaining segs */
	for (i = 2; i < nr_pages; i++) {
		src = (uint8_t *)vaddr(pending_req, i);

		len = copy_chunk(dst, src, remaining);

		dst       += len;
		remaining -= len;
	}

#if (DUMP_URB_SZ > 0)
	if (usbback_debug_lvl() >= LOG_LVL_DUMP)
		dump_iso_urb(pending_req->urb);
#endif
	return (0);
}

int copy_out(pending_req_t *pending_req)
{
	struct urb *urb = pending_req->urb;

	if (pending_req->type == USBIF_T_ISOC) {
		if (copy_out_iso_descriptors(pending_req) >
			pending_req->urb->transfer_buffer_length)
			return (-EINVAL);

		if (urb->sg)
			setup_sgs(pending_req, 1);
		else if (!pending_req->direction_in)
			copy_out_iso(pending_req);
	} else {
		if (urb->sg)
			setup_sgs(pending_req, 0);
		else if (!pending_req->direction_in)
			copy_out_req(pending_req);
	}

	return (0);
}

static void copy_in_req(pending_req_t *pending_req)
{
	uint8_t *dst, *src;
	int i, len, remaining, nr_pages;

	remaining = pending_req->urb->actual_length;
	nr_pages = data_pages(pending_req);

	if (!nr_pages)
		return;

	/* copy first seg */
	dst = (uint8_t *)vaddr(pending_req, 0) + pending_req->offset;
	src = pending_req->urb->transfer_buffer;

	len = copy_first_chunk(dst, src, pending_req->offset, remaining);

	src += len;
	remaining -= len;

	/* copy remaining segs */
	for (i = 1; i < nr_pages; i++) {
		dst = (uint8_t *)vaddr(pending_req, i);

		len = copy_chunk(dst, src, remaining);

		src += len;
		remaining -= len;
	}

#if (DUMP_URB_SZ > 0)
	if (usbback_debug_lvl() >= LOG_LVL_DUMP)
		dump(pending_req->urb->transfer_buffer,
			pending_req->urb->actual_length);
#endif
}

static void cleanup_sgs(pending_req_t *pending_req)
{
	struct urb *urb = pending_req->urb;
	int i;

	debug_print(LOG_LVL_DEBUG, "sgs: total %d mapped %d\n",
		urb->num_sgs, urb->num_mapped_sgs);

	for (i = 0; i < urb->num_sgs; i++) {
		struct scatterlist *sg = &urb->sg[i];

		debug_print(LOG_LVL_DEBUG, "  %d: ptr %p len %d\n",
			i, sg_virt(sg), sg->length);

#if (DUMP_URB_SZ > 0)
		if (usbback_debug_lvl() >= LOG_LVL_DUMP)
			dump(sg_virt(sg), sg->length);
#endif
	}
}

static int copy_in_iso_descriptors(pending_req_t *pending_req)
{
	struct usb_iso_packet_descriptor *desc;
	usbif_iso_packet_info_t *info;
	int i, length = 0;

	if (!data_pages(pending_req))
		return (length);

	/* copy ISO packet descriptors */
	info = (usbif_iso_packet_info_t *)vaddr(pending_req, 0);
	desc = pending_req->urb->iso_frame_desc;

	debug_print(LOG_LVL_DEBUG, "iso descs: %d info %p desc %p\n",
		pending_req->nr_packets, info, desc);

	for (i=0; i<pending_req->nr_packets; i++) {
		int end = desc[i].offset + desc[i].actual_length;

		if (pending_req->direction_in) {
		 	info[i].length = desc[i].actual_length;
			info[i].status = get_usb_status(desc[i].status);
		}
		debug_print(LOG_LVL_DEBUG,
			"  %d: iso desc: off %d len %d status %d\n",
			i, desc[i].offset, desc[i].length, desc[i].status);

		if (end > length)
			length = end;
	}

	return (length);
}

static void copy_in_iso(pending_req_t *pending_req, int remaining)
{
	uint8_t *dst, *src;
	int i, len, nr_pages;

	nr_pages = data_pages(pending_req);

	if (data_pages(pending_req) < 2)
		return;

	/* copy first seg */
	dst = (uint8_t *)vaddr(pending_req, 1) + pending_req->offset;
	src = pending_req->urb->transfer_buffer;

	len = copy_first_chunk(dst, src, pending_req->offset, remaining);

	src       += len;
	remaining -= len;

	/* copy remaining segs */
	for (i = 2; i < nr_pages; i++) {
		dst = (uint8_t *)vaddr(pending_req, i);

		len = copy_chunk(dst, src, remaining);

		src       += len;
		remaining -= len;
	}

#if (DUMP_URB_SZ > 0)
	if (usbback_debug_lvl() >= LOG_LVL_DUMP)
		dump_iso_urb(pending_req->urb);
#endif
}

void copy_in(pending_req_t *pending_req)
{
	struct urb *urb = pending_req->urb;

	if (pending_req->type == USBIF_T_ISOC) {
		int remaining = copy_in_iso_descriptors(pending_req);

		if (urb->sg)
			cleanup_sgs(pending_req);
		else if (pending_req->direction_in)
			copy_in_iso(pending_req, remaining);
	} else {
		if (urb->sg)
			cleanup_sgs(pending_req);
		else if (pending_req->direction_in)
			copy_in_req(pending_req);
	}
}

