#include "e_devicemgr_viewport.h"

int _log_dom_viewport = -1;
#undef CRI
#undef ERR
#undef WRN
#undef INF
#undef DBG
#define CRI(...) EINA_LOG_DOM_CRIT(_log_dom_viewport, __VA_ARGS__)
#define ERR(...) EINA_LOG_DOM_ERR(_log_dom_viewport, __VA_ARGS__)
#define WRN(...) EINA_LOG_DOM_WARN(_log_dom_viewport, __VA_ARGS__)
#define INF(...) EINA_LOG_DOM_INFO(_log_dom_viewport, __VA_ARGS__)
#define DBG(...) EINA_LOG_DOM_DBG(_log_dom_viewport, __VA_ARGS__)

#define PER(fmt,arg...)   ERR("window(0x%08"PRIxPTR") ec(%p) epc(%p): "fmt, viewport->window, viewport->ec, viewport->epc, ##arg)
#define PWR(fmt,arg...)   WRN("window(0x%08"PRIxPTR") ec(%p) epc(%p):"fmt, viewport->window, viewport->ec, viewport->epc, ##arg)
#define PIN(fmt,arg...)   INF("window(0x%08"PRIxPTR") ec(%p) epc(%p):"fmt, viewport->window, viewport->ec, viewport->epc, ##arg)
#define PDB(fmt,arg...)   DBG("window(0x%08"PRIxPTR") ec(%p) epc(%p):"fmt, viewport->window, viewport->ec, viewport->epc, ##arg)

#undef SWAP
#define SWAP(a, b)  ({double t; t = a; a = b; b = t;})

typedef enum {
   DESTINATION_TYPE_NONE,
   DESTINATION_TYPE_RECT,
   DESTINATION_TYPE_RATIO,
   DESTINATION_TYPE_MODE,
} E_Viewport_Destination_Type;

typedef struct _E_Viewport {
   struct wl_resource *resource;

   E_Client *ec;
   E_Client *epc;
   E_Client *topmost;
   Ecore_Window window;

   Ecore_Event_Handler *topmost_rotate_hdl;

   struct wl_listener surface_destroy_listener;
   struct wl_listener surface_apply_viewport_listener;

   Eina_Bool changed;

   unsigned int transform;

   Eina_Rectangle source;
   Eina_Rectangle cropped_source;

   E_Viewport_Destination_Type type;
   struct {
      Eina_Rectangle rect;

      struct {
         double x, y, w, h;
      } ratio;

      struct {
         struct wl_resource *resource;

         enum tizen_destination_mode_type type;

         double ratio_h;
         double ratio_v;

         double scale_h;
         double scale_v;

         int offset_x;
         int offset_y;
         int offset_w;
         int offset_h;

         double align_h;
         double align_v;
      } mode;
   } destination;

   Eina_Bool query_parent_size;
   Eina_Rectangle parent_size;

   Eina_Bool follow_parent_transform;
} E_Viewport;

static E_Viewport* _e_devicemgr_viewport_get_viewport(struct wl_resource *resource);
static void _e_devicemgr_viewport_cb_resize(void *data, Evas *e, Evas_Object *obj, void *event_info);
static void _e_devicemgr_viewport_cb_move(void *data, Evas *e, Evas_Object *obj, void *event_info);
static void _e_devicemgr_viewport_cb_topmost_show(void *data, Evas *e, Evas_Object *obj, void *event_info);
static void _e_devicemgr_viewport_cb_topmost_resize(void *data, Evas *e, Evas_Object *obj, void *event_info);
static void _e_devicemgr_viewport_cb_topmost_move(void *data, Evas *e, Evas_Object *obj, void *event_info);
static void _e_devicemgr_viewport_cb_parent_resize(void *data, Evas *e, Evas_Object *obj, void *event_info);

static E_Client*
_topmost_parent_get(E_Client *ec)
{
   E_Client *parent = NULL;

   if (!ec->comp_data || !ec->comp_data->sub.data)
      return ec;

   parent = ec->comp_data->sub.data->parent;
   while (parent)
     {
        if (!parent->comp_data || !parent->comp_data->sub.data)
          return parent;

        parent = parent->comp_data->sub.data->parent;
     }

   return ec;
}

static void
_destroy_viewport(E_Viewport *viewport)
{
   E_Client *ec;

   if (!viewport) return;

   ec = viewport->ec;

   ecore_event_handler_del(viewport->topmost_rotate_hdl);

   if (viewport->query_parent_size)
     evas_object_event_callback_del_full(viewport->epc->frame, EVAS_CALLBACK_RESIZE,
                                         _e_devicemgr_viewport_cb_parent_resize, viewport);

   evas_object_event_callback_del_full(viewport->ec->frame, EVAS_CALLBACK_RESIZE,
                                       _e_devicemgr_viewport_cb_resize, viewport);
   evas_object_event_callback_del_full(viewport->ec->frame, EVAS_CALLBACK_MOVE,
                                       _e_devicemgr_viewport_cb_move, viewport);
   evas_object_event_callback_del_full(viewport->topmost->frame, EVAS_CALLBACK_SHOW,
                                       _e_devicemgr_viewport_cb_topmost_show, viewport);
   evas_object_event_callback_del_full(viewport->topmost->frame, EVAS_CALLBACK_RESIZE,
                                       _e_devicemgr_viewport_cb_topmost_resize, viewport);
   evas_object_event_callback_del_full(viewport->topmost->frame, EVAS_CALLBACK_MOVE,
                                       _e_devicemgr_viewport_cb_topmost_move, viewport);

   wl_list_remove(&viewport->surface_destroy_listener.link);
   wl_list_remove(&viewport->surface_apply_viewport_listener.link);

   wl_resource_set_user_data(viewport->resource, NULL);

   if (viewport->destination.mode.resource)
     wl_resource_set_user_data(viewport->destination.mode.resource, NULL);

   if (ec->comp_data && ec->comp_data->scaler.viewport)
     {
        ec->comp_data->scaler.viewport = NULL;
        ec->comp_data->scaler.buffer_viewport.buffer.src_width = wl_fixed_from_int(-1);
        ec->comp_data->scaler.buffer_viewport.surface.width = -1;
        ec->comp_data->scaler.buffer_viewport.changed = 1;
        ec->comp_data->pending.buffer_viewport.buffer.src_width = wl_fixed_from_int(-1);
        ec->comp_data->pending.buffer_viewport.surface.width = -1;
        ec->comp_data->pending.buffer_viewport.changed = 1;
     }

   PIN("tizen_viewport@%d destroy", wl_resource_get_id(viewport->resource));

   free(viewport);
}

static void
_e_devicemgr_destination_mode_destroy(struct wl_resource *resource)
{
   E_Viewport *viewport = wl_resource_get_user_data(resource);

   if (!viewport) return;

   if (viewport->type == DESTINATION_TYPE_MODE)
     viewport->type = DESTINATION_TYPE_NONE;

   viewport->changed = EINA_TRUE;

   PIN("destination.mode destroy");
}

static void
_e_devicemgr_destination_mode_cb_destroy(struct wl_client *client,
                                         struct wl_resource *resource)
{
   wl_resource_destroy(resource);
}

static void
_e_devicemgr_destination_mode_cb_follow_parent_transform(struct wl_client *client EINA_UNUSED,
                                                         struct wl_resource *resource)
{
   E_Viewport *viewport = wl_resource_get_user_data(resource);

   if (!viewport) return;

   if (viewport->follow_parent_transform)
     return;

   PDB("follow_parent_transform");

   viewport->follow_parent_transform = EINA_TRUE;
   viewport->changed = EINA_TRUE;
}

