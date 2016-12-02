#define E_COMP_WL
#include "e.h"
#include <wayland-server.h>
#include <Ecore_Wayland.h>
#include <Ecore_Drm.h>
#include <screenshooter-server-protocol.h>
#include <tizen-extension-server-protocol.h>
#include <tdm.h>
#include "e_devicemgr_screenshooter.h"
#include "e_devicemgr_video.h"
#include "e_devicemgr_buffer.h"
#include "e_devicemgr_dpms.h"
#ifdef ENABLE_CYNARA
#include <cynara-session.h>
#include <cynara-client.h>
#include <cynara-creds-socket.h>
#endif

#define DUMP_FPS     30

typedef struct _E_Mirror
{
   struct wl_resource *resource;
   struct wl_resource *shooter;
   struct wl_resource *output;

   Eina_Bool started;
   enum tizen_screenmirror_stretch stretch;

   Eina_List *buffer_queue;
   E_Comp_Wl_Output *wl_output;
   Ecore_Drm_Output *drm_output;
   Ecore_Drm_Device *drm_device;

   tdm_display *tdm_dpy;
   tdm_output *tdm_output;
   tdm_layer *tdm_primary_layer;
   tdm_capture *capture;

   /* vblank info */
   int per_vblank;
   Eina_Bool wait_vblank;

   /* timer info when dpms off */
   Ecore_Timer *timer;

   /* converter info */
   tdm_pp *pp;
   Eina_List *ui_buffer_list;
   Eina_List *buffer_clear_check;

   struct wl_listener client_destroy_listener;

#ifdef ENABLE_CYNARA
   cynara *p_cynara;
   Eina_Bool cynara_initialized;
#endif
} E_Mirror;

typedef struct _E_Mirror_Buffer
{
   E_Devmgr_Buf *mbuf;

   E_Mirror *mirror;

   Eina_Bool in_use;
   Eina_Bool dirty;

   /* in case of shm buffer */
   struct wl_listener destroy_listener;
} E_Mirror_Buffer;

static uint mirror_format_table[] =
{
   TBM_FORMAT_ARGB8888,
   TBM_FORMAT_XRGB8888,
   TBM_FORMAT_NV12,
   TBM_FORMAT_NV21,
};

#define NUM_MIRROR_FORMAT   (sizeof(mirror_format_table) / sizeof(mirror_format_table[0]))

static E_Mirror *keep_mirror;

static void _e_tz_screenmirror_destroy(E_Mirror *mirror);
static void _e_tz_screenmirror_buffer_dequeue(E_Mirror_Buffer *buffer);
static void _e_tz_screenmirror_buffer_cb_free(E_Devmgr_Buf *mbuf, void *data);
static void _e_tz_screenmirror_vblank_handler(void *data);

#ifdef ENABLE_CYNARA
static void _e_screenmirror_cynara_log(const char *func_name, int err);

#define PRIVILEGE_SCREENSHOT "http://tizen.org/privilege/screenshot"
#define SMACK_LABEL_LEN 255
#define PATH_MAX_LEN 64
#define CYNARA_BUFSIZE 128
#define E_SCREENMIRROR_CYNARA_ERROR_CHECK_GOTO(func_name, ret, label) \
   do \
     { \
        if (EINA_UNLIKELY(ret != CYNARA_API_SUCCESS)) \
          { \
             _e_screenmirror_cynara_log(func_name, ret); \
             goto label; \
          } \
     } \
   while (0)

static void
_e_screenmirror_cynara_log(const char *func_name, int err)
{
   char buf[CYNARA_BUFSIZE] = "\0";
   int ret;

   ret = cynara_strerror(err, buf, CYNARA_BUFSIZE);
   if (ret != CYNARA_API_SUCCESS)
     {
        DBG("Failed to cynara_strerror: %d (error log about %s: %d)\n", ret, func_name, err);
        return;
     }
   DBG("%s is failed: %s\n", func_name, buf);
}

static Eina_Bool
_e_screenmirror_su_check(struct wl_client *client)
{
   uid_t uid;

   wl_client_get_credentials(client, NULL, &uid, NULL);

   if (uid == 0)
     {
        DBG("pass privilege check if super user");
        return EINA_TRUE;
     }

   return EINA_FALSE;
}

static Eina_Bool
_e_screenmirror_privilege(struct wl_client *client, cynara *p_cynara, int socket_fd, const char *rule)
{
   int ret, pid;
   char *clientSmack = NULL, *uid = NULL, *client_session = NULL;
   Eina_Bool res = EINA_FALSE;

   ret = cynara_creds_socket_get_user(socket_fd, USER_METHOD_UID, &uid);
   E_SCREENMIRROR_CYNARA_ERROR_CHECK_GOTO("cynara_creds_socket_get_user", ret, finish);

   ret = cynara_creds_socket_get_pid(socket_fd, &pid);
   E_SCREENMIRROR_CYNARA_ERROR_CHECK_GOTO("cynara_creds_socket_get_pid", ret, finish);

   client_session = cynara_session_from_pid(pid);

   ret = cynara_creds_socket_get_client(socket_fd, CLIENT_METHOD_SMACK, &clientSmack);
   E_SCREENMIRROR_CYNARA_ERROR_CHECK_GOTO("cynara_creds_socket_get_client", ret, finish);

   ret = cynara_check(p_cynara, clientSmack, client_session, uid, rule);

   if (ret == CYNARA_API_ACCESS_ALLOWED)
     res = EINA_TRUE;

finish:
   E_FREE(client_session);
   E_FREE(clientSmack);
   E_FREE(uid);

   return res;
}
#endif

static Eina_Bool
_e_screenmirror_privilege_check(struct wl_client *client, E_Mirror *mirror, int socket_fd, const char *rule)
{
#ifdef ENABLE_CYNARA
   Eina_Bool res = EINA_FALSE;

   if (mirror->p_cynara == NULL && !mirror->cynara_initialized) return EINA_TRUE;

   if (_e_screenmirror_su_check(client) == EINA_TRUE) return EINA_TRUE;

   res = _e_screenmirror_privilege(client, mirror->p_cynara, socket_fd, rule);

   return res;
#else
   return EINA_TRUE;
#endif
}

static Eina_Bool
_e_screenmirror_privilege_check_with_cynara_init(struct wl_client *client, int socket_fd, const char *rule)
{
#ifdef ENABLE_CYNARA
   int ret;
   Eina_Bool res = EINA_FALSE;
   cynara *p_cynara;

   if (_e_screenmirror_su_check(client) == EINA_TRUE) return EINA_TRUE;

   ret = cynara_initialize(&p_cynara, NULL);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(ret == CYNARA_API_SUCCESS, EINA_FALSE);

   res = _e_screenmirror_privilege(client, p_cynara, socket_fd, rule);

   if (p_cynara) cynara_finish(p_cynara);

   return res;
#else
   return EINA_TRUE;
#endif
}

