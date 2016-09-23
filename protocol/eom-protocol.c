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

#include <stdlib.h>
#include <stdint.h>
#include "wayland-util.h"

extern const struct wl_interface wl_shell_surface_interface;
extern const struct wl_interface xdg_surface_interface;

static const struct wl_interface *types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	&xdg_surface_interface,
	NULL,
	&wl_shell_surface_interface,
};

static const struct wl_message wl_eom_requests[] = {
	{ "set_attribute", "uu", types + 0 },
	{ "set_xdg_window", "uo", types + 12 },
	{ "set_shell_window", "uo", types + 14 },
	{ "get_output_info", "u", types + 0 },
};

static const struct wl_message wl_eom_events[] = {
	{ "output_count", "u", types + 0 },
	{ "output_info", "uuuuuuuuuuuu", types + 0 },
	{ "output_type", "uuu", types + 0 },
	{ "output_mode", "uu", types + 0 },
	{ "output_attribute", "uuuu", types + 0 },
	{ "output_set_window", "uu", types + 0 },
};

WL_EXPORT const struct wl_interface wl_eom_interface = {
	"wl_eom", 1,
	4, wl_eom_requests,
	6, wl_eom_events,
};

