#ifndef WL_EOM_SERVER_PROTOCOL_H
#define WL_EOM_SERVER_PROTOCOL_H

#ifdef  __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include "wayland-server.h"

struct wl_client;
struct wl_resource;

struct wl_eom;
struct wl_shell_surface;
struct xdg_surface;

extern const struct wl_interface wl_eom_interface;

#ifndef WL_EOM_ERROR_ENUM
#define WL_EOM_ERROR_ENUM
enum wl_eom_error {
	WL_EOM_ERROR_NONE = 0,
	WL_EOM_ERROR_NO_OUTPUT = 1,
	WL_EOM_ERROR_NO_ATTRIBUTE = 2,
	WL_EOM_ERROR_OUTPUT_OCCUPIED = 3,
};
#endif /* WL_EOM_ERROR_ENUM */

#ifndef WL_EOM_TYPE_ENUM
#define WL_EOM_TYPE_ENUM
/**
 * wl_eom_type - connector type of the external output
 * @WL_EOM_TYPE_NONE: none
 * @WL_EOM_TYPE_VGA: VGA output connector type
 * @WL_EOM_TYPE_DIVI: VGA output connector type
 * @WL_EOM_TYPE_DIVD: VGA output connector type
 * @WL_EOM_TYPE_DIVA: VGA output connector type
 * @WL_EOM_TYPE_COMPOSITE: VGA output connector type
 * @WL_EOM_TYPE_SVIDEO: VGA output connector type
 * @WL_EOM_TYPE_LVDS: VGA output connector type
 * @WL_EOM_TYPE_COMPONENT: VGA output connector type
 * @WL_EOM_TYPE_9PINDIN: VGA output connector type
 * @WL_EOM_TYPE_DISPLAYPORT: VGA output connector type
 * @WL_EOM_TYPE_HDMIA: VGA output connector type
 * @WL_EOM_TYPE_HDMIB: VGA output connector type
 * @WL_EOM_TYPE_TV: VGA output connector type
 * @WL_EOM_TYPE_EDP: VGA output connector type
 * @WL_EOM_TYPE_VIRTUAL: VGA output connector type
 * @WL_EOM_TYPE_DSI: VGA output connector type
 *
 * ***** TODO ******
 */
enum wl_eom_type {
	WL_EOM_TYPE_NONE = 0,
	WL_EOM_TYPE_VGA = 1,
	WL_EOM_TYPE_DIVI = 2,
	WL_EOM_TYPE_DIVD = 3,
	WL_EOM_TYPE_DIVA = 4,
	WL_EOM_TYPE_COMPOSITE = 5,
	WL_EOM_TYPE_SVIDEO = 6,
	WL_EOM_TYPE_LVDS = 7,
	WL_EOM_TYPE_COMPONENT = 8,
	WL_EOM_TYPE_9PINDIN = 9,
	WL_EOM_TYPE_DISPLAYPORT = 10,
	WL_EOM_TYPE_HDMIA = 11,
	WL_EOM_TYPE_HDMIB = 12,
	WL_EOM_TYPE_TV = 13,
	WL_EOM_TYPE_EDP = 14,
	WL_EOM_TYPE_VIRTUAL = 15,
	WL_EOM_TYPE_DSI = 16,
};
#endif /* WL_EOM_TYPE_ENUM */

#ifndef WL_EOM_STATUS_ENUM
#define WL_EOM_STATUS_ENUM
/**
 * wl_eom_status - connection status of the external output
 * @WL_EOM_STATUS_NONE: none
 * @WL_EOM_STATUS_CONNECTION: output connected
 * @WL_EOM_STATUS_DISCONNECTION: output disconnected
 *
 * ***** TODO ******
 */
enum wl_eom_status {
	WL_EOM_STATUS_NONE = 0,
	WL_EOM_STATUS_CONNECTION = 1,
	WL_EOM_STATUS_DISCONNECTION = 2,
};
#endif /* WL_EOM_STATUS_ENUM */

#ifndef WL_EOM_MODE_ENUM
#define WL_EOM_MODE_ENUM
/**
 * wl_eom_mode - mode of the external output
 * @WL_EOM_MODE_NONE: none
 * @WL_EOM_MODE_MIRROR: mirror mode
 * @WL_EOM_MODE_PRESENTATION: presentation mode
 *
 * ***** TODO ******
 */
enum wl_eom_mode {
	WL_EOM_MODE_NONE = 0,
	WL_EOM_MODE_MIRROR = 1,
	WL_EOM_MODE_PRESENTATION = 2,
};
#endif /* WL_EOM_MODE_ENUM */

#ifndef WL_EOM_ATTRIBUTE_ENUM
#define WL_EOM_ATTRIBUTE_ENUM
/**
 * wl_eom_attribute - attribute of the external output
 * @WL_EOM_ATTRIBUTE_NONE: none
 * @WL_EOM_ATTRIBUTE_NORMAL: nomal attribute
 * @WL_EOM_ATTRIBUTE_EXCLUSIVE_SHARED: exclusive shared attribute
 * @WL_EOM_ATTRIBUTE_EXCLUSIVE: exclusive attribute
 *
 * ***** TODO ******
 */