static void
_e_tz_screenmirror_center_rect (int src_w, int src_h, int dst_w, int dst_h, Eina_Rectangle *fit)
{
   float rw, rh;

   if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 || !fit)
     return;

   rw = (float)src_w / dst_w;
   rh = (float)src_h / dst_h;

   if (rw > rh)
     {
        fit->w = dst_w;
        fit->h = src_h / rw;
        fit->x = 0;
        fit->y = (dst_h - fit->h) / 2;
     }
   else if (rw < rh)
     {
        fit->w = src_w / rh;
        fit->h = dst_h;
        fit->x = (dst_w - fit->w) / 2;
        fit->y = 0;
     }
   else
     {
        fit->w = dst_w;
        fit->h = dst_h;
        fit->x = 0;
        fit->y = 0;
     }

   if (fit->x % 2)
     fit->x = fit->x - 1;
}

void
_e_tz_screenmirror_rect_scale (int src_w, int src_h, int dst_w, int dst_h, Eina_Rectangle *scale)
{
   float ratio;
   Eina_Rectangle center;

   if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 || !scale)
     return;

   _e_tz_screenmirror_center_rect (src_w, src_h, dst_w, dst_h, &center);

   ratio = (float)center.w / src_w;

   scale->x = scale->x * ratio + center.x;
   scale->y = scale->y * ratio + center.y;
   scale->w = scale->w * ratio;
   scale->h = scale->h * ratio;
}

static Eina_Bool
_e_tz_screenmirror_cb_timeout(void *data)
{
   E_Mirror *mirror = data;

   EINA_SAFETY_ON_NULL_RETURN_VAL(mirror, ECORE_CALLBACK_RENEW);

   _e_tz_screenmirror_vblank_handler((void*)mirror);

   return ECORE_CALLBACK_RENEW;
}

static Eina_Bool
_e_tz_screenmirror_watch_vblank(E_Mirror *mirror)
{
    /* If not DPMS_ON, we call vblank handler directly to dump screen */
   if (e_devicemgr_dpms_get(mirror->drm_output))
     {
        if (!mirror->timer)
          mirror->timer = ecore_timer_add((double)1/DUMP_FPS,
                                          _e_tz_screenmirror_cb_timeout, mirror);
        EINA_SAFETY_ON_NULL_RETURN_VAL(mirror->timer, EINA_FALSE);

        return EINA_TRUE;
     }
   else if (mirror->timer)
     {
        ecore_timer_del(mirror->timer);
        mirror->timer = NULL;
     }

   if (mirror->wait_vblank)
     return EINA_TRUE;

   if (!ecore_drm_output_wait_vblank(mirror->drm_output, mirror->per_vblank,
                                     _e_tz_screenmirror_vblank_handler, mirror))
     {
         ERR("failed: ecore_drm_output_wait_vblank");
         return EINA_FALSE;
     }

   mirror->wait_vblank = EINA_TRUE;

   return EINA_TRUE;
}

static Eina_Bool
_e_tz_screenmirror_buffer_check(struct wl_resource *resource)
{
   if (wl_shm_buffer_get(resource) ||
       wayland_tbm_server_get_surface(e_comp->wl_comp_data->tbm.server, resource))
     return EINA_TRUE;

   ERR("unrecognized buffer");

   return EINA_FALSE;
}

static void
_e_tz_screenmirror_ui_buffer_cb_free(E_Devmgr_Buf *mbuf, void *data)
{
   E_Mirror *mirror = (E_Mirror*)data;

   EINA_SAFETY_ON_NULL_RETURN(mirror);
   mirror->ui_buffer_list = eina_list_remove(mirror->ui_buffer_list, mbuf);
}

static E_Devmgr_Buf*
_e_tz_screenmirror_ui_buffer_get(E_Mirror *mirror)
{
   E_Devmgr_Buf *mbuf;
   tbm_surface_h buffer;
   Eina_List *l;
   tdm_error err = TDM_ERROR_NONE;

   buffer = tdm_layer_get_displaying_buffer(mirror->tdm_primary_layer, &err);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(err == TDM_ERROR_NONE, NULL);

   EINA_LIST_FOREACH(mirror->ui_buffer_list, l, mbuf)
     if (mbuf->tbm_surface == buffer)
       return mbuf;

   mbuf = e_devmgr_buffer_create_tbm(buffer);
   EINA_SAFETY_ON_NULL_RETURN_VAL(mbuf, NULL);

   e_devmgr_buffer_free_func_add(mbuf, _e_tz_screenmirror_ui_buffer_cb_free, mirror);
   mirror->ui_buffer_list = eina_list_append(mirror->ui_buffer_list, mbuf);

   return mbuf;
}

static void
_e_tz_screenmirror_pp_destroy(E_Mirror *mirror)
{
   if (!mirror->pp)
     return;

   tdm_pp_destroy(mirror->pp);
   mirror->pp = NULL;
}

static Eina_Bool
_e_tz_screenmirror_pp_create(E_Mirror *mirror, E_Devmgr_Buf *src, E_Devmgr_Buf *dst)
{
   tdm_info_pp info;

   if (mirror->pp)
     return EINA_TRUE;

   mirror->pp = tdm_display_create_pp(e_devmgr_dpy->tdm, NULL);
   EINA_SAFETY_ON_NULL_RETURN_VAL(mirror->pp, EINA_FALSE);

   CLEAR(info);
   info.src_config.size.h = src->width_from_pitch;
   info.src_config.size.v = src->height;
   info.src_config.pos.w = src->width;
   info.src_config.pos.h = src->height;
   info.src_config.format = src->tbmfmt;
   info.dst_config.size.h = dst->width;
   info.dst_config.size.v = dst->height;
   info.dst_config.format = dst->tbmfmt;

   if (mirror->stretch == TIZEN_SCREENMIRROR_STRETCH_KEEP_RATIO)
     {
        Eina_Rectangle dst_pos;
        _e_tz_screenmirror_center_rect(src->width, src->height, dst->width, dst->height, &dst_pos);
        info.dst_config.pos.x = dst_pos.x;
        info.dst_config.pos.y = dst_pos.y;
        info.dst_config.pos.w = dst_pos.w;
        info.dst_config.pos.h = dst_pos.h;
     }
   else
     {
        info.dst_config.pos.w = dst->width;
        info.dst_config.pos.h = dst->height;
     }

   if (tdm_pp_set_info(mirror->pp, &info))
       goto failed;

   return EINA_TRUE;

failed:
   _e_tz_screenmirror_pp_destroy(mirror);
   return EINA_FALSE;
}