static void
_e_devicemgr_destination_mode_cb_unfollow_parent_transform(struct wl_client *client EINA_UNUSED,
                                                           struct wl_resource *resource)
{
   E_Viewport *viewport = wl_resource_get_user_data(resource);

   if (!viewport) return;

   if (!viewport->follow_parent_transform)
     return;

   PDB("unfollow_parent_transform");

   viewport->follow_parent_transform = EINA_FALSE;
   viewport->changed = EINA_TRUE;
}

static void
_e_devicemgr_destination_mode_cb_set(struct wl_client *client,
                                     struct wl_resource *resource,
                                     uint32_t type)
{
   E_Viewport *viewport = wl_resource_get_user_data(resource);

   if (!viewport) return;

   if (type > TIZEN_DESTINATION_MODE_TYPE_ORIGIN_OR_LETTER)
     {
        PER("invalid param: type(%d)", type);
        return;
     }

   if (type != TIZEN_DESTINATION_MODE_TYPE_NONE)
     viewport->type = DESTINATION_TYPE_MODE;

   if (viewport->destination.mode.type == type)
     return;

   PDB("type(%d)", type);

   viewport->destination.mode.type = type;
   viewport->changed = EINA_TRUE;
}

static void
_e_devicemgr_destination_mode_cb_set_ratio(struct wl_client *client,
                                           struct wl_resource *resource,
                                           wl_fixed_t horizontal,
                                           wl_fixed_t vertical)
{
   E_Viewport *viewport = wl_resource_get_user_data(resource);
   double ratio_h, ratio_v;

   if (!viewport) return;

   ratio_h = wl_fixed_to_double(horizontal);
   ratio_v = wl_fixed_to_double(vertical);

   if (ratio_h == -1.0)
     {
        PDB("reset destinatino ratio");
        viewport->destination.mode.ratio_h = ratio_h;
        viewport->changed = EINA_TRUE;
        return;
     }

   if (ratio_h <= 0 || ratio_v <= 0)
     {
        PER("invalid param: ratio_h(%.2f) ratio_v(%.2f)", ratio_h, ratio_v);
        return;
     }

   if (viewport->destination.mode.ratio_h == ratio_h &&
       viewport->destination.mode.ratio_v == ratio_v)
     return;

   PDB("ratio_h(%.2f) ratio_v(%.2f)", ratio_h, ratio_v);

   viewport->destination.mode.ratio_h = ratio_h;
   viewport->destination.mode.ratio_v = ratio_v;
   viewport->changed = EINA_TRUE;
}

static void
_e_devicemgr_destination_mode_cb_set_scale(struct wl_client *client,
                                           struct wl_resource *resource,
                                           wl_fixed_t horizontal,
                                           wl_fixed_t vertical)
{
   E_Viewport *viewport = wl_resource_get_user_data(resource);
   double scale_h, scale_v;

   if (!viewport) return;

   scale_h = wl_fixed_to_double(horizontal);
   scale_v = wl_fixed_to_double(vertical);

   if (scale_h == -1.0)
     {
        PDB("reset destinatino scale");
        viewport->destination.mode.scale_h = scale_h;
        viewport->changed = EINA_TRUE;
        return;
     }

   if (scale_h <= 0 || scale_v <= 0)
     {
        PER("invalid param: scale_h(%.2f) scale_v(%.2f)", scale_h, scale_v);
        return;
     }

   if (viewport->destination.mode.scale_h == scale_h &&
       viewport->destination.mode.scale_v == scale_v)
     return;

   PDB("scale_h(%.2f) scale_v(%.2f)", scale_h, scale_v);

   viewport->destination.mode.scale_h = scale_h;
   viewport->destination.mode.scale_v = scale_v;
   viewport->changed = EINA_TRUE;
}

static void
_e_devicemgr_destination_mode_cb_set_align(struct wl_client *client,
                                           struct wl_resource *resource,
                                           wl_fixed_t horizontal,
                                           wl_fixed_t vertical)
{
   E_Viewport *viewport = wl_resource_get_user_data(resource);
   double align_h, align_v;

   if (!viewport) return;

   align_h = wl_fixed_to_double(horizontal);
   align_v = wl_fixed_to_double(vertical);

   if (align_h == -1.0)
     {
        PDB("reset destinatino align");
        viewport->destination.mode.align_h = align_h;
        viewport->changed = EINA_TRUE;
        return;
     }

   if (align_h < 0.0)
      align_h = 0.0;
   else if (align_h > 1.0)
      align_h = 1.0;

   if (align_v < 0.0)
      align_v = 0.0;
   else if (align_v > 1.0)
      align_v = 1.0;

   if (viewport->destination.mode.align_h == align_h &&
       viewport->destination.mode.align_v == align_v)
     return;

   PDB("align_h(%.2f) align_v(%.2f)", align_h, align_v);

   viewport->destination.mode.align_h = align_h;
   viewport->destination.mode.align_v = align_v;
   viewport->changed = EINA_TRUE;
}

static void
_e_devicemgr_destination_mode_cb_set_offset(struct wl_client *client,
                                            struct wl_resource *resource,
                                            int32_t x,
                                            int32_t y,
                                            int32_t w,
                                            int32_t h)
{
   E_Viewport *viewport = wl_resource_get_user_data(resource);

   if (!viewport) return;

   if (viewport->destination.mode.offset_x == x &&
       viewport->destination.mode.offset_y == y &&
       viewport->destination.mode.offset_w == w &&
       viewport->destination.mode.offset_h == h)
     return;

   PDB("offset_x(%d) offset_y(%d) offset_w(%d) offset_h(%d)", x, y, w, h);

   viewport->destination.mode.offset_x = x;
   viewport->destination.mode.offset_y = y;
   viewport->destination.mode.offset_w = w;
   viewport->destination.mode.offset_h = h;
   viewport->changed = EINA_TRUE;
}

static const struct tizen_destination_mode_interface _e_devicemgr_destination_mode_interface =
{
   _e_devicemgr_destination_mode_cb_destroy,
   _e_devicemgr_destination_mode_cb_follow_parent_transform,
   _e_devicemgr_destination_mode_cb_unfollow_parent_transform,
   _e_devicemgr_destination_mode_cb_set,
   _e_devicemgr_destination_mode_cb_set_ratio,
   _e_devicemgr_destination_mode_cb_set_scale,
   _e_devicemgr_destination_mode_cb_set_align,
   _e_devicemgr_destination_mode_cb_set_offset,
};

static void
_e_devicemgr_viewport_cb_parent_resize(void *data, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event_info EINA_UNUSED)
{
   E_Viewport *viewport = data;
   Evas_Coord old_w, old_h;

   if (e_object_is_del(E_OBJECT(viewport->epc))) return;

   old_w = viewport->parent_size.w;
   old_h = viewport->parent_size.h;

   evas_object_geometry_get(viewport->epc->frame,
                            &viewport->parent_size.x, &viewport->parent_size.y,
                            &viewport->parent_size.w, &viewport->parent_size.h);

   if (old_w != viewport->parent_size.w || old_h != viewport->parent_size.h)
      tizen_viewport_send_parent_size(viewport->resource,
                                      viewport->parent_size.w,
                                      viewport->parent_size.h);
}

static void
_e_devicemgr_viewport_destroy(struct wl_resource *resource)
{
   E_Viewport *viewport = _e_devicemgr_viewport_get_viewport(resource);

   if (!viewport) return;

   _destroy_viewport(viewport);
}

static void
_e_devicemgr_viewport_cb_destroy(struct wl_client *client EINA_UNUSED,
                                 struct wl_resource *resource)
{
   wl_resource_destroy(resource);
}