enum wl_eom_attribute {
	WL_EOM_ATTRIBUTE_NONE = 0,
	WL_EOM_ATTRIBUTE_NORMAL = 1,
	WL_EOM_ATTRIBUTE_EXCLUSIVE_SHARED = 2,
	WL_EOM_ATTRIBUTE_EXCLUSIVE = 3,
};
#endif /* WL_EOM_ATTRIBUTE_ENUM */

#ifndef WL_EOM_ATTRIBUTE_STATE_ENUM
#define WL_EOM_ATTRIBUTE_STATE_ENUM
/**
 * wl_eom_attribute_state - state of the external output attribute
 * @WL_EOM_ATTRIBUTE_STATE_NONE: none
 * @WL_EOM_ATTRIBUTE_STATE_ACTIVE: attribute is active on the output
 * @WL_EOM_ATTRIBUTE_STATE_INACTIVE: attribute is inactive on the output
 * @WL_EOM_ATTRIBUTE_STATE_LOST: the connection of output is lost
 *
 * ***** TODO ******
 */
enum wl_eom_attribute_state {
	WL_EOM_ATTRIBUTE_STATE_NONE = 0,
	WL_EOM_ATTRIBUTE_STATE_ACTIVE = 1,
	WL_EOM_ATTRIBUTE_STATE_INACTIVE = 2,
	WL_EOM_ATTRIBUTE_STATE_LOST = 3,
};
#endif /* WL_EOM_ATTRIBUTE_STATE_ENUM */

/**
 * wl_eom - an interface to get the information of the external outputs
 * @set_attribute: (none)
 * @set_xdg_window: (none)
 * @set_shell_window: (none)
 * @get_output_info: (none)
 *
 * ***** TODO ******
 */
struct wl_eom_interface {
	/**
	 * set_attribute - (none)
	 * @output_id: (none)
	 * @attribute: (none)
	 */
	void (*set_attribute)(struct wl_client *client,
			      struct wl_resource *resource,
			      uint32_t output_id,
			      uint32_t attribute);
	/**
	 * set_xdg_window - (none)
	 * @output_id: (none)
	 * @surface: (none)
	 */
	void (*set_xdg_window)(struct wl_client *client,
			       struct wl_resource *resource,
			       uint32_t output_id,
			       struct wl_resource *surface);
	/**
	 * set_shell_window - (none)
	 * @output_id: (none)
	 * @surface: (none)
	 */
	void (*set_shell_window)(struct wl_client *client,
				 struct wl_resource *resource,
				 uint32_t output_id,
				 struct wl_resource *surface);
	/**
	 * get_output_info - (none)
	 * @output_id: (none)
	 */
	void (*get_output_info)(struct wl_client *client,
				struct wl_resource *resource,
				uint32_t output_id);
};

#define WL_EOM_OUTPUT_COUNT	0
#define WL_EOM_OUTPUT_INFO	1
#define WL_EOM_OUTPUT_TYPE	2
#define WL_EOM_OUTPUT_MODE	3
#define WL_EOM_OUTPUT_ATTRIBUTE	4
#define WL_EOM_OUTPUT_SET_WINDOW	5

#define WL_EOM_OUTPUT_COUNT_SINCE_VERSION	1
#define WL_EOM_OUTPUT_INFO_SINCE_VERSION	1
#define WL_EOM_OUTPUT_TYPE_SINCE_VERSION	1
#define WL_EOM_OUTPUT_MODE_SINCE_VERSION	1
#define WL_EOM_OUTPUT_ATTRIBUTE_SINCE_VERSION	1
#define WL_EOM_OUTPUT_SET_WINDOW_SINCE_VERSION	1

static inline void
wl_eom_send_output_count(struct wl_resource *resource_, uint32_t count)
{
	wl_resource_post_event(resource_, WL_EOM_OUTPUT_COUNT, count);
}

static inline void
wl_eom_send_output_info(struct wl_resource *resource_, uint32_t output_id, uint32_t type, uint32_t mode, uint32_t w, uint32_t h, uint32_t w_mm, uint32_t h_mm, uint32_t connection, uint32_t skip, uint32_t attribute, uint32_t attribute_state, uint32_t error)
{
	wl_resource_post_event(resource_, WL_EOM_OUTPUT_INFO, output_id, type, mode, w, h, w_mm, h_mm, connection, skip, attribute, attribute_state, error);
}

static inline void
wl_eom_send_output_type(struct wl_resource *resource_, uint32_t output_id, uint32_t type, uint32_t status)
{
	wl_resource_post_event(resource_, WL_EOM_OUTPUT_TYPE, output_id, type, status);
}

static inline void
wl_eom_send_output_mode(struct wl_resource *resource_, uint32_t output_id, uint32_t mode)
{
	wl_resource_post_event(resource_, WL_EOM_OUTPUT_MODE, output_id, mode);
}

static inline void
wl_eom_send_output_attribute(struct wl_resource *resource_, uint32_t output_id, uint32_t attribute, uint32_t attribute_state, uint32_t error)
{
	wl_resource_post_event(resource_, WL_EOM_OUTPUT_ATTRIBUTE, output_id, attribute, attribute_state, error);
}

static inline void
wl_eom_send_output_set_window(struct wl_resource *resource_, uint32_t output_id, uint32_t error)
{
	wl_resource_post_event(resource_, WL_EOM_OUTPUT_SET_WINDOW, output_id, error);
}

#ifdef  __cplusplus
}
#endif

#endif