static void
_e_tz_screenmirror_shm_dump(E_Mirror_Buffer *buffer)
{
   E_Mirror *mirror = buffer->mirror;
   Eina_Rectangle dst = {0,};
   struct wl_shm_buffer *shm_buffer;
   int32_t stride;
   int32_t width, height;
   uint32_t format;
   void *data;

   shm_buffer = wl_shm_buffer_get(buffer->mbuf->resource);
   EINA_SAFETY_ON_NULL_RETURN(shm_buffer);

   stride = wl_shm_buffer_get_stride(shm_buffer);
   width = wl_shm_buffer_get_width(shm_buffer);
   height = wl_shm_buffer_get_height(shm_buffer);
   format = wl_shm_buffer_get_format(shm_buffer);
   data = wl_shm_buffer_get_data(shm_buffer);

   if (mirror->stretch == TIZEN_SCREENMIRROR_STRETCH_KEEP_RATIO)
     _e_tz_screenmirror_center_rect(mirror->wl_output->w, mirror->wl_output->h, width, height, &dst);
   else
     {
        dst.x = dst.y = 0;
        dst.w = width;
        dst.h = height;
     }

   wl_shm_buffer_begin_access(shm_buffer);
   evas_render_copy(e_comp->evas, data, stride, width, height, format,
                    mirror->wl_output->x, mirror->wl_output->y, mirror->wl_output->w, mirror->wl_output->h,
                    dst.x, dst.y, dst.w, dst.h);
   wl_shm_buffer_end_access(shm_buffer);

   DBG("dump src(%d,%d,%d,%d) dst(%d,%d %d,%d,%d,%d)",
       mirror->wl_output->x, mirror->wl_output->y, mirror->wl_output->w, mirror->wl_output->h,
       width, height, dst.x, dst.y, dst.w, dst.h);
}

static void
_e_tz_screenmirror_ui_buffer_release_cb(tbm_surface_h surface, void *user_data)
{
   E_Devmgr_Buf *mbuf = user_data;
   tdm_buffer_remove_release_handler(surface,
                                     _e_tz_screenmirror_ui_buffer_release_cb, mbuf);
}

static void
_e_tz_screenmirror_buffer_release_cb(tbm_surface_h surface, void *user_data)
{
   E_Mirror_Buffer *buffer = user_data;
   tdm_buffer_remove_release_handler(surface,
                                     _e_tz_screenmirror_buffer_release_cb, buffer);

   _e_tz_screenmirror_buffer_dequeue(buffer);

#if 0
   static int i = 0;
   e_devmgr_buffer_dump(src, "in", i, 0);
   e_devmgr_buffer_dump(dst, "out", i++, 0);
#endif
}

static void
_e_tz_screenmirror_drm_buffer_clear_check(E_Mirror_Buffer *buffer)
{
   E_Mirror *mirror = buffer->mirror;
   E_Devmgr_Buf *mbuf, *dst;
   Eina_List *l;
   uint32_t buf_id;

   dst = buffer->mbuf;
   buf_id = wl_resource_get_id(dst->resource);

   EINA_LIST_FOREACH(mirror->buffer_clear_check, l, mbuf)
     {
        uint32_t id;

        id = wl_resource_get_id(mbuf->resource);
        if (id == buf_id)
          return;
     }

   e_devmgr_buffer_clear(dst);
   mirror->buffer_clear_check = eina_list_append(mirror->buffer_clear_check, dst);
}

static Eina_Bool
_e_tz_screenmirror_drm_dump(E_Mirror_Buffer *buffer)
{
   E_Mirror *mirror = buffer->mirror;
   E_Devmgr_Buf *ui, *dst;

   dst = buffer->mbuf;
   EINA_SAFETY_ON_NULL_RETURN_VAL(dst, EINA_FALSE);

   if (e_devicemgr_dpms_get(mirror->drm_output))
     {
        if (buffer->dirty)
          {
             e_devmgr_buffer_clear(dst);
             buffer->dirty = EINA_FALSE;
             DBG("clear buffer");
#if 0
             static int i = 0;
             e_devmgr_buffer_dump(dst, "clear", i++, 0);
#endif
          }

        /* buffer has been cleared. return false to dequeue buffer immediately */
        return EINA_FALSE;
     }

   ui = _e_tz_screenmirror_ui_buffer_get(mirror);
   EINA_SAFETY_ON_NULL_RETURN_VAL(ui, EINA_FALSE);

   if (!mirror->pp)
     if (!_e_tz_screenmirror_pp_create(mirror, ui, dst))
       return EINA_FALSE;

   _e_tz_screenmirror_drm_buffer_clear_check(buffer);

   if (tdm_pp_attach(mirror->pp, ui->tbm_surface, dst->tbm_surface))
     return EINA_FALSE;

   if (tdm_pp_commit(mirror->pp))
     return EINA_FALSE;

   tdm_buffer_add_release_handler(ui->tbm_surface,
                                  _e_tz_screenmirror_ui_buffer_release_cb, ui);
   tdm_buffer_add_release_handler(dst->tbm_surface,
                                  _e_tz_screenmirror_buffer_release_cb, buffer);

   buffer->in_use = EINA_TRUE;
   buffer->dirty = EINA_TRUE;

   return EINA_TRUE;
}

static void
_e_tz_screenmirror_dump_still(E_Mirror_Buffer *buffer)
{
   E_Mirror *mirror = buffer->mirror;
   E_Devmgr_Buf *video, *ui, *dst;
   Eina_Rectangle vr, ur;
   Eina_List *video_list, *l;
   E_Video *vdo;

   dst = buffer->mbuf;
   EINA_SAFETY_ON_NULL_RETURN(dst);

   e_devmgr_buffer_clear(dst);

   /* get ui buffer */
   ui = _e_tz_screenmirror_ui_buffer_get(mirror);
   EINA_SAFETY_ON_NULL_RETURN(ui);

   video_list = e_devicemgr_video_list_get();
   EINA_LIST_FOREACH(video_list, l, vdo)
     {
        if (mirror->drm_output != e_devicemgr_video_drm_output_get(vdo))
          continue;

        /* get video buffer */
        video = e_devicemgr_video_fb_get(vdo);
        if (!video)
          continue;

        e_devicemgr_video_pos_get(vdo, &vr.x, &vr.y);
        vr.w = video->width;
        vr.h = video->height;

        _e_tz_screenmirror_rect_scale(ui->width, ui->height, dst->width, dst->height, &vr);

        /* dump video buffer */
        e_devmgr_buffer_convert(video, dst,
                                0, 0, video->width, video->height,
                                vr.x, vr.y, vr.w, vr.h,
                                EINA_FALSE, 0, 0, 0);
     }

   ur.x = ur.y = 0;
   ur.w = ui->width;
   ur.h = ui->height;

   _e_tz_screenmirror_center_rect(ui->width, ui->height, dst->width, dst->height, &ur);

   /* dump ui buffer */
   e_devmgr_buffer_convert(ui, dst,
                           0, 0, ui->width, ui->height,
                           ur.x, ur.y, ur.w, ur.h,
                           EINA_TRUE, 0, 0, 0);
}