static void
_e_devicemgr_viewport_cb_set_transform(struct wl_client *client EINA_UNUSED,
                                       struct wl_resource *resource,
                                       uint32_t transform)
{
   E_Viewport *viewport = _e_devicemgr_viewport_get_viewport(resource);

   if (!viewport) return;

   if (transform > WL_OUTPUT_TRANSFORM_FLIPPED_270)
     {
        PER("invalid param: transform(%d)", transform);
        return;
     }

   if (viewport->transform == transform)
     return;

   PDB("transform(%d)", transform);

   viewport->transform = transform;
   viewport->changed = EINA_TRUE;
}

static void
_e_devicemgr_viewport_cb_set_source(struct wl_client *client EINA_UNUSED,
                                    struct wl_resource *resource,
                                    uint32_t x,
                                    uint32_t y,
                                    uint32_t width,
                                    uint32_t height)
{
   E_Viewport *viewport = _e_devicemgr_viewport_get_viewport(resource);

   if (!viewport) return;

   if (viewport->source.x == x && viewport->source.y == y &&
       viewport->source.w == width && viewport->source.h == height)
     return;

   viewport->source.x = x;
   viewport->source.y = y;
   viewport->source.w = width;
   viewport->source.h = height;
   viewport->cropped_source = viewport->source;
   viewport->changed = EINA_TRUE;

   PDB("source(%d,%d %dx%d)", EINA_RECTANGLE_ARGS(&viewport->source));
}

static void
_e_devicemgr_viewport_cb_set_destination(struct wl_client *client EINA_UNUSED,
                                         struct wl_resource *resource,
                                         int32_t x,
                                         int32_t y,
                                         uint32_t width,
                                         uint32_t height)
{
   E_Viewport *viewport = _e_devicemgr_viewport_get_viewport(resource);

   if (!viewport) return;

   if (width == 0 || height == 0)
     {
        PER("invalid param: destination.rect(%d,%d %dx%d)", x, y, width, height);
        return;
     }

   /* ignore x, y pos in case of a toplevel-role surface(shell surface). The
    * position is decided by shell surface interface.
    */
   if (viewport->topmost == viewport->ec)
     x = y = 0;

   PDB("destination.rect(%d,%d %dx%d)", x, y, width, height);

   viewport->type = DESTINATION_TYPE_RECT;

   if (viewport->destination.rect.x == x && viewport->destination.rect.y == y &&
       viewport->destination.rect.w == width && viewport->destination.rect.h == height)
     return;

   viewport->destination.rect.x = x;
   viewport->destination.rect.y = y;
   viewport->destination.rect.w = width;
   viewport->destination.rect.h = height;
   viewport->changed = EINA_TRUE;
}

static void
_e_devicemgr_viewport_cb_set_destination_ratio(struct wl_client *client EINA_UNUSED,
                                               struct wl_resource *resource,
                                               wl_fixed_t x,
                                               wl_fixed_t y,
                                               wl_fixed_t width,
                                               wl_fixed_t height)
{
   E_Viewport *viewport = _e_devicemgr_viewport_get_viewport(resource);
   double ratio_x, ratio_y, ratio_w, ratio_h;

   if (!viewport) return;

   if (viewport->type == DESTINATION_TYPE_MODE)
     {
        PER("couldn't set viewport destination ratio. tizen_viewport@%d has the mode",
            wl_resource_get_id(resource));
        return;
     }

   ratio_x = (viewport->epc) ? wl_fixed_to_double(x) : 0;
   ratio_y = (viewport->epc) ? wl_fixed_to_double(y) : 0;
   ratio_w = wl_fixed_to_double(width);
   ratio_h = wl_fixed_to_double(height);

   if (ratio_x < 0 || ratio_x >= 1 || ratio_y < 0 || ratio_y >= 1 || ratio_w <= 0 || ratio_h <= 0)
     {
        PER("invalid param: destination.ratio(%.2f,%.2f %.2fx%.2f)", ratio_x, ratio_y, ratio_w, ratio_h);
        return;
     }

   PDB("destination.ratio(%.2f,%.2f %.2fx%.2f)", ratio_x, ratio_y, ratio_w, ratio_h);
   viewport->type = DESTINATION_TYPE_RATIO;

   if (viewport->destination.ratio.x == ratio_x && viewport->destination.ratio.y == ratio_y &&
       viewport->destination.ratio.w == ratio_w && viewport->destination.ratio.h == ratio_h)
     return;

   viewport->destination.ratio.x = ratio_x;
   viewport->destination.ratio.y = ratio_y;
   viewport->destination.ratio.w = ratio_w;
   viewport->destination.ratio.h = ratio_h;
   viewport->changed = EINA_TRUE;
}
static void
_e_devicemgr_viewport_cb_get_destination_mode(struct wl_client *client,
                                              struct wl_resource *resource,
                                              uint32_t id)
{
   int version = wl_resource_get_version(resource);
   E_Viewport *viewport = _e_devicemgr_viewport_get_viewport(resource);
   struct wl_resource *res;

   if (!viewport)
     {
        wl_resource_post_error(resource,
                               WL_DISPLAY_ERROR_INVALID_OBJECT,
                               "tizen_viewport@%d is invalid",
                               wl_resource_get_id(resource));
        return;
     }

   if (!(res = wl_resource_create(client, &tizen_destination_mode_interface, version, id)))
     {
        PER("Failed to create destination_mode resource");
        wl_resource_post_no_memory(resource);
        return;
     }

   memset(&viewport->destination.mode, 0, sizeof viewport->destination.mode);

   PIN("destination.mode created");

   viewport->destination.mode.resource = res;
   viewport->destination.mode.type = TIZEN_DESTINATION_MODE_TYPE_NONE;
   viewport->destination.mode.ratio_h = -1.0;
   viewport->destination.mode.scale_h = -1.0;
   viewport->destination.mode.align_h = -1.0;

   /* set resource implementation */
   wl_resource_set_implementation(res, &_e_devicemgr_destination_mode_interface,
                                  viewport, _e_devicemgr_destination_mode_destroy);

}

static void
_e_devicemgr_viewport_cb_query_parent_size(struct wl_client *client,
                                           struct wl_resource *resource)
{
   E_Viewport *viewport = _e_devicemgr_viewport_get_viewport(resource);
   Evas_Coord w = 0, h = 0;

   if (!viewport) return;

   if (viewport->epc)
     {
        evas_object_geometry_get(viewport->epc->frame,
                                 &viewport->parent_size.x, &viewport->parent_size.y,
                                 &viewport->parent_size.w, &viewport->parent_size.h);
        w = viewport->parent_size.w;
        h = viewport->parent_size.h;

        if (!viewport->query_parent_size)
          {
             viewport->query_parent_size = EINA_TRUE;
             evas_object_event_callback_add(viewport->epc->frame, EVAS_CALLBACK_RESIZE,
                                            _e_devicemgr_viewport_cb_parent_resize, viewport);
          }
     }
   else
     {
        E_Zone *zone = e_comp_zone_xy_get(viewport->ec->x, viewport->ec->y);
        if (zone)
          {
             w = zone->w;
             h = zone->h;
          }
        else
          PWR("out of zone");
     }

   tizen_viewport_send_parent_size(resource, w, h);
}

static void
_e_devicemgr_viewport_cb_follow_parent_transform(struct wl_client *client EINA_UNUSED,
                                                 struct wl_resource *resource)
{
   E_Viewport *viewport = wl_resource_get_user_data(resource);

   if (!viewport) return;

   if (viewport->follow_parent_transform)
     return;

   PDB("follow_parent_transform");

   viewport->follow_parent_transform = EINA_TRUE;
   viewport->changed = EINA_TRUE;
}

