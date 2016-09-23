/*
 * Copyright 2016 Samsung Electronics co., Ltd. All Rights Reserved.
 *
 * Permission to use, copy, modify, distribute, and sell this
 * software and its documentation for any purpose is hereby granted
 * without fee, provided that\n the above copyright notice appear in
 * all copies and that both that copyright notice and this permission
 * notice appear in supporting documentation, and that the name of
 * the copyright holders not be used in advertising or publicity
 * pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no
 * representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied
 * warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
 * ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF
 * THIS SOFTWARE.
 */

#ifndef WL_EOM_CLIENT_PROTOCOL_H
#define WL_EOM_CLIENT_PROTOCOL_H

#ifdef  __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include "wayland-client.h"

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
 * wl_eom_type - connector type
 * @WL_EOM_TYPE_NONE: none
 * @WL_EOM_TYPE_VGA: VGA output connector type
 * @WL_EOM_TYPE_DVII: DVI-I output connector type
 * @WL_EOM_TYPE_DVID: DVI-D output connector type
 * @WL_EOM_TYPE_DVIA: DVI-A output connector type
 * @WL_EOM_TYPE_COMPOSITE: Composite output connector type
 * @WL_EOM_TYPE_SVIDEO: S-Video output connector type
 * @WL_EOM_TYPE_LVDS: LVDS output connector type
 * @WL_EOM_TYPE_COMPONENT: Component output connector type
 * @WL_EOM_TYPE_9PINDIN: 9 pin DIN output connector type
 * @WL_EOM_TYPE_DISPLAYPORT: DisplayPort output connector type
 * @WL_EOM_TYPE_HDMIA: HDMI type A output connector type
 * @WL_EOM_TYPE_HDMIB: HDMI type B output connector type
 * @WL_EOM_TYPE_TV: TV output connector type
 * @WL_EOM_TYPE_EDP: eDP output connector type
 * @WL_EOM_TYPE_VIRTUAL: Virtual output connector type
 * @WL_EOM_TYPE_DSI: DSI output connector type
 *
 * Define several connectors type of the external outputs
 */