static void
_e_tz_screenmirror_buffer_free(E_Mirror_Buffer *buffer)
{
   E_Mirror *mirror = buffer->mirror;

   /* then, dequeue and send dequeue event */
   _e_tz_screenmirror_buffer_dequeue(buffer);

   if (buffer->destroy_listener.notify)
     {
        wl_list_remove(&buffer->destroy_listener.link);
        buffer->destroy_listener.notify = NULL;
     }

   if (buffer->mbuf)
     {
        e_devmgr_buffer_free_func_del(buffer->mbuf, _e_tz_screenmirror_buffer_cb_free, buffer);
        e_devmgr_buffer_unref(buffer->mbuf);

        mirror->buffer_clear_check = eina_list_remove(mirror->buffer_clear_check, buffer->mbuf);
     }

   E_FREE(buffer);
}

static Eina_Bool
_e_tz_screenmirror_capture_oneshot_done(void *data)
{
   E_Mirror *mirror = data;

   EINA_SAFETY_ON_NULL_RETURN_VAL(mirror, ECORE_CALLBACK_RENEW);

   _e_tz_screenmirror_destroy(mirror);

   DBG("_e_tz_screenmirror_capture_oneshot_done");

   return ECORE_CALLBACK_RENEW;
}

static void
_e_tz_screenmirror_capture_oneshot_done_handler(tdm_capture *capture, tbm_surface_h buffer, void *user_data)
{
   E_Mirror_Buffer *mirror_buffer = user_data;
   E_Mirror *mirror = mirror_buffer->mirror;

   _e_tz_screenmirror_buffer_free(mirror_buffer);

   mirror->timer = ecore_timer_add((double)1/DUMP_FPS, _e_tz_screenmirror_capture_oneshot_done, mirror);
}

static Eina_Bool
_e_tz_screenmirror_capture_stream_done(void *data)
{
   E_Mirror *mirror = data;

   EINA_SAFETY_ON_NULL_RETURN_VAL(mirror, ECORE_CALLBACK_RENEW);

   if (mirror != keep_mirror)
     return ECORE_CALLBACK_RENEW;

   if (mirror->capture)
     {
        tdm_capture_destroy(mirror->capture);
        mirror->capture = NULL;
     }

   return ECORE_CALLBACK_RENEW;
}

static E_Mirror_Buffer *
_e_tz_screenmirror_mirrorbuf_find(E_Mirror *mirror, tbm_surface_h surf)
{
   Eina_List *l;
   E_Mirror_Buffer *buffer = NULL;

   if (!mirror->buffer_queue)
     return NULL;

   EINA_LIST_FOREACH(mirror->buffer_queue, l, buffer)
     {
        if (buffer && buffer->mbuf && buffer->mbuf->tbm_surface)
          {
             if (buffer->mbuf->tbm_surface == surf)
               return buffer;
          }
     }

   return NULL;
}

static void
_e_tz_screenmirror_capture_stream_done_handler(tdm_capture *capture, tbm_surface_h surf, void *user_data)
{
   E_Mirror *mirror = user_data;
   E_Mirror_Buffer *buffer = NULL;

   if (mirror != keep_mirror)
     return;

   buffer = _e_tz_screenmirror_mirrorbuf_find(mirror, surf);
   if (buffer == NULL)
     {
        ERR("_e_tz_screenmirror_capture_stream_done_handler: find mirror buffer failed");
        return;
     }

   _e_tz_screenmirror_buffer_dequeue(buffer);

   if (mirror->started == EINA_FALSE)
     {
        if (eina_list_count(mirror->buffer_queue) == 0)
          mirror->timer = ecore_timer_add((double)1/DUMP_FPS, _e_tz_screenmirror_capture_stream_done, mirror);
     }
}

static Eina_Bool
_e_tz_screenmirror_position_get(E_Mirror *mirror, E_Devmgr_Buf *mbuf, Eina_Rectangle *fit)
{
   E_Devmgr_Buf *ui = NULL;

   ui = _e_tz_screenmirror_ui_buffer_get(mirror);
   if (ui == NULL)
     {
        ERR("_e_tz_screenmirror_position_get: get ui buf failed");
        return EINA_FALSE;
     }
   _e_tz_screenmirror_center_rect(ui->width, ui->height, mbuf->width, mbuf->height, fit);

   return EINA_TRUE;
}

static Eina_Bool
_e_tz_screenmirror_tdm_capture_handle_set(E_Mirror_Buffer *buffer, tdm_capture *capture, tdm_capture_capability cap)
{
   E_Mirror *mirror = buffer->mirror;
   tdm_error err = TDM_ERROR_NONE;

   if (cap == TDM_CAPTURE_CAPABILITY_ONESHOT)
     err = tdm_capture_set_done_handler(capture, _e_tz_screenmirror_capture_oneshot_done_handler, buffer);
   else
     err = tdm_capture_set_done_handler(capture, _e_tz_screenmirror_capture_stream_done_handler, mirror);
   if (err != TDM_ERROR_NONE)
     {
        ERR("_e_tz_screenmirror_tdm_capture_support: tdm_capture set_handler failed");
        return EINA_FALSE;
     }

   return EINA_TRUE;
}

