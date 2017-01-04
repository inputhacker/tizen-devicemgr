#ifndef __E_DEVICEMGR_VIEWPORT_H__
#define __E_DEVICEMGR_VIEWPORT_H__

#define E_COMP_WL
#include "e.h"
#include "e_comp_wl.h"
#include "e_mod_main.h"
#include "e_devicemgr_privates.h"
#include <wayland-server.h>
#include <wayland-tbm-server.h>

int e_devicemgr_viewport_init(void);
void e_devicemgr_viewport_fini(void);

Eina_Bool e_devicemgr_viewport_create(struct wl_resource *resource,
                                      uint32_t id,
                                      struct wl_resource *surface);
Eina_Bool e_devicemgr_viewport_apply(E_Client *ec);
Eina_Bool e_devicemgr_viewport_is_changed(E_Client *ec);

#endif