enum wl_eom_type {
	WL_EOM_TYPE_NONE = 0,
	WL_EOM_TYPE_VGA = 1,
	WL_EOM_TYPE_DVII = 2,
	WL_EOM_TYPE_DVID = 3,
	WL_EOM_TYPE_DVIA = 4,
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
 * The status of external output is connected or not.
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
 * There are two modes for external output. Mirror mode is showing main
 * display screen to external output. Presentation mode is showing app's
 * specific buffer contents to external output.
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
 * @WL_EOM_ATTRIBUTE_EXCLUSIVE_SHARE: exclusive share attribute
 * @WL_EOM_ATTRIBUTE_EXCLUSIVE: exclusive attribute
 *
 * Application can use external output by attribute.
 *
 * If application succeed to set attribute and set external output window,
 * the external output's mode will be changed to Presentation mode.
 *
 * Attribute has priority. If attribute is set to normal, it can be changed
 * by normal, exclusive_share, exclusive. If attribute is set to
 * exclusive_share, it can be changed by exclusive_share, exclusive. If
 * attribute is set to exclusive, it cannot be changed by other
 * application. If application which set attribute is quit or set to none,
 * the mode will be changed to Mirror if connected.
 */
enum wl_eom_attribute {
	WL_EOM_ATTRIBUTE_NONE = 0,
	WL_EOM_ATTRIBUTE_NORMAL = 1,
	WL_EOM_ATTRIBUTE_EXCLUSIVE_SHARE = 2,
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
 * It means the state of attribute. The applicatoin which set attribute
 * successful can get state.
 *
 * Active means the external window is set to external output succefully.
 * So application can use that window. Inactive means cannot use external
 * output, because of dissconnecting or some other reasons. But if
 * connected again, the application can use external output. Lost means the
 * application is lost it's right to external output by other application's
 * attribute set.
 */
enum wl_eom_attribute_state {
	WL_EOM_ATTRIBUTE_STATE_NONE = 0,
	WL_EOM_ATTRIBUTE_STATE_ACTIVE = 1,
	WL_EOM_ATTRIBUTE_STATE_INACTIVE = 2,
	WL_EOM_ATTRIBUTE_STATE_LOST = 3,
};
#endif /* WL_EOM_ATTRIBUTE_STATE_ENUM */

/**
 * wl_eom - an interface for external outputs
 * @output_count: external output count
 * @output_info: 
 * @output_type: output type and connection info
 * @output_mode: output mode info
 * @output_attribute: output attribute info
 * @output_set_window: reslut of set_window
 *
 * An interface to get information of external outputs and to use
 * external outputs.
 */
struct wl_eom_listener {
	/**
	 * output_count - external output count
	 * @count: (none)
	 *
	 * Get the number of external output devices that are supported
	 * by this device.
	 */
	void (*output_count)(void *data,
			     struct wl_eom *wl_eom,
			     uint32_t count);
	/**
	 * output_info - 
	 * @output_id: (none)
	 * @type: (none)
	 * @mode: (none)
	 * @w: (none)
	 * @h: (none)
	 * @w_mm: (none)
	 * @h_mm: (none)
	 * @connection: (none)
	 * @skip: (none)
	 * @attribute: (none)
	 * @attribute_state: (none)
	 * @error: (none)
	 *
	 * Send information of specific external output to client.
	 *
	 * Output_id is numbering of external outputs. It is fixed when
	 * booting time. The type, mode attribute, attribute_state is
	 * mentioned above. The w and h is the resolution of external
	 * output. The w_mm and h_mm is the physical size of external
	 * output. The unit is mm.
	 */
	void (*output_info)(void *data,
			    struct wl_eom *wl_eom,
			    uint32_t output_id,
			    uint32_t type,
			    uint32_t mode,
			    uint32_t w,
			    uint32_t h,
			    uint32_t w_mm,
			    uint32_t h_mm,
			    uint32_t connection,
			    uint32_t skip,
			    uint32_t attribute,
			    uint32_t attribute_state,
			    uint32_t error);
	/**
	 * output_type - output type and connection info
	 * @output_id: (none)
	 * @type: (none)
	 * @status: (none)
	 *
	 * Send information of output type and connection.
	 */
	void (*output_type)(void *data,
			    struct wl_eom *wl_eom,
			    uint32_t output_id,
			    uint32_t type,
			    uint32_t status);
	/**
	 * output_mode - output mode info
	 * @output_id: (none)
	 * @mode: (none)
	 *
	 * Send information of output mode.
	 */
	void (*output_mode)(void *data,
			    struct wl_eom *wl_eom,
			    uint32_t output_id,
			    uint32_t mode);
	/**
	 * output_attribute - output attribute info
	 * @output_id: (none)
	 * @attribute: (none)
	 * @attribute_state: (none)
	 * @error: (none)
	 *
	 * Send information of output attribute and attribute state.
	 */
	void (*output_attribute)(void *data,
				 struct wl_eom *wl_eom,
				 uint32_t output_id,
				 uint32_t attribute,
				 uint32_t attribute_state,
				 uint32_t error);
	/**
	 * output_set_window - reslut of set_window
	 * @output_id: (none)
	 * @error: (none)
	 *
	 * Send the result of set_window to client.
	 */
	void (*output_set_window)(void *data,
				  struct wl_eom *wl_eom,
				  uint32_t output_id,
				  uint32_t error);
};

static inline int
wl_eom_add_listener(struct wl_eom *wl_eom,
		    const struct wl_eom_listener *listener, void *data)
{
	return wl_proxy_add_listener((struct wl_proxy *) wl_eom,
				     (void (**)(void)) listener, data);
}

#define WL_EOM_SET_ATTRIBUTE	0
#define WL_EOM_SET_XDG_WINDOW	1
#define WL_EOM_SET_SHELL_WINDOW	2
#define WL_EOM_GET_OUTPUT_INFO	3

#define WL_EOM_SET_ATTRIBUTE_SINCE_VERSION	1
#define WL_EOM_SET_XDG_WINDOW_SINCE_VERSION	1
#define WL_EOM_SET_SHELL_WINDOW_SINCE_VERSION	1
#define WL_EOM_GET_OUTPUT_INFO_SINCE_VERSION	1

static inline void
wl_eom_set_user_data(struct wl_eom *wl_eom, void *user_data)
{
	wl_proxy_set_user_data((struct wl_proxy *) wl_eom, user_data);
}

static inline void *
wl_eom_get_user_data(struct wl_eom *wl_eom)
{
	return wl_proxy_get_user_data((struct wl_proxy *) wl_eom);
}

static inline uint32_t
wl_eom_get_version(struct wl_eom *wl_eom)
{
	return wl_proxy_get_version((struct wl_proxy *) wl_eom);
}

static inline void
wl_eom_destroy(struct wl_eom *wl_eom)
{
	wl_proxy_destroy((struct wl_proxy *) wl_eom);
}

static inline void
wl_eom_set_attribute(struct wl_eom *wl_eom, uint32_t output_id, uint32_t attribute)
{
	wl_proxy_marshal((struct wl_proxy *) wl_eom,
			 WL_EOM_SET_ATTRIBUTE, output_id, attribute);
}

static inline void
wl_eom_set_xdg_window(struct wl_eom *wl_eom, uint32_t output_id, struct xdg_surface *surface)
{
	wl_proxy_marshal((struct wl_proxy *) wl_eom,
			 WL_EOM_SET_XDG_WINDOW, output_id, surface);
}

static inline void
wl_eom_set_shell_window(struct wl_eom *wl_eom, uint32_t output_id, struct wl_shell_surface *surface)
{
	wl_proxy_marshal((struct wl_proxy *) wl_eom,
			 WL_EOM_SET_SHELL_WINDOW, output_id, surface);
}

static inline void
wl_eom_get_output_info(struct wl_eom *wl_eom, uint32_t output_id)
{
	wl_proxy_marshal((struct wl_proxy *) wl_eom,
			 WL_EOM_GET_OUTPUT_INFO, output_id);
}

#ifdef  __cplusplus
}
#endif

#endif
