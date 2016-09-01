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