static void
_e_devicemgr_viewport_cb_unfollow_parent_transform(struct wl_client *client EINA_UNUSED,
                                                   struct wl_resource *resource)
{
   E_Viewport *viewport = wl_resource_get_user_data(resource);

   if (!viewport) return;

   if (!viewport->follow_parent_transform)
     return;

   PDB("unfollow_parent_transform");

   viewport->follow_parent_transform = EINA_FALSE;
   viewport->changed = EINA_TRUE;
}

static const struct tizen_viewport_interface _e_devicemgr_viewport_interface =
{
   _e_devicemgr_viewport_cb_destroy,
   _e_devicemgr_viewport_cb_set_transform,
   _e_devicemgr_viewport_cb_set_source,
   _e_devicemgr_viewport_cb_set_destination,
   _e_devicemgr_viewport_cb_set_destination_ratio,
   _e_devicemgr_viewport_cb_get_destination_mode,
   _e_devicemgr_viewport_cb_query_parent_size,
   _e_devicemgr_viewport_cb_follow_parent_transform,
   _e_devicemgr_viewport_cb_unfollow_parent_transform,
};

static void
_buffer_size_get(E_Pixmap *pixmap, int *bw, int *bh)
{
   E_Comp_Wl_Buffer *buffer = e_pixmap_resource_get(pixmap);

   *bw = *bh = 0;

   if (!buffer) return;

   if (buffer->type == E_COMP_WL_BUFFER_TYPE_VIDEO)
     {
        tbm_surface_h tbm_surface = wayland_tbm_server_get_surface(NULL, buffer->resource);

        *bw = tbm_surface_get_width(tbm_surface);
        *bh = tbm_surface_get_height(tbm_surface);
     }
   else
     {
        *bw = buffer->w;
        *bh = buffer->h;
     }
}

static void
_source_transform_coord(int width, int height, int trans, int scale, float ox, float oy, float *tx, float *ty)
{
   switch (trans)
     {
      case WL_OUTPUT_TRANSFORM_NORMAL:
      default:
        *tx = ox, *ty = oy;
        break;
      case WL_OUTPUT_TRANSFORM_FLIPPED:
        *tx = width - ox, *ty = oy;
        break;
      case WL_OUTPUT_TRANSFORM_90:
        *tx = oy, *ty = width - ox;
        break;
      case WL_OUTPUT_TRANSFORM_FLIPPED_90:
        *tx = height - oy, *ty = width - ox;
        break;
      case WL_OUTPUT_TRANSFORM_180:
        *tx = width - ox, *ty = height - oy;
        break;
      case WL_OUTPUT_TRANSFORM_FLIPPED_180:
        *tx = ox, *ty = height - oy;
        break;
      case WL_OUTPUT_TRANSFORM_270:
        *tx = height - oy, *ty = ox;
        break;
      case WL_OUTPUT_TRANSFORM_FLIPPED_270:
        *tx = oy, *ty = ox;
        break;
     }

   *tx /= scale;
   *ty /= scale;
}

static void
_source_transform_to_surface(int width, int height, int trans, int scale,
                             Eina_Rectangle *orig, Eina_Rectangle *tran)
{
   float x1, x2, y1, y2;

   _source_transform_coord(width, height, trans, scale, orig->x, orig->y, &x1, &y1);
   _source_transform_coord(width, height, trans, scale, orig->x + orig->w, orig->y + orig->h, &x2, &y2);

   tran->x = (x1 <= x2) ? x1 : x2;
   tran->w = (x1 <= x2) ? x2 - x1 : x1 - x2;
   tran->y = (y1 <= y2) ? y1 : y2;
   tran->h = (y1 <= y2) ? y2 - y1 : y1 - y2;
}

static void
_destination_mode_calculate_letter_box(int pw, int ph, int sw, int sh,
                                       double rh, double rv,
                                       Eina_Rectangle *rect)
{
   int fit_width;
   int fit_height;
   double fw, fh, ssw, ssh, max;

   ssw = sw;
   if (rh != -1.0)
     ssh = (double)sw * rv / rh;
   else
     ssh = sh;

   fw = ssw / pw;
   fh = ssh / ph;
   max = MAX(fw, fh);

   fit_width = ssw / max;
   fit_height = ssh / max;

   rect->x = (pw - fit_width) / 2;
   rect->y = (ph - fit_height) / 2;
   rect->w = fit_width;
   rect->h = fit_height;
}

static void
_destination_mode_calculate_origin(int pw, int ph, int sw, int sh,
                                   double rh, double rv,
                                   Eina_Rectangle *rect)
{
   rect->x = (pw - sw) / 2;
   rect->y = (ph - sh) / 2;
   rect->w = sw;
   rect->h = sh;
}

static void
_destination_mode_calculate_full(int pw, int ph, int sw, int sh,
                                 double rh, double rv,
                                 Eina_Rectangle *rect)
{
   rect->x = rect->y = 0;
   rect->w = pw;
   rect->h = ph;
}

static void
_destination_mode_calculate_cropped_full(int pw, int ph, int sw, int sh,
                                         double rh, double rv,
                                         Eina_Rectangle *rect)
{
   int fit_width;
   int fit_height;
   double fw, fh, ssw, ssh, min;

   ssw = sw;
   if (rh != -1.0)
     ssh = (double)sw * rv / rh;
   else
     ssh = sh;

   fw = ssw / pw;
   fh = ssh / ph;
   min = MIN(fw, fh);

   fit_width = ssw / min;
   fit_height = ssh / min;

   rect->x = (pw - fit_width) / 2;
   rect->y = (ph - fit_height) / 2;
   rect->w = fit_width;
   rect->h = fit_height;
}

static void
_destination_mode_calculate_origin_or_letter(int pw, int ph, int sw, int sh,
                                             double rh, double rv,
                                             Eina_Rectangle *rect)
{
   if (sw < pw && sh < ph)
      _destination_mode_calculate_origin(pw, ph, sw, sh, rh, rv, rect);
   else
      _destination_mode_calculate_letter_box(pw, ph, sw, sh, rh, rv, rect);
}

