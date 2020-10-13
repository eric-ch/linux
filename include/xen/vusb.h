#ifndef __XEN_VUSB_H__
#define __XEN_VUSB_H__

#include <xen/interface/io/ring.h>
#include <xen/interface/io/usbif.h>
#include <xen/interface/io/protocols.h>

/* Not a real protocol.  Used to generate ring structs which contain
 * the elements common to all protocols only.  This way we get a
 * compiler-checkable way to use common struct elements, so we can
 * avoid using switch(protocol) in a number of places.  */
struct usbif_common_request {
	char dummy;
};
struct usbif_common_response {
	char dummy;
};

/* i386 protocol version */
typedef struct usbif_request usbif_x86_32_request_t;
typedef struct usbif_response usbif_x86_32_response_t;


/* x86_64 protocol version */
typedef struct usbif_request usbif_x86_64_request_t;
typedef struct usbif_response usbif_x86_64_response_t;

DEFINE_RING_TYPES(usbif_common, struct usbif_common_request, struct usbif_common_response);
DEFINE_RING_TYPES(usbif_x86_32, struct usbif_request, struct usbif_response);
DEFINE_RING_TYPES(usbif_x86_64, struct usbif_request, struct usbif_response);

union usbif_back_rings {
	struct usbif_back_ring        native;
	struct usbif_common_back_ring common;
	struct usbif_x86_32_back_ring x86_32;
	struct usbif_x86_64_back_ring x86_64;
};
typedef union usbif_back_rings usbif_back_rings_t;

enum usbif_protocol {
	USBIF_PROTOCOL_NATIVE = 1,
	USBIF_PROTOCOL_X86_32 = 2,
	USBIF_PROTOCOL_X86_64 = 3,
};

static void inline usbif_get_x86_32_req(usbif_request_t *dst, usbif_x86_32_request_t *src)
{
	memcpy(dst, src, sizeof(usbif_request_t));
}

static void inline usbif_get_x86_64_req(usbif_request_t *dst, usbif_x86_64_request_t *src)
{
	memcpy(dst, src, sizeof(usbif_request_t));
}

#endif /* __XEN_VUSB_H__ */