static Eina_Bool
_e_tz_screenmirror_tdm_capture_support(E_Mirror_Buffer *buffer, tdm_capture_capability cap)
{
   E_Mirror *mirror = buffer->mirror;
   tdm_error err = TDM_ERROR_NONE;
   tdm_capture *capture = NULL;
   tdm_info_capture capture_info;
   tdm_capture_capability capabilities;
   Eina_Rectangle dst_pos;

   err = tdm_display_get_capture_capabilities(mirror->tdm_dpy, &capabilities);
   EINA_SAFETY_ON_TRUE_RETURN_VAL(err == TDM_ERROR_NO_CAPABILITY, EINA_FALSE);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(err == TDM_ERROR_NONE, EINA_FALSE);

   if (capabilities & cap)
     {
        if (mirror->capture)
          return EINA_TRUE;

        capture = tdm_output_create_capture(mirror->tdm_output, &err);
        if (err != TDM_ERROR_NONE)
          {
             ERR("_e_tz_screenmirror_tdm_capture_support: create tdm_capture failed");
             return EINA_FALSE;
          }

        CLEAR(capture_info);
        capture_info.dst_config.size.h = buffer->mbuf->width_from_pitch;
        capture_info.dst_config.size.v = buffer->mbuf->height;
        capture_info.dst_config.pos.x = 0;
        capture_info.dst_config.pos.y = 0;
        capture_info.dst_config.pos.w = buffer->mbuf->width;
        capture_info.dst_config.pos.h = buffer->mbuf->height;
        capture_info.dst_config.format = buffer->mbuf->tbmfmt;
        capture_info.transform = TDM_TRANSFORM_NORMAL;
        if (cap == TDM_CAPTURE_CAPABILITY_ONESHOT)
          capture_info.type = TDM_CAPTURE_TYPE_ONESHOT;
        else
          capture_info.type = TDM_CAPTURE_TYPE_STREAM;

        if (mirror->stretch == TIZEN_SCREENMIRROR_STRETCH_KEEP_RATIO)
          {
             if (_e_tz_screenmirror_position_get(mirror, buffer->mbuf, &dst_pos))
               {
                  capture_info.dst_config.pos.x = dst_pos.x;
                  capture_info.dst_config.pos.y = dst_pos.y;
                  capture_info.dst_config.pos.w = dst_pos.w;
                  capture_info.dst_config.pos.h = dst_pos.h;
               }
          }

        err = tdm_capture_set_info(capture, &capture_info);
        if (err != TDM_ERROR_NONE)
          {
             ERR("_e_tz_screenmirror_tdm_capture_support: tdm_capture set_info failed");
             tdm_capture_destroy(capture);
             return EINA_FALSE;
          }

        if (_e_tz_screenmirror_tdm_capture_handle_set(buffer, capture, cap) == EINA_FALSE)
          {
             tdm_capture_destroy(capture);
             return EINA_FALSE;
          }
        mirror->capture = capture;

        return EINA_TRUE;
     }

   return EINA_FALSE;
}

static void
_e_tz_screenmirror_buffer_queue(E_Mirror_Buffer *buffer)
{
   E_Mirror *mirror = buffer->mirror;
   tdm_error err = TDM_ERROR_NONE;

   mirror->buffer_queue = eina_list_append(mirror->buffer_queue, buffer);

   if (mirror->started)
     {
        if (buffer->mbuf->type == TYPE_SHM)
          {
             _e_tz_screenmirror_watch_vblank(mirror);
             return;
          }

        if (_e_tz_screenmirror_tdm_capture_support(buffer, TDM_CAPTURE_CAPABILITY_STREAM))
          {
             _e_tz_screenmirror_drm_buffer_clear_check(buffer);

             err = tdm_capture_attach(mirror->capture, buffer->mbuf->tbm_surface);
             EINA_SAFETY_ON_FALSE_RETURN(err == TDM_ERROR_NONE);

             err = tdm_capture_commit(mirror->capture);
             EINA_SAFETY_ON_FALSE_RETURN(err == TDM_ERROR_NONE);
          }
        else
          _e_tz_screenmirror_watch_vblank(mirror);
     }
   else
     {
        if (_e_tz_screenmirror_tdm_capture_support(buffer, TDM_CAPTURE_CAPABILITY_STREAM))
          {
             _e_tz_screenmirror_drm_buffer_clear_check(buffer);

             err = tdm_capture_attach(mirror->capture, buffer->mbuf->tbm_surface);
             EINA_SAFETY_ON_FALSE_RETURN(err == TDM_ERROR_NONE);
          }
     }
}

static void
_e_tz_screenmirror_buffer_dequeue(E_Mirror_Buffer *buffer)
{
   E_Mirror *mirror = buffer->mirror;

   EINA_SAFETY_ON_NULL_RETURN(mirror);
   if (!mirror->buffer_queue || !eina_list_data_find_list(mirror->buffer_queue, buffer))
     return;

   buffer->in_use = EINA_FALSE;
   mirror->buffer_queue = eina_list_remove(mirror->buffer_queue, buffer);

   /* resource == shooter means that we're using weston screenshooter
    * In case of wetson screenshooter, send a done event. Otherwise, send
    * a dequeued event for tizen_screenmirror.
    */
   if (mirror->resource == mirror->shooter)
     screenshooter_send_done(mirror->resource);
   else
     tizen_screenmirror_send_dequeued(mirror->resource, buffer->mbuf->resource);
}

static Eina_Bool
_e_tz_screenmirror_tdm_capture_oneshot(E_Mirror *mirror, E_Mirror_Buffer *buffer)
{
   tdm_error err = TDM_ERROR_NONE;

   err = tdm_capture_attach(mirror->capture, buffer->mbuf->tbm_surface);
   EINA_SAFETY_ON_FALSE_GOTO(err == TDM_ERROR_NONE, capture_fail);

   err = tdm_capture_commit(mirror->capture);
   EINA_SAFETY_ON_FALSE_GOTO(err == TDM_ERROR_NONE, capture_fail);

   return EINA_TRUE;

capture_fail:
   return EINA_FALSE;
}

static void
_e_tz_screenmirror_buffer_cb_destroy(struct wl_listener *listener, void *data)
{
   E_Mirror_Buffer *buffer = container_of(listener, E_Mirror_Buffer, destroy_listener);

   if (buffer->destroy_listener.notify)
     {
        wl_list_remove(&buffer->destroy_listener.link);
        buffer->destroy_listener.notify = NULL;
     }
}

static void
_e_tz_screenmirror_buffer_cb_free(E_Devmgr_Buf *mbuf, void *data)
{
   E_Mirror_Buffer *buffer = data;

   _e_tz_screenmirror_buffer_free(buffer);
}