static Eina_Bool
_destination_mode_calculate_destination(E_Viewport *viewport, Eina_Rectangle *prect, Eina_Rectangle *rect)
{
   E_Client *ec = viewport->ec;
   E_Comp_Wl_Buffer_Viewport *vp;
   int sw = 0, sh = 0;
   double rh = -1.0, rv = -1.0;

   vp = &ec->comp_data->scaler.buffer_viewport;

   if (viewport->source.w != -1)
     {
        sw = viewport->source.w;
        sh = viewport->source.h;
     }
   else
     _buffer_size_get(ec->pixmap, &sw, &sh);

   if (vp->buffer.transform % 2)
      SWAP(sw, sh);

   PDB("parent(%dx%d) src(%dx%d)", prect->w, prect->h, sw, sh);

   /* ratio -> type -> scale -> offset -> align */
   if (viewport->destination.mode.ratio_h != -1.0)
     {
        if (vp->buffer.transform % 2)
          {
             rh = viewport->destination.mode.ratio_v;
             rv = viewport->destination.mode.ratio_h;
          }
        else
          {
             rh = viewport->destination.mode.ratio_h;
             rv = viewport->destination.mode.ratio_v;
          }
     }

   PDB("%dx%d %dx%d %.2fx%.2f (%d,%d %dx%d)", prect->w, prect->h, sw, sh, rh, rv, EINA_RECTANGLE_ARGS(rect));

   switch(viewport->destination.mode.type)
     {
      case TIZEN_DESTINATION_MODE_TYPE_LETTER_BOX:
         _destination_mode_calculate_letter_box(prect->w, prect->h, sw, sh, rh, rv, rect);
         break;
      case TIZEN_DESTINATION_MODE_TYPE_ORIGIN:
         _destination_mode_calculate_origin(prect->w, prect->h, sw, sh, rh, rv, rect);
         break;
      case TIZEN_DESTINATION_MODE_TYPE_FULL:
         _destination_mode_calculate_full(prect->w, prect->h, sw, sh, rh, rv, rect);
         break;
      case TIZEN_DESTINATION_MODE_TYPE_CROPPED_FULL:
         _destination_mode_calculate_cropped_full(prect->w, prect->h, sw, sh, rh, rv, rect);
         break;
      case TIZEN_DESTINATION_MODE_TYPE_ORIGIN_OR_LETTER:
         _destination_mode_calculate_origin_or_letter(prect->w, prect->h, sw, sh, rh, rv, rect);
         break;
      case TIZEN_DESTINATION_MODE_TYPE_NONE:
      default:
         PER("no destination mode for tizen_viewport@%d", wl_resource_get_id(viewport->resource));
         return EINA_FALSE;
     }

   PDB("(%d,%d %dx%d)", EINA_RECTANGLE_ARGS(rect));

   if (viewport->destination.mode.scale_h != -1.0)
     {
        int new_x, new_y, new_w, new_h;
        double h = viewport->destination.mode.scale_h;
        double v = viewport->destination.mode.scale_v;

        if (vp->buffer.transform % 2)
          SWAP(h, v);

        new_w = rect->w * h;
        new_h = rect->h * v;
        new_x = rect->x + (rect->w - new_w) / 2;
        new_y = rect->y + (rect->h - new_h) / 2;
        rect->x = new_x;
        rect->y = new_y;
        rect->w = new_w;
        rect->h = new_h;
     }

   PDB("(%d,%d %dx%d)", EINA_RECTANGLE_ARGS(rect));

   if (viewport->destination.mode.align_h != -1.0)
     {
        E_Client *epc = viewport->epc;
        double h = viewport->destination.mode.align_h;
        double v = viewport->destination.mode.align_v;
        int dx, dy;

        if (epc)
          {
             E_Comp_Wl_Buffer_Viewport *vpp;

             if (!epc->comp_data || e_object_is_del(E_OBJECT(epc)))
               return EINA_FALSE;

             vpp = &epc->comp_data->scaler.buffer_viewport;

             PDB("parent's transform(%d)", vpp->buffer.transform);

             switch (vpp->buffer.transform)
               {
                default:
                case WL_OUTPUT_TRANSFORM_NORMAL:
                  break;
                case WL_OUTPUT_TRANSFORM_90:
                  SWAP(h, v);
                  v = 1.0 - v;
                  break;
                case WL_OUTPUT_TRANSFORM_180:
                  h = 1.0 - h;
                  v = 1.0 - v;
                  break;
                case WL_OUTPUT_TRANSFORM_270:
                  SWAP(h, v);
                  h = 1.0 - h;
                  break;
                case WL_OUTPUT_TRANSFORM_FLIPPED:
                  h = 1.0 - h;
                  break;
                case WL_OUTPUT_TRANSFORM_FLIPPED_90:
                  SWAP(h, v);
                  h = 1.0 - h;
                  v = 1.0 - v;
                  break;
                  break;
                case WL_OUTPUT_TRANSFORM_FLIPPED_180:
                  v = 1.0 - v;
                  break;
                case WL_OUTPUT_TRANSFORM_FLIPPED_270:
                  SWAP(h, v);
                  break;
               }
          }

        dx = (prect->w - rect->w) * (h - 0.5);
        dy = (prect->h - rect->h) * (v - 0.5);

        rect->x += dx;
        rect->y += dy;
     }

   PDB("(%d,%d %dx%d)", EINA_RECTANGLE_ARGS(rect));

   if (viewport->destination.mode.offset_x != 0 ||
       viewport->destination.mode.offset_y != 0 ||
       viewport->destination.mode.offset_w != 0 ||
       viewport->destination.mode.offset_h != 0)
     {
        int x = viewport->destination.mode.offset_x;
        int y = viewport->destination.mode.offset_y;
        int w = viewport->destination.mode.offset_w;
        int h = viewport->destination.mode.offset_h;

        if (vp->buffer.transform % 2)
          {
             SWAP(x, y);
             SWAP(w, h);
          }

        rect->x += x;
        rect->y += y;
        rect->w += w;
        rect->h += h;
     }

   PDB("mode destination(%d,%d %dx%d)", EINA_RECTANGLE_ARGS(rect));

   return EINA_TRUE;
}

static Eina_Bool
_e_devicemgr_viewport_calculate_destination(E_Viewport *viewport, Eina_Rectangle *prect, Eina_Rectangle *rect)
{
   switch (viewport->type)
     {
      case DESTINATION_TYPE_RECT:
         rect->x = viewport->destination.rect.x;
         rect->y = viewport->destination.rect.y;
         rect->w = viewport->destination.rect.w;
         rect->h = viewport->destination.rect.h;
         break;
      case DESTINATION_TYPE_RATIO:
         rect->x = viewport->destination.ratio.x * prect->w;
         rect->y = viewport->destination.ratio.y * prect->h;
         rect->w = viewport->destination.ratio.w * prect->w;
         rect->h = viewport->destination.ratio.h * prect->h;
         break;
      case DESTINATION_TYPE_MODE:
      case DESTINATION_TYPE_NONE:
      default:
         PER("wrong destination type: %d", viewport->type);
         return EINA_FALSE;
     }

   if (viewport->epc)
     {
        E_Client *epc = viewport->epc;
        E_Comp_Wl_Buffer_Viewport *vpp;

        vpp = &epc->comp_data->scaler.buffer_viewport;
        if (vpp->buffer.transform > 0)
          {
             if (vpp->buffer.transform % 2)
               _source_transform_to_surface(prect->h, prect->w, vpp->buffer.transform, 1, rect, rect);
             else
               _source_transform_to_surface(prect->w, prect->h, vpp->buffer.transform, 1, rect, rect);
          }
     }

   PDB("destination(%d,%d %dx%d)", EINA_RECTANGLE_ARGS(rect));

   return EINA_TRUE;
}

static void
_e_devicemgr_viewport_crop_by_parent(E_Viewport *viewport, Eina_Rectangle *parent, Eina_Rectangle *dst)
{
   E_Comp_Wl_Buffer_Viewport *vp = &viewport->ec->comp_data->scaler.buffer_viewport;
   Eina_Rectangle crop;
   double rx, ry, rw, rh;
   int bw, bh;

   PDB("dst(%d,%d %dx%d) parent(%d,%d %dx%d)", EINA_RECTANGLE_ARGS(dst), EINA_RECTANGLE_ARGS(parent));

   crop = *dst;

   if (!eina_rectangle_intersection(&crop, parent))
     {
        *dst = crop;
        PDB("dst(%d,%d %dx%d)", EINA_RECTANGLE_ARGS(dst));
        return;
     }

   if (crop.w == dst->w && crop.h == dst->h)
     return;

   PDB("dst(%d,%d %dx%d)", EINA_RECTANGLE_ARGS(dst));

   crop.x -= dst->x;
   crop.y -= dst->y;

   rx = (double)crop.x / dst->w;
   ry = (double)crop.y / dst->h;
   rw = (double)crop.w / dst->w;
   rh = (double)crop.h / dst->h;

   crop.x += dst->x;
   crop.y += dst->y;
   *dst = crop;

   PDB("  => (%d,%d %dx%d)", EINA_RECTANGLE_ARGS(dst));

   _buffer_size_get(viewport->ec->pixmap, &bw, &bh);

   if (viewport->source.w == -1)
     {
        viewport->cropped_source.x = viewport->cropped_source.y = 0;
        viewport->cropped_source.w = bw;
        viewport->cropped_source.h = bh;
     }
   else
     viewport->cropped_source = viewport->source;

   PDB("src(%d,%d %dx%d) ratio(%.2f,%.2f,%.2f,%.2f)",
       EINA_RECTANGLE_ARGS(&viewport->cropped_source), rx, ry, rw, rh);

   viewport->cropped_source.x += viewport->cropped_source.w * rx;
   viewport->cropped_source.y += viewport->cropped_source.h * ry;
   viewport->cropped_source.w = viewport->cropped_source.w * rw;
   viewport->cropped_source.h = viewport->cropped_source.h * rh;

   _source_transform_to_surface(bw, bh, vp->buffer.transform, 1, &viewport->cropped_source, &viewport->cropped_source);

   vp->buffer.src_x = wl_fixed_from_int(viewport->cropped_source.x);
   vp->buffer.src_y = wl_fixed_from_int(viewport->cropped_source.y);
   vp->buffer.src_width = wl_fixed_from_int(viewport->cropped_source.w);
   vp->buffer.src_height = wl_fixed_from_int(viewport->cropped_source.h);

   PDB("  => (%d,%d %dx%d)", EINA_RECTANGLE_ARGS(&viewport->cropped_source));
}

