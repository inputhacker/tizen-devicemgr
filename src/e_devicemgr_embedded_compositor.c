#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define E_COMP_WL
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <e.h>
#include <tizen-extension-server-protocol.h>
#include <sys/socket.h>

static struct wl_global *e_embedded;

static void
_e_tizen_embedded_compositor_cb_get_socket(struct wl_client *client,
                          struct wl_resource *resource)
{
   int sock_fd=-1;

   sock_fd = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);
   if (sock_fd < 0)
     {
        ERR("Could not create socket: %m");
        return;
     }

   tizen_embedded_compositor_send_socket(resource, sock_fd);
   close(sock_fd);
}

static void
_e_tizen_embedded_compositor_cb_destroy(struct wl_client *client,
                          struct wl_resource *resource)
{
   wl_resource_destroy(resource);
}

static const struct tizen_embedded_compositor_interface _e_tizen_embedded_compositor_interface =
{
   _e_tizen_embedded_compositor_cb_get_socket,
   _e_tizen_embedded_compositor_cb_destroy
};

static void
_e_tizen_embedded_compositor_cb_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
   struct wl_resource *res;

   if (!(res = wl_resource_create(client, &tizen_embedded_compositor_interface, MIN(version, 1), id)))
     {
        ERR("Could not create tizen_video_interface resource: %m");
        wl_client_post_no_memory(client);
        return;
     }

   wl_resource_set_implementation(res, &_e_tizen_embedded_compositor_interface, NULL, NULL);
}

int
e_devicemgr_embedded_compositor_init(void)
{
   if (!e_comp_wl) return 0;
   if (!e_comp_wl->wl.disp) return 0;

   /* try to add tizen_embedded_compositor to wayland globals */
   if (!(e_embedded = wl_global_create(e_comp_wl->wl.disp, &tizen_embedded_compositor_interface, 1,
                         NULL, _e_tizen_embedded_compositor_cb_bind)))
     {
        ERR("Could not add tizen_embedded_compositor to wayland globals");
        return 0;
     }

   return 1;
}

void
e_devicemgr_embedded_compositor_fini(void)
{
   if (e_embedded) wl_global_destroy(e_embedded);
}