static E_Mirror_Buffer*
_e_tz_screenmirror_buffer_get(E_Mirror *mirror, struct wl_resource *resource)
{
   E_Mirror_Buffer *buffer = NULL;
   struct wl_listener *listener;

   listener = wl_resource_get_destroy_listener(resource, _e_tz_screenmirror_buffer_cb_destroy);
   if (listener)
     return container_of(listener, E_Mirror_Buffer, destroy_listener);

   if (!(buffer = E_NEW(E_Mirror_Buffer, 1)))
      return NULL;

   /* FIXME: this is very tricky. DON'T add listner after e_devmgr_buffer_create. */
   buffer->destroy_listener.notify = _e_tz_screenmirror_buffer_cb_destroy;
   wl_resource_add_destroy_listener(resource, &buffer->destroy_listener);

   buffer->mbuf = e_devmgr_buffer_create(resource);
   EINA_SAFETY_ON_NULL_GOTO(buffer->mbuf, fail_get);

   buffer->mirror = mirror;

   DBG("capture buffer: %c%c%c%c %dx%d (%d,%d,%d) (%d,%d,%d)",
       FOURCC_STR(buffer->mbuf->tbmfmt),
       buffer->mbuf->width, buffer->mbuf->height,
       buffer->mbuf->pitches[0], buffer->mbuf->pitches[1], buffer->mbuf->pitches[2],
       buffer->mbuf->offsets[0], buffer->mbuf->offsets[1], buffer->mbuf->offsets[2]);

   e_devmgr_buffer_free_func_add(buffer->mbuf, _e_tz_screenmirror_buffer_cb_free, buffer);

   return buffer;
fail_get:
   E_FREE(buffer);
   return NULL;
}

static void
_e_tz_screenmirror_vblank_handler(void *data)
{
   E_Mirror *mirror = data;
   E_Mirror_Buffer *buffer;
   Eina_List *l;

   EINA_SAFETY_ON_NULL_RETURN(mirror);
   if (mirror != keep_mirror)
     return;

   mirror->wait_vblank = EINA_FALSE;

   EINA_LIST_FOREACH(mirror->buffer_queue, l, buffer)
     {
        if (!buffer->in_use) break;
     }

   /* can be null when client doesn't queue a buffer previously */
   if (!buffer)
     return;

   /* in case of wl_shm_buffer */
   if (wl_shm_buffer_get(buffer->mbuf->resource))
     {
        _e_tz_screenmirror_shm_dump(buffer);
        _e_tz_screenmirror_buffer_dequeue(buffer);
     }
   else if (wayland_tbm_server_get_surface(e_comp->wl_comp_data->tbm.server, buffer->mbuf->resource))
     {
        /* If _e_tz_screenmirror_drm_dump is failed, we dequeue buffer at now.
         * Otherwise, _e_tz_screenmirror_buffer_dequeue will be called in pp callback
         */
        if (!_e_tz_screenmirror_drm_dump(buffer))
          _e_tz_screenmirror_buffer_dequeue(buffer);
     }

   /* timer is a substitution for vblank during dpms off. so if timer is running,
    * we don't watch vblank events recursively.
    */
   if (!mirror->timer)
     _e_tz_screenmirror_watch_vblank(mirror);
}

static void
_e_tz_screenmirror_cb_client_destroy(struct wl_listener *listener, void *data)
{
   E_Mirror *mirror = container_of(listener, E_Mirror, client_destroy_listener);

   _e_tz_screenmirror_destroy(mirror);
}

static E_Mirror*
_e_tz_screenmirror_create(struct wl_client *client, struct wl_resource *shooter_resource, struct wl_resource *output_resource)
{
   E_Mirror *mirror = NULL;
   Ecore_Drm_Output *drm_output;
   Ecore_Drm_Device *dev;
   Eina_List *devs;
   Eina_List *l, *ll;
   tdm_error err = TDM_ERROR_NONE;
   int count, i, ret;
   unsigned int crtc_id;

   EINA_SAFETY_ON_FALSE_RETURN_VAL(keep_mirror == NULL, NULL);

   mirror = E_NEW(E_Mirror, 1);
   EINA_SAFETY_ON_NULL_RETURN_VAL(mirror, NULL);

   keep_mirror = mirror;

   mirror->stretch = TIZEN_SCREENMIRROR_STRETCH_KEEP_RATIO;
   mirror->shooter = shooter_resource;
   mirror->output = output_resource;
   mirror->wl_output = wl_resource_get_user_data(mirror->output);
   EINA_SAFETY_ON_NULL_GOTO(mirror->wl_output, fail_create);

   mirror->per_vblank = (mirror->wl_output->refresh / (DUMP_FPS * 1000));

   devs = eina_list_clone(ecore_drm_devices_get());
   EINA_LIST_FOREACH(devs, l, dev)
     EINA_LIST_FOREACH(dev->outputs, ll, drm_output)
       {
          int x, y;
          ecore_drm_output_position_get(drm_output, &x, &y);
          if (x != mirror->wl_output->x || y != mirror->wl_output->y) continue;
          mirror->drm_output = drm_output;
          mirror->drm_device = dev;
          break;
       }
   eina_list_free(devs);
   EINA_SAFETY_ON_NULL_GOTO(mirror->drm_output, fail_create);

   mirror->tdm_dpy = tdm_display_init(&err);
   EINA_SAFETY_ON_NULL_GOTO(mirror->tdm_dpy, fail_create);

   crtc_id = ecore_drm_output_crtc_id_get(mirror->drm_output);

   mirror->tdm_output = tdm_display_get_output(mirror->tdm_dpy, crtc_id, &err);
   EINA_SAFETY_ON_FALSE_GOTO(err == TDM_ERROR_NONE, fail_create);

   err = tdm_output_get_layer_count(mirror->tdm_output, &count);
   EINA_SAFETY_ON_FALSE_GOTO(err == TDM_ERROR_NONE, fail_create);
   EINA_SAFETY_ON_FALSE_GOTO(count >= 0, fail_create);

   for (i = 0; i < count; i++)
     {
        tdm_layer *layer;
        tdm_layer_capability capability;

        layer = tdm_output_get_layer(mirror->tdm_output, i, &err);
        EINA_SAFETY_ON_FALSE_GOTO(err == TDM_ERROR_NONE, fail_create);

        err = tdm_layer_get_capabilities (layer, &capability);
        EINA_SAFETY_ON_FALSE_GOTO(err == TDM_ERROR_NONE, fail_create);

        if (capability & TDM_LAYER_CAPABILITY_PRIMARY)
          {
            mirror->tdm_primary_layer = layer;
            break;
          }
     }
   EINA_SAFETY_ON_NULL_GOTO(mirror->tdm_primary_layer, fail_create);

   INF("per_vblank(%d)", mirror->per_vblank);

#ifdef ENABLE_CYNARA
   ret = cynara_initialize(&mirror->p_cynara, NULL);
   EINA_SAFETY_ON_FALSE_GOTO(ret == CYNARA_API_SUCCESS, fail_create);
   mirror->cynara_initialized = EINA_TRUE;
#endif

   mirror->client_destroy_listener.notify = _e_tz_screenmirror_cb_client_destroy;
   wl_client_add_destroy_listener(client, &mirror->client_destroy_listener);

   return mirror;
fail_create:
#ifdef ENABLE_CYNARA
   mirror->p_cynara = NULL;
#endif

   if (mirror->tdm_dpy)
      tdm_display_deinit(mirror->tdm_dpy);

   E_FREE(mirror);
   keep_mirror = NULL;

   return NULL;
}