static Eina_Bool
_e_devicemgr_viewport_apply_transform(E_Viewport *viewport, int *rtransform)
{
   E_Client *ec = viewport->ec;
   E_Comp_Wl_Buffer_Viewport *vp = &ec->comp_data->scaler.buffer_viewport;
   Eina_Bool changed = EINA_FALSE;
   unsigned int new_transform;

   new_transform = viewport->transform;

   if (viewport->follow_parent_transform && viewport->epc)
     {
        E_Client *epc = viewport->epc;
        E_Comp_Wl_Buffer_Viewport *vpp;
        unsigned int pwtran, ptran, pflip, ctran, cflip;

        if (!epc->comp_data || e_object_is_del(E_OBJECT(epc)))
          {
             *rtransform = vp->buffer.transform;
             return EINA_FALSE;
          }

        vpp = &epc->comp_data->scaler.buffer_viewport;

        PDB("parent's transform(%d) rot.ang.curr(%d)",
            vpp->buffer.transform, epc->e.state.rot.ang.curr/90);

        pwtran = ((epc->e.state.rot.ang.curr + 360) % 360) / 90;

        ptran = ((pwtran & 0x3) + (vpp->buffer.transform & 0x3)) & 0x3;
        pflip = (vpp->buffer.transform & 0x4);

        ctran = (viewport->transform & 0x3);
        cflip = (viewport->transform & 0x4);

        new_transform = ((ptran + ctran) & 0x3) + ((pflip + cflip) & 0x4);
     }

   if (new_transform != vp->buffer.transform)
     {
        vp->buffer.transform = new_transform;
        vp->changed = changed = EINA_TRUE;

        ec->comp_data->pending.buffer_viewport = *vp;
        if (ec->comp_data->sub.data)
          ec->comp_data->sub.data->cached.buffer_viewport = *vp;
     }

   PDB("apply transform: %d type(%d) follow(%d) changed(%d)",
       vp->buffer.transform, viewport->type,
       viewport->follow_parent_transform, changed);

   *rtransform = vp->buffer.transform;

   return changed;
}

static Eina_Bool
_e_devicemgr_viewport_apply_destination(E_Viewport *viewport, Eina_Rectangle *rrect)
{
   E_Client *ec = viewport->ec;
   E_Comp_Wl_Buffer_Viewport *vp;
   Eina_Rectangle dst = {0,}, prect;
   Eina_Bool changed = EINA_FALSE;

   vp = &ec->comp_data->scaler.buffer_viewport;
   if (!viewport->epc)
     {
        E_Zone *zone = e_comp_zone_xy_get(ec->x, ec->y);

        EINA_SAFETY_ON_FALSE_RETURN_VAL(zone != NULL, EINA_FALSE);

        prect.x = prect.y = 0;
        prect.w = zone->w;
        prect.h = zone->h;
     }
   else
     {
        E_Client *epc = viewport->epc;
        E_Comp_Wl_Buffer_Viewport *vpp;

        if (!epc->comp_data || e_object_is_del(E_OBJECT(epc)))
          return EINA_FALSE;

        vpp = &epc->comp_data->scaler.buffer_viewport;
        prect.x = prect.y = 0;
        if (vpp->surface.width != -1)
          {
             prect.w = vpp->surface.width;
             prect.h = vpp->surface.height;
          }
        else
          _buffer_size_get(epc->pixmap, &prect.w, &prect.h);
     }

   EINA_SAFETY_ON_FALSE_RETURN_VAL(prect.w > 0 && prect.h > 0, EINA_FALSE);

   switch (viewport->type)
     {
      case DESTINATION_TYPE_RECT:
      case DESTINATION_TYPE_RATIO:
         if (!_e_devicemgr_viewport_calculate_destination(viewport, &prect, &dst))
           return EINA_FALSE;
         break;
      case DESTINATION_TYPE_MODE:
         if (!_destination_mode_calculate_destination(viewport, &prect, &dst))
           return EINA_FALSE;
         break;
      case DESTINATION_TYPE_NONE:
      default:
         PER("no destination for tizen_viewport@%d", wl_resource_get_id(viewport->resource));
         return EINA_FALSE;
     }

   _e_devicemgr_viewport_crop_by_parent(viewport, &prect, &dst);

   /* The values of below x, y, w, h are specified in the transform 0 and in the parent */
   if (ec->comp_data->sub.data)
     {
        if (ec->comp_data->sub.data->position.x != dst.x ||
            ec->comp_data->sub.data->position.y != dst.y)
          {
             ec->comp_data->sub.data->position.x = dst.x;
             ec->comp_data->sub.data->position.y = dst.y;
             ec->comp_data->sub.data->position.set = EINA_TRUE;
             vp->changed = changed = EINA_TRUE;
          }
     }
   else
     {
        /* if toplevel surface, the x,y pos is decided by shell surface */
        dst.x = ec->x;
        dst.y = ec->y;
     }

   if (vp->surface.width != dst.w || vp->surface.height != dst.h)
     {
        vp->surface.width = dst.w;
        vp->surface.height = dst.h;
        vp->changed = changed = EINA_TRUE;

        ec->comp_data->pending.buffer_viewport = *vp;
        if (ec->comp_data->sub.data)
          ec->comp_data->sub.data->cached.buffer_viewport = *vp;
     }

   *rrect = dst;

   PDB("apply destination: %d,%d %dx%d changed(%d)", EINA_RECTANGLE_ARGS(&dst), changed);

   return changed;
}

static Eina_Bool
_e_devicemgr_viewport_apply_source(E_Viewport *viewport)
{
   E_Client *ec = viewport->ec;
   E_Comp_Wl_Buffer_Viewport *vp = &ec->comp_data->scaler.buffer_viewport;
   Eina_Rectangle rect = {0,};
   int bw = 0, bh = 0;
   wl_fixed_t fx, fy, fw, fh;
   Eina_Bool changed = EINA_FALSE;

   if (viewport->cropped_source.w == -1)
     return EINA_FALSE;

   _buffer_size_get(ec->pixmap, &bw, &bh);

   rect.w = bw;
   rect.h = bh;

   if (!eina_rectangle_intersection(&rect, &viewport->cropped_source))
     {
        PER("source area is empty");
        return EINA_FALSE;
     }

   _source_transform_to_surface(bw, bh, vp->buffer.transform, 1, &rect, &rect);

   fx = wl_fixed_from_int(rect.x);
   fy = wl_fixed_from_int(rect.y);
   fw = wl_fixed_from_int(rect.w);
   fh = wl_fixed_from_int(rect.h);

   if (vp->buffer.src_x != fx || vp->buffer.src_y != fy ||
       vp->buffer.src_width != fw || vp->buffer.src_height != fh)
     {
        vp->buffer.src_x = wl_fixed_from_int(rect.x);
        vp->buffer.src_y = wl_fixed_from_int(rect.y);
        vp->buffer.src_width = wl_fixed_from_int(rect.w);
        vp->buffer.src_height = wl_fixed_from_int(rect.h);
        vp->changed = changed = EINA_TRUE;

        ec->comp_data->pending.buffer_viewport = *vp;
        if (ec->comp_data->sub.data)
          ec->comp_data->sub.data->cached.buffer_viewport = *vp;
     }

   PDB("apply source: %d,%d %dx%d orig(%d,%d %dx%d) changed(%d)",
       EINA_RECTANGLE_ARGS(&rect), EINA_RECTANGLE_ARGS(&viewport->cropped_source), changed);

   return changed;
}

static Eina_Bool
_e_devicemgr_viewport_apply(E_Client *ec)
{
   E_Viewport *viewport;
   E_Client *subc;
   Eina_List *l;

   if (!ec || !ec->comp_data || e_object_is_del(E_OBJECT(ec)))
     return EINA_TRUE;

   viewport = _e_devicemgr_viewport_get_viewport(ec->comp_data->scaler.viewport);

   if (viewport && e_pixmap_resource_get(ec->pixmap))
     {
        Eina_Bool changed = EINA_FALSE, src_changed = EINA_FALSE;
        Eina_Rectangle rrect = {0,};
        int rtransform = 0;

        PDB("apply: ec(%p)", ec);

        changed |= _e_devicemgr_viewport_apply_transform(viewport, &rtransform);
        changed |= _e_devicemgr_viewport_apply_destination(viewport, &rrect);
        src_changed |= _e_devicemgr_viewport_apply_source(viewport);

        if (changed || src_changed)
          {
             e_comp_wl_map_size_cal_from_buffer(viewport->ec);
             e_comp_wl_map_size_cal_from_viewport(viewport->ec);
             e_comp_wl_map_apply(viewport->ec);

             if (changed)
               {
                  PDB("send destination_changed: transform(%d) x(%d) y(%d) w(%d) h(%d)",
                      rtransform, rrect.x, rrect.y, rrect.w, rrect.h);
                  tizen_viewport_send_destination_changed(viewport->resource, rtransform,
                                                          rrect.x, rrect.y, rrect.w, rrect.h);
               }
          }
     }

   EINA_LIST_FOREACH(ec->comp_data->sub.list, l, subc)
     _e_devicemgr_viewport_apply(subc);

   EINA_LIST_FOREACH(ec->comp_data->sub.below_list, l, subc)
     _e_devicemgr_viewport_apply(subc);

   return EINA_TRUE;
}

static void
_e_devicemgr_viewport_cb_surface_destroy(struct wl_listener *listener, void *data)
{
   E_Viewport *viewport = container_of(listener, E_Viewport, surface_destroy_listener);

   _destroy_viewport(viewport);
}

static void
_e_devicemgr_viewport_cb_apply_viewport(struct wl_listener *listener, void *data)
{
   E_Viewport *viewport = container_of(listener, E_Viewport, surface_apply_viewport_listener);
   E_Client *ec = viewport->ec;
   E_Comp_Wl_Buffer_Viewport *vp = &ec->comp_data->scaler.buffer_viewport;

   if (vp->changed)
     {
        PWR("The client seems to use wl_viewport interface. Ignore it.");
        viewport->changed = EINA_TRUE;
     }

   if (!viewport->changed) return;
   if (!e_pixmap_resource_get(ec->pixmap)) return;
   if (viewport->epc && !e_pixmap_resource_get(viewport->epc->pixmap)) return;

   viewport->changed = EINA_FALSE;

   PDB("apply");

   if (!_e_devicemgr_viewport_apply(viewport->topmost))
     {
        PER("failed to apply tizen_viewport");
        return;
     }

   vp->changed = EINA_TRUE;
}

static void
_e_devicemgr_viewport_cb_resize(void *data, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event_info EINA_UNUSED)
{
   E_Viewport *viewport = data;
   Evas_Coord w, h;

   evas_object_geometry_get(obj, NULL, NULL, &w, &h);
   PDB("resized: %dx%d", w, h);
}

static void
_e_devicemgr_viewport_cb_move(void *data, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event_info EINA_UNUSED)
{
   E_Viewport *viewport = data;
   Evas_Coord x, y;

   evas_object_geometry_get(obj, &x, &y, NULL, NULL);
   PDB("moved: %d,%d", x, y);
}

static void
_e_devicemgr_viewport_cb_topmost_show(void *data, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event_info EINA_UNUSED)
{
   E_Viewport *viewport = data;

   PDB("show start");
   _e_devicemgr_viewport_apply(viewport->topmost);
   PDB("show end");
}

static void
_e_devicemgr_viewport_cb_topmost_resize(void *data, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event_info EINA_UNUSED)
{
   E_Viewport *viewport = data;

   PDB("resize start");
   _e_devicemgr_viewport_apply(viewport->topmost);
   PDB("resize end");
}

static void
_e_devicemgr_viewport_cb_topmost_move(void *data, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event_info EINA_UNUSED)
{
   E_Viewport *viewport = data;

   PDB("move start");
   _e_devicemgr_viewport_apply(viewport->topmost);
   PDB("move end");
}

static Eina_Bool
_e_devicemgr_viewport_cb_topmost_rotate(void *data, int type, void *event)
{
   E_Viewport *viewport = data;
   E_Event_Client *ev = event;

   if (viewport->topmost != ev->ec)
      return ECORE_CALLBACK_PASS_ON;

   PDB("rorate start");
   _e_devicemgr_viewport_apply(viewport->topmost);
   PDB("rorate end");

   return ECORE_CALLBACK_PASS_ON;
}

Eina_Bool
e_devicemgr_viewport_create(struct wl_resource *resource,
                            uint32_t id,
                            struct wl_resource *surface)
{
   E_Client *ec = wl_resource_get_user_data(surface);
   int version = wl_resource_get_version(resource);
   struct wl_client *client;
   struct wl_resource *res;
   E_Viewport *viewport;

   if (!ec || !ec->comp_data || e_object_is_del(E_OBJECT(ec)) || !ec->comp_data->surface)
     {
        ERR("wrong resource %d", wl_resource_get_id(surface));
        return EINA_FALSE;
     }

   if (ec->comp_data->scaler.viewport)
     {
        ERR("wl_surface@%d already has a viewport",
            wl_resource_get_id(surface));
        return EINA_FALSE;
     }

   if (!(client = wl_resource_get_client(surface)))
     {
        ERR("Could not get client from wl_surface@%d",
            wl_resource_get_id(surface));
        return EINA_FALSE;
     }

   viewport = calloc(1, sizeof *viewport);
   if (!viewport)
     {
        ERR("failed to alloc a viewport");
        return EINA_FALSE;
     }

   res = wl_resource_create(client, &tizen_viewport_interface, version, id);
   if (!res)
     {
        ERR("Could not create tizen_viewport_interface resource");
        free(viewport);
        return EINA_FALSE;
     }

   viewport->resource = res;
   viewport->ec = ec;
   viewport->epc = (ec->comp_data->sub.data) ? ec->comp_data->sub.data->parent : NULL;
   viewport->topmost = _topmost_parent_get(ec);
   viewport->window = e_client_util_win_get(ec);