static void
_e_tz_screenmirror_destroy(E_Mirror *mirror)
{
   E_Mirror_Buffer *buffer;
   Eina_List *l, *ll;
   E_Devmgr_Buf *mbuf;

   if (!mirror)
     return;

#ifdef ENABLE_CYNARA
   if (mirror->p_cynara) cynara_finish(mirror->p_cynara);
   mirror->p_cynara = NULL;
   mirror->cynara_initialized = EINA_FALSE;
#endif

   if (mirror->timer)
     ecore_timer_del(mirror->timer);

   if (mirror->client_destroy_listener.notify)
     {
        wl_list_remove(&mirror->client_destroy_listener.link);
        mirror->client_destroy_listener.notify = NULL;
     }

   wl_resource_set_destructor(mirror->resource, NULL);

   _e_tz_screenmirror_pp_destroy(mirror);

   EINA_LIST_FREE(mirror->buffer_clear_check, mbuf);

   EINA_LIST_FOREACH_SAFE(mirror->buffer_queue, l, ll, buffer)
     _e_tz_screenmirror_buffer_free(buffer);

   EINA_LIST_FOREACH_SAFE(mirror->ui_buffer_list, l, ll, mbuf)
     e_devmgr_buffer_unref(mbuf);

   if (mirror->capture)
     tdm_capture_destroy(mirror->capture);

   if (mirror->tdm_dpy)
     tdm_display_deinit(mirror->tdm_dpy);

   free(mirror);

   keep_mirror = NULL;
#if 0
   if (e_devmgr_buffer_list_length() > 0)
     e_devmgr_buffer_list_print(NULL);
#endif
}

static void
destroy_tz_screenmirror(struct wl_resource *resource)
{
   E_Mirror *mirror = wl_resource_get_user_data(resource);

   _e_tz_screenmirror_destroy(mirror);
}

static void
_e_tz_screenmirror_cb_destroy(struct wl_client *client EINA_UNUSED, struct wl_resource *resource)
{
   wl_resource_destroy(resource);
}

static void
_e_tz_screenmirror_cb_set_stretch(struct wl_client *client EINA_UNUSED, struct wl_resource *resource, uint32_t stretch)
{
   E_Mirror *mirror = wl_resource_get_user_data(resource);

   EINA_SAFETY_ON_NULL_RETURN(mirror);

   if (_e_screenmirror_privilege_check(client, mirror, wl_client_get_fd(client),
                                       PRIVILEGE_SCREENSHOT) == EINA_FALSE)
     {
        ERR("_e_tz_screenmirror_cb_set_stretch: priv check failed");
        return;
     }

   if (mirror->stretch == stretch)
     return;

   mirror->stretch = stretch;
}

static void
_e_tz_screenmirror_cb_queue(struct wl_client *client EINA_UNUSED, struct wl_resource *resource, struct wl_resource *buffer_resource)
{
   E_Mirror *mirror = wl_resource_get_user_data(resource);
   E_Mirror_Buffer *buffer;

   EINA_SAFETY_ON_NULL_RETURN(mirror);

   if (_e_screenmirror_privilege_check(client, mirror, wl_client_get_fd(client),
                                       PRIVILEGE_SCREENSHOT) == EINA_FALSE)
     {
        ERR("_e_tz_screenmirror_cb_queue: priv check failed");
        return;
     }

   if (!_e_tz_screenmirror_buffer_check(buffer_resource))
     {
        wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
                               "tizen_screenshooter failed: wrong buffer resource");
        return;
     }

   buffer = _e_tz_screenmirror_buffer_get(mirror, buffer_resource);
   if (!buffer)
     {
        wl_resource_post_no_memory(resource);
        return;
     }

   _e_tz_screenmirror_buffer_queue(buffer);
}

static void
_e_tz_screenmirror_cb_dequeue(struct wl_client *client EINA_UNUSED, struct wl_resource *resource, struct wl_resource *buffer_resource)
{
   E_Mirror *mirror = wl_resource_get_user_data(resource);
   E_Mirror_Buffer *buffer;

   EINA_SAFETY_ON_NULL_RETURN(mirror);

   if (_e_screenmirror_privilege_check(client, mirror, wl_client_get_fd(client),
                                       PRIVILEGE_SCREENSHOT) == EINA_FALSE)
     {
        ERR("_e_tz_screenmirror_cb_dequeue: priv check failed");
        return;
     }

   if (!_e_tz_screenmirror_buffer_check(buffer_resource))
     {
        wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
                               "tizen_screenshooter failed: wrong buffer resource");
        return;
     }

   buffer = _e_tz_screenmirror_buffer_get(mirror, buffer_resource);
   if (!buffer || !eina_list_data_find_list(mirror->buffer_queue, buffer))
     return;

   _e_tz_screenmirror_buffer_dequeue(buffer);
}

static void
_e_tz_screenmirror_cb_start(struct wl_client *client EINA_UNUSED, struct wl_resource *resource)
{
   E_Mirror *mirror = wl_resource_get_user_data(resource);
   tdm_error err = TDM_ERROR_NONE;

   EINA_SAFETY_ON_NULL_RETURN(mirror);

   if (_e_screenmirror_privilege_check(client, mirror, wl_client_get_fd(client),
                                       PRIVILEGE_SCREENSHOT) == EINA_FALSE)
     {
        ERR("_e_tz_screenmirror_cb_start: priv check failed");
        return;
     }

   if (mirror->started) return;

   mirror->started = EINA_TRUE;

   if (!mirror->buffer_queue)
     return;

   if (mirror->capture)
     {
        err = tdm_capture_commit(mirror->capture);
        EINA_SAFETY_ON_FALSE_RETURN(err == TDM_ERROR_NONE);
     }
   else
     _e_tz_screenmirror_watch_vblank(mirror);
}

static void
_e_tz_screenmirror_cb_stop(struct wl_client *client EINA_UNUSED, struct wl_resource *resource)
{
   E_Mirror *mirror = wl_resource_get_user_data(resource);

   EINA_SAFETY_ON_NULL_RETURN(mirror);

   if (_e_screenmirror_privilege_check(client, mirror, wl_client_get_fd(client),
                                       PRIVILEGE_SCREENSHOT) == EINA_FALSE)
     {
        ERR("_e_tz_screenmirror_cb_stop: priv check failed");
        return;
     }

   if (!mirror->started) return;

   mirror->started = EINA_FALSE;

   _e_tz_screenmirror_pp_destroy(mirror);
   tizen_screenmirror_send_stop(resource);
}