   viewport->topmost_rotate_hdl =
     ecore_event_handler_add(E_EVENT_CLIENT_ROTATION_CHANGE_END,
                             _e_devicemgr_viewport_cb_topmost_rotate, viewport);

   evas_object_event_callback_add(viewport->ec->frame, EVAS_CALLBACK_RESIZE,
                                  _e_devicemgr_viewport_cb_resize, viewport);
   evas_object_event_callback_add(viewport->ec->frame, EVAS_CALLBACK_MOVE,
                                  _e_devicemgr_viewport_cb_move, viewport);

   evas_object_event_callback_add(viewport->topmost->frame, EVAS_CALLBACK_SHOW,
                                  _e_devicemgr_viewport_cb_topmost_show, viewport);
   evas_object_event_callback_add(viewport->topmost->frame, EVAS_CALLBACK_RESIZE,
                                  _e_devicemgr_viewport_cb_topmost_resize, viewport);
   evas_object_event_callback_add(viewport->topmost->frame, EVAS_CALLBACK_MOVE,
                                  _e_devicemgr_viewport_cb_topmost_move, viewport);

   viewport->source.w = -1;
   viewport->cropped_source.w = -1;

   viewport->surface_apply_viewport_listener.notify = _e_devicemgr_viewport_cb_apply_viewport;
   wl_signal_add(&ec->comp_data->apply_viewport_signal, &viewport->surface_apply_viewport_listener);

   /* Use scaler variable because tizen_viewport is the alternative of wl_viewport */
   ec->comp_data->scaler.viewport = res;
   wl_resource_set_implementation(res, &_e_devicemgr_viewport_interface,
                                  viewport, _e_devicemgr_viewport_destroy);

   viewport->surface_destroy_listener.notify = _e_devicemgr_viewport_cb_surface_destroy;
   wl_resource_add_destroy_listener(ec->comp_data->surface, &viewport->surface_destroy_listener);

   PIN("tizen_viewport@%d ec(%p) epc(%p) created", id, viewport->ec, viewport->epc);

   return EINA_TRUE;
}

static E_Viewport*
_e_devicemgr_viewport_get_viewport(struct wl_resource *resource)
{
   if (!resource)
     return NULL;

   if (wl_resource_instance_of(resource, &tizen_viewport_interface, &_e_devicemgr_viewport_interface))
     return wl_resource_get_user_data(resource);

   return NULL;
}

static void
_e_devicemgr_viewport_print(void *data, const char *log_path)
{
   FILE *log_fl;
   Evas_Object *o;
   const char *dest_type_str[] = { "None", "Rect", "Ratio", "Mode" };
   const char *mode_str[] = { "None", "LetterBox", "Origin", "Full", "Cropped_Full", "Origin_or_Letter" };
   int first_line = 1;
   log_fl = fopen(log_path, "a");
   if (!log_fl)
     {
        ERR("failed: open file(%s)", log_path);
        return;
     }

   setvbuf(log_fl, NULL, _IOLBF, 512);

   // append clients.
   for (o = evas_object_top_get(e_comp->evas); o; o = evas_object_below_get(o))
     {
        E_Client *ec;
        E_Viewport *viewport;
        Ecore_Window win = 0;
        const char *name = NULL;

        ec = evas_object_data_get(o, "E_Client");
        if (!ec || e_object_is_del(E_OBJECT(ec)) || !ec->comp_data) continue;

        viewport = _e_devicemgr_viewport_get_viewport(ec->comp_data->scaler.viewport);
        if (!viewport) continue;

        if (first_line)
          {
             fprintf(log_fl, "\n[ viewport information ]\n");
             first_line = 0;
          }

        win = e_client_util_win_get(ec);
        name = e_client_util_name_get(ec);
        if (!name)
          name = "NO NAME";

        fprintf(log_fl, "* WinID: 0x%08"PRIxPTR" '%s'", win, name);
        if (viewport->epc)
          {
             win = e_client_util_win_get(viewport->epc);
             fprintf(log_fl, " (parentID: 0x%08"PRIxPTR")\n", win);
          }
        else
          fprintf(log_fl, "\n");
        if (viewport->transform > 0)
          fprintf(log_fl, "\t  transform: %d%s\n",
                  (viewport->transform & 0x3) * 90 % 360,
                  (viewport->transform & 0x4) ? "(flipped)" : "");
        if (viewport->follow_parent_transform)
          fprintf(log_fl, "\t     follow: parent's transform\n");
        if (viewport->source.w != -1)
          fprintf(log_fl, "\t     source: %dx%d+%d+%d\n",
                  viewport->source.w, viewport->source.h,
                  viewport->source.x, viewport->source.y);
        fprintf(log_fl, "\t       type: %s\n", dest_type_str[viewport->type]);
        if (viewport->type == DESTINATION_TYPE_RECT)
          fprintf(log_fl, "\t  dest_rect: %dx%d+%d+%d\n",
                  viewport->destination.rect.w, viewport->destination.rect.h,
                  viewport->destination.rect.x, viewport->destination.rect.y);
        else if (viewport->type == DESTINATION_TYPE_RATIO)
          fprintf(log_fl, "\t dest_ratio: %.2fx%.2f+%.2f+%.2f\n",
                  viewport->destination.ratio.w, viewport->destination.ratio.h,
                  viewport->destination.ratio.x, viewport->destination.ratio.y);
        else if (viewport->type == DESTINATION_TYPE_MODE)
          {
             fprintf(log_fl, "\t       mode: %s\n",
                     mode_str[viewport->destination.mode.type]);
             if (viewport->destination.mode.ratio_h != -1.0)
               fprintf(log_fl, "\t\t    ratio: H(%.2f) V(%.2f)\n",
                       viewport->destination.mode.ratio_h,
                       viewport->destination.mode.ratio_v);
             if (viewport->destination.mode.scale_h != -1.0)
               fprintf(log_fl, "\t\t    scale: H(%.2f) V(%.2f)\n",
                       viewport->destination.mode.scale_h,
                       viewport->destination.mode.scale_v);
             if (viewport->destination.mode.align_h != -1.0)
               fprintf(log_fl, "\t\t    align: H(%.2f) V(%.2f)\n",
                       viewport->destination.mode.align_h,
                       viewport->destination.mode.align_v);
             if (viewport->destination.mode.offset_w != 0 ||
                 viewport->destination.mode.offset_h != 0 ||
                 viewport->destination.mode.offset_x != 0 ||
                 viewport->destination.mode.offset_y != 0)
               fprintf(log_fl, "\t\t   offset: W(%d) H(%d) (%d) Y(%d)\n",
                       viewport->destination.mode.offset_w, viewport->destination.mode.offset_h,
                       viewport->destination.mode.offset_x, viewport->destination.mode.offset_y);
          }

        fprintf(log_fl, "\n");
     }

   fflush(log_fl);
   fclose(log_fl);
}


int
e_devicemgr_viewport_init(void)
{
   if (!e_comp_wl) return 0;
   if (!e_comp_wl->wl.disp) return 0;

   e_info_server_hook_set("viewport", _e_devicemgr_viewport_print, NULL);
   _log_dom_viewport = eina_log_domain_register("e-devicemgr-viewport", EINA_COLOR_BLUE);
   if (_log_dom_viewport < 0)
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed @ eina_log_domain_register()..!\n", __FUNCTION__);
        return 0;
     }

   return 1;
}

void
e_devicemgr_viewport_fini(void)
{
   e_info_server_hook_set("viewport", NULL, NULL);
   eina_log_domain_unregister(_log_dom_viewport);
}