static const struct tizen_screenmirror_interface _e_tz_screenmirror_interface = {
   _e_tz_screenmirror_cb_destroy,
   _e_tz_screenmirror_cb_set_stretch,
   _e_tz_screenmirror_cb_queue,
   _e_tz_screenmirror_cb_dequeue,
   _e_tz_screenmirror_cb_start,
   _e_tz_screenmirror_cb_stop
};

static void
_e_tz_screenshooter_get_screenmirror(struct wl_client *client,
                                     struct wl_resource *resource,
                                     uint32_t id,
                                     struct wl_resource *output)
{
   int version = wl_resource_get_version(resource);
   E_Mirror *mirror;

   mirror = _e_tz_screenmirror_create(client, resource, output);
   if (!mirror)
     {
        wl_resource_post_no_memory(resource);
        return;
     }

   mirror->resource = wl_resource_create(client, &tizen_screenmirror_interface, version, id);
   if (mirror->resource == NULL)
     {
        _e_tz_screenmirror_destroy(mirror);
        wl_client_post_no_memory(client);
        return;
     }

   wl_resource_set_implementation(mirror->resource, &_e_tz_screenmirror_interface,
                                  mirror, destroy_tz_screenmirror);

   if (_e_screenmirror_privilege_check(client, mirror, wl_client_get_fd(client),
                                       PRIVILEGE_SCREENSHOT) == EINA_TRUE)
     DBG("_e_tz_screenshooter_get_screenmirror: priv check success");
   else
     DBG("_e_tz_screenshooter_get_screenmirror: priv check failed");

   tizen_screenmirror_send_content(mirror->resource, TIZEN_SCREENMIRROR_CONTENT_NORMAL);
}

static const struct tizen_screenshooter_interface _e_tz_screenshooter_interface =
{
   _e_tz_screenshooter_get_screenmirror
};

static void
_e_tz_screenshooter_cb_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
   struct wl_resource *res;
   int i;

   if (!(res = wl_resource_create(client, &tizen_screenshooter_interface, MIN(version, 1), id)))
     {
        ERR("Could not create tizen_screenshooter resource: %m");
        wl_client_post_no_memory(client);
        return;
     }

   if (_e_screenmirror_privilege_check_with_cynara_init(client, wl_client_get_fd(client),
                                                        PRIVILEGE_SCREENSHOT) == EINA_TRUE)
     tizen_screenshooter_send_screenshooter_notify(res, EINA_TRUE);
   else
     tizen_screenshooter_send_screenshooter_notify(res, EINA_FALSE);

   wl_resource_set_implementation(res, &_e_tz_screenshooter_interface, NULL, NULL);

   for (i = 0; i < NUM_MIRROR_FORMAT; i++)
     tizen_screenshooter_send_format(res, mirror_format_table[i]);
}

static void
_e_screenshooter_cb_shoot(struct wl_client *client,
                          struct wl_resource *resource,
                          struct wl_resource *output_resource,
                          struct wl_resource *buffer_resource)
{
   E_Mirror *mirror;
   E_Mirror_Buffer *buffer;

   if (!_e_tz_screenmirror_buffer_check(buffer_resource))
     {
        wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
                               "tizen_screenshooter failed: wrong buffer resource");
        return;
     }

   mirror = _e_tz_screenmirror_create(client, resource, output_resource);
   if (!mirror)
     {
        wl_resource_post_no_memory(resource);
        return;
     }

   /* resource == shooter means that we're using weston screenshooter */
   mirror->resource = mirror->shooter;

   if (_e_screenmirror_privilege_check(client, mirror, wl_client_get_fd(client),
                                       PRIVILEGE_SCREENSHOT) == EINA_FALSE)
     {
        ERR("_e_screenshooter_cb_shoot: priv check failed");
        screenshooter_send_done(mirror->resource);
        goto privilege_fail;
     }

   buffer = _e_tz_screenmirror_buffer_get(mirror, buffer_resource);
   if (!buffer)
     {
        wl_resource_post_no_memory(resource);
        _e_tz_screenmirror_destroy(mirror);
        return;
     }
   e_devmgr_buffer_clear(buffer->mbuf);

   mirror->buffer_queue = eina_list_append(mirror->buffer_queue, buffer);

   /* in case of shm, we dump only ui framebuffer */
   if (buffer->mbuf->type == TYPE_SHM)
     {
        _e_tz_screenmirror_shm_dump(buffer);
        goto dump_done;
     }

   if (_e_tz_screenmirror_tdm_capture_support(buffer, TDM_CAPTURE_CAPABILITY_ONESHOT))
     {
        if (_e_tz_screenmirror_tdm_capture_oneshot(mirror, buffer) == EINA_TRUE)
          return;
        else
          {
             tdm_capture_destroy(mirror->capture);
             mirror->capture = NULL;
          }
     }
   else
     _e_tz_screenmirror_dump_still(buffer);

dump_done:
   _e_tz_screenmirror_buffer_free(buffer);

privilege_fail:
   _e_tz_screenmirror_destroy(mirror);
}

static const struct screenshooter_interface _e_screenshooter_interface =
{
   _e_screenshooter_cb_shoot
};

static void
_e_screenshooter_cb_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
   struct wl_resource *res;

   if (!(res = wl_resource_create(client, &screenshooter_interface, MIN(version, 1), id)))
     {
        ERR("Could not create screenshooter resource");
        wl_client_post_no_memory(client);
        return;
     }

   wl_resource_set_implementation(res, &_e_screenshooter_interface, NULL, NULL);
}

int
e_devicemgr_screenshooter_init(void)
{
   if (!e_comp_wl) return 0;
   if (!e_comp_wl->wl.disp) return 0;

   /* try to add screenshooter to wayland globals */
   if (!wl_global_create(e_comp_wl->wl.disp, &screenshooter_interface, 1,
                         NULL, _e_screenshooter_cb_bind))
     {
        ERR("Could not add screenshooter to wayland globals");
        return 0;
     }

   /* try to add tizen_screenshooter to wayland globals */
   if (!wl_global_create(e_comp_wl->wl.disp, &tizen_screenshooter_interface, 1,
                         NULL, _e_tz_screenshooter_cb_bind))
     {
        ERR("Could not add tizen_screenshooter to wayland globals");
        return 0;
     }

   return 1;
}

void
e_devicemgr_screenshooter_fini(void)
{
}
