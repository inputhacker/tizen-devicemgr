#include "e_devicemgr_viewport.h"
#include "e_devicemgr_buffer.h"

#define PER(fmt,arg...)   ERR("window(0x%08"PRIxPTR") ec(%p) epc(%p): "fmt, \
   viewport->window, viewport->ec, viewport->epc, ##arg)
#define PWR(fmt,arg...)   WRN("window(0x%08"PRIxPTR") ec(%p) epc(%p): "fmt, \
   viewport->window, viewport->ec, viewport->epc, ##arg)
#define PIN(fmt,arg...)   INF("window(0x%08"PRIxPTR") ec(%p) epc(%p): "fmt, \
   viewport->window, viewport->ec, viewport->epc, ##arg)
#define PDB(fmt,arg...)   DBG("window(0x%08"PRIxPTR") ec(%p) epc(%p): "fmt, \
   viewport->window, viewport->ec, viewport->epc, ##arg)

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

   E_Client_Hook *client_hook_del;
   E_Client_Hook *client_hook_move;
   E_Client_Hook *client_hook_resize;

   E_Comp_Wl_Hook *subsurf_hook_create;
} E_Viewport;

static E_Viewport* _e_devicemgr_viewport_get_viewport(struct wl_resource *resource);
static void _e_devicemgr_viewport_cb_parent_resize(void *data, Evas *e, Evas_Object *obj, void *event_info);
static void _e_devicemgr_viewport_parent_check(E_Viewport *viewport);

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

   if (viewport->epc && viewport->query_parent_size)
     {
        evas_object_event_callback_del_full(viewport->epc->frame, EVAS_CALLBACK_RESIZE,
                                            _e_devicemgr_viewport_cb_parent_resize, viewport);
        viewport->epc = NULL;
     }

   e_client_hook_del(viewport->client_hook_del);
   e_client_hook_del(viewport->client_hook_move);
   e_client_hook_del(viewport->client_hook_resize);

   e_comp_wl_hook_del(viewport->subsurf_hook_create);

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

   PIN("tizen_viewport@%d destroy. viewport(%p)", wl_resource_get_id(viewport->resource), viewport);

   free(viewport);
}

static void
_subsurface_cb_create(void *data, E_Client *ec)
{
   E_Viewport *viewport = data;

   if (EINA_UNLIKELY(!ec)) return;
   if (viewport->ec != ec) return;

   _e_devicemgr_viewport_parent_check(viewport);
}

static void
_client_cb_del(void *data, E_Client *ec)
{
   E_Viewport *viewport = data;

   if (viewport->epc != ec) return;

   evas_object_event_callback_del(viewport->epc->frame, EVAS_CALLBACK_RESIZE,
                                  _e_devicemgr_viewport_cb_parent_resize);
   PIN("epc del");
   viewport->epc = NULL;
}

static void
_client_cb_move(void *data, E_Client *ec)
{
   E_Viewport *viewport = data;
   E_Client *topmost = _topmost_parent_get(ec);

   if (ec != topmost && ec != viewport->epc) return;

   PDB("move start: topmost(%p)", topmost);
   e_devicemgr_viewport_apply(topmost);
   PDB("move end");
}

static void
_client_cb_resize(void *data, E_Client *ec)
{
   E_Viewport *viewport = data;
   E_Client *topmost = _topmost_parent_get(ec);

   if (ec != topmost && ec != viewport->epc) return;

   PDB("resize start: topmost(%p)", topmost);
   e_devicemgr_viewport_apply(topmost);
   PDB("resize end");
}

static void
_e_devicemgr_viewport_parent_check(E_Viewport *viewport)
{
   E_Client *ec = viewport->ec;
   E_Client *new_parent;

   if (e_object_is_del(E_OBJECT(ec)) || !ec->comp_data) return;

   new_parent = (ec->comp_data->sub.data) ? ec->comp_data->sub.data->parent : NULL;

   if (viewport->epc == new_parent) return;

   if (viewport->epc)
      evas_object_event_callback_del(viewport->epc->frame, EVAS_CALLBACK_RESIZE,
                                     _e_devicemgr_viewport_cb_parent_resize);

   viewport->epc = new_parent;

   PIN("epc(%p)", viewport->epc);

   if (!viewport->epc) return;

   if (viewport->query_parent_size)
     evas_object_event_callback_add(viewport->epc->frame, EVAS_CALLBACK_RESIZE,
                                    _e_devicemgr_viewport_cb_parent_resize, viewport);
}

static void
_e_devicemgr_viewport_set_changed(E_Viewport *viewport)
{
   E_Client *ec;
   E_Client *subc;
   Eina_List *l;
   E_Viewport *sub_viewport;

   if (!viewport) return;

   viewport->changed = EINA_TRUE;

   ec = viewport->ec;
   EINA_LIST_FOREACH(ec->comp_data->sub.list, l, subc)
     {
        if (!subc || !subc->comp_data || e_object_is_del(E_OBJECT(subc)))
          continue;

        sub_viewport = _e_devicemgr_viewport_get_viewport(subc->comp_data->scaler.viewport);
        _e_devicemgr_viewport_set_changed(sub_viewport);
     }

   EINA_LIST_FOREACH(ec->comp_data->sub.below_list, l, subc)
     {
        if (!subc || !subc->comp_data || e_object_is_del(E_OBJECT(subc)))
          continue;

        sub_viewport = _e_devicemgr_viewport_get_viewport(subc->comp_data->scaler.viewport);
        _e_devicemgr_viewport_set_changed(sub_viewport);
     }
}

static void
_e_devicemgr_destination_mode_destroy(struct wl_resource *resource)
{
   E_Viewport *viewport = wl_resource_get_user_data(resource);

   if (!viewport) return;

   if (viewport->type == DESTINATION_TYPE_MODE)
     viewport->type = DESTINATION_TYPE_NONE;

   _e_devicemgr_viewport_set_changed(viewport);

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

   PIN("follow_parent_transform");

   viewport->follow_parent_transform = EINA_TRUE;
   _e_devicemgr_viewport_set_changed(viewport);
}

static void
_e_devicemgr_destination_mode_cb_unfollow_parent_transform(struct wl_client *client EINA_UNUSED,
                                                           struct wl_resource *resource)
{
   E_Viewport *viewport = wl_resource_get_user_data(resource);

   if (!viewport) return;

   if (!viewport->follow_parent_transform)
     return;

   PIN("unfollow_parent_transform");

   viewport->follow_parent_transform = EINA_FALSE;
   _e_devicemgr_viewport_set_changed(viewport);
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

   PIN("type(%d)", type);

   viewport->destination.mode.type = type;
   _e_devicemgr_viewport_set_changed(viewport);
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
        _e_devicemgr_viewport_set_changed(viewport);
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

   PIN("ratio_h(%.2f) ratio_v(%.2f)", ratio_h, ratio_v);

   viewport->destination.mode.ratio_h = ratio_h;
   viewport->destination.mode.ratio_v = ratio_v;
   _e_devicemgr_viewport_set_changed(viewport);
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
        _e_devicemgr_viewport_set_changed(viewport);
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

   PIN("scale_h(%.2f) scale_v(%.2f)", scale_h, scale_v);

   viewport->destination.mode.scale_h = scale_h;
   viewport->destination.mode.scale_v = scale_v;
   _e_devicemgr_viewport_set_changed(viewport);
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
        _e_devicemgr_viewport_set_changed(viewport);
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

   PIN("align_h(%.2f) align_v(%.2f)", align_h, align_v);

   viewport->destination.mode.align_h = align_h;
   viewport->destination.mode.align_v = align_v;
   _e_devicemgr_viewport_set_changed(viewport);
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

   PIN("offset_x(%d) offset_y(%d) offset_w(%d) offset_h(%d)", x, y, w, h);

   viewport->destination.mode.offset_x = x;
   viewport->destination.mode.offset_y = y;
   viewport->destination.mode.offset_w = w;
   viewport->destination.mode.offset_h = h;
   _e_devicemgr_viewport_set_changed(viewport);
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

   _e_devicemgr_viewport_parent_check(viewport);

   if (viewport->transform == transform)
     return;

   PIN("transform(%d)", transform);

   viewport->transform = transform;
   _e_devicemgr_viewport_set_changed(viewport);
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

   _e_devicemgr_viewport_parent_check(viewport);

   if (viewport->source.x == x && viewport->source.y == y &&
       viewport->source.w == width && viewport->source.h == height)
     return;

   viewport->source.x = x;
   viewport->source.y = y;
   viewport->source.w = width;
   viewport->source.h = height;
   viewport->cropped_source = viewport->source;
   _e_devicemgr_viewport_set_changed(viewport);

   PIN("source(%d,%d %dx%d)", EINA_RECTANGLE_ARGS(&viewport->source));
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

   _e_devicemgr_viewport_parent_check(viewport);

   viewport->type = DESTINATION_TYPE_RECT;

   if (viewport->destination.rect.x == x && viewport->destination.rect.y == y &&
       viewport->destination.rect.w == width && viewport->destination.rect.h == height)
     return;

   PIN("destination.rect(%d,%d %dx%d)", x, y, width, height);

   viewport->destination.rect.x = x;
   viewport->destination.rect.y = y;
   viewport->destination.rect.w = width;
   viewport->destination.rect.h = height;
   _e_devicemgr_viewport_set_changed(viewport);
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

   _e_devicemgr_viewport_parent_check(viewport);

   ratio_x = (viewport->epc) ? wl_fixed_to_double(x) : 0;
   ratio_y = (viewport->epc) ? wl_fixed_to_double(y) : 0;
   ratio_w = wl_fixed_to_double(width);
   ratio_h = wl_fixed_to_double(height);

   if (ratio_x < 0 || ratio_x >= 1 || ratio_y < 0 || ratio_y >= 1 || ratio_w <= 0 || ratio_h <= 0)
     {
        PER("invalid param: destination.ratio(%.2f,%.2f %.2fx%.2f)", ratio_x, ratio_y, ratio_w, ratio_h);
        return;
     }

   viewport->type = DESTINATION_TYPE_RATIO;

   if (viewport->destination.ratio.x == ratio_x && viewport->destination.ratio.y == ratio_y &&
       viewport->destination.ratio.w == ratio_w && viewport->destination.ratio.h == ratio_h)
     return;

   PIN("destination.ratio(%.2f,%.2f %.2fx%.2f)", ratio_x, ratio_y, ratio_w, ratio_h);

   viewport->destination.ratio.x = ratio_x;
   viewport->destination.ratio.y = ratio_y;
   viewport->destination.ratio.w = ratio_w;
   viewport->destination.ratio.h = ratio_h;
   _e_devicemgr_viewport_set_changed(viewport);
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

   _e_devicemgr_viewport_parent_check(viewport);

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

   _e_devicemgr_viewport_parent_check(viewport);

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

   _e_devicemgr_viewport_parent_check(viewport);

   if (viewport->follow_parent_transform)
     return;

   PIN("follow_parent_transform");

   viewport->follow_parent_transform = EINA_TRUE;
   _e_devicemgr_viewport_set_changed(viewport);
}

static void
_e_devicemgr_viewport_cb_unfollow_parent_transform(struct wl_client *client EINA_UNUSED,
                                                   struct wl_resource *resource)
{
   E_Viewport *viewport = wl_resource_get_user_data(resource);

   if (!viewport) return;

   _e_devicemgr_viewport_parent_check(viewport);

   if (!viewport->follow_parent_transform)
     return;

   PIN("unfollow_parent_transform");

   viewport->follow_parent_transform = EINA_FALSE;
   _e_devicemgr_viewport_set_changed(viewport);
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

/* we have to consider the output transform. if epc is toplevel, and if
 * output transform is 3, and if vpp->buffer.transform is 3, then actual
 * epc's transform is 0.
 */
static int
_get_parent_transform(E_Viewport *viewport)
{
   E_Client *epc = viewport->epc;
   E_Client *topmost;
   unsigned int ptran, pflip;
   int ptransform;

   if (!epc->comp_data || e_object_is_del(E_OBJECT(epc)))
     return 0;

   ptransform = e_comp_wl_output_buffer_transform_get(epc);

   topmost = _topmost_parent_get(epc);

   if (ptransform != 0 && epc == topmost)
     {
        E_Comp_Wl_Output *output = e_comp_wl_output_find(topmost);

        EINA_SAFETY_ON_NULL_RETURN_VAL(output, 0);

        ptran = ptransform & 0x3;
        pflip = ptransform & 0x4;
        ptransform = pflip + (4 + ptran - output->transform) % 4;
     }

   return ptransform;
}

static Eina_Bool
_destination_mode_calculate_destination(E_Viewport *viewport, Eina_Rectangle *prect, Eina_Rectangle *rect)
{
   E_Client *ec = viewport->ec;
   int sw = 0, sh = 0, transform;
   double rh = -1.0, rv = -1.0;

   if (viewport->source.w != -1)
     {
        sw = viewport->source.w;
        sh = viewport->source.h;
     }
   else
     e_devmgr_buffer_size_get(ec, &sw, &sh);

   transform = e_comp_wl_output_buffer_transform_get(ec);

   if (transform % 2)
      SWAP(sw, sh);

   PDB("parent(%dx%d) src(%dx%d)", prect->w, prect->h, sw, sh);

   /* ratio -> type -> scale -> offset -> align */
   if (viewport->destination.mode.ratio_h != -1.0)
     {
        if (transform % 2)
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

        if (transform % 2)
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
             int ptransform;

             if (!epc->comp_data || e_object_is_del(E_OBJECT(epc)))
               return EINA_FALSE;

             ptransform = e_comp_wl_output_buffer_transform_get(epc);

             PDB("parent's transform(%d)", ptransform);

             switch (ptransform)
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

        if (transform % 2)
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
        int ptransform = _get_parent_transform(viewport);

        if (ptransform > 0)
          {
             if (ptransform % 2)
               _source_transform_to_surface(prect->h, prect->w, ptransform, 1, rect, rect);
             else
               _source_transform_to_surface(prect->w, prect->h, ptransform, 1, rect, rect);
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
     {
        if (viewport->source.w == -1)
          {
             e_devmgr_buffer_size_get(viewport->ec, &bw, &bh);

             viewport->cropped_source.x = viewport->cropped_source.y = 0;
             viewport->cropped_source.w = bw;
             viewport->cropped_source.h = bh;
          }
        else
          viewport->cropped_source = viewport->source;

        PDB("src(%d,%d %dx%d)", EINA_RECTANGLE_ARGS(&viewport->cropped_source));

        return;
     }

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

   e_devmgr_buffer_size_get(viewport->ec, &bw, &bh);

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

   _source_transform_to_surface(bw, bh,
                                e_comp_wl_output_buffer_transform_get(viewport->ec), 1,
                                &viewport->cropped_source, &viewport->cropped_source);

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
        unsigned int pwtran, ptran, pflip, ctran, cflip;
        int ptransform;

        if (!epc->comp_data || e_object_is_del(E_OBJECT(epc)))
          {
             *rtransform = vp->buffer.transform;
             return EINA_FALSE;
          }

        ptransform = _get_parent_transform(viewport);
        PDB("parent's transform(%d) rot.ang.curr(%d)", ptransform, epc->e.state.rot.ang.curr/90);

        pwtran = ((epc->e.state.rot.ang.curr + 360) % 360) / 90;

        ptran = ((pwtran & 0x3) + (ptransform & 0x3)) & 0x3;
        pflip = (ptransform & 0x4);

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

   if (changed)
     PIN("apply transform: %d type(%d) follow(%d) changed(%d)",
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
          e_devmgr_buffer_transform_scale_size_get(epc, &prect.w, &prect.h);
     }

   if (!(prect.w > 0 && prect.h > 0))
     {
        PWR("prect.w > 0 && prect.h > 0 is false");
        return EINA_FALSE;
     }

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

   if (changed)
     PIN("apply destination: %d,%d %dx%d changed(%d)", EINA_RECTANGLE_ARGS(&dst), changed);

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

   e_devmgr_buffer_size_get(ec, &bw, &bh);

   rect.w = bw;
   rect.h = bh;

   if (!eina_rectangle_intersection(&rect, &viewport->cropped_source))
     {
        PWR("source area is empty");
        return EINA_FALSE;
     }

   _source_transform_to_surface(bw, bh,
                                e_comp_wl_output_buffer_transform_get(ec), 1,
                                &rect, &rect);

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

   if (changed)
     PDB("apply source: %d,%d %dx%d orig(%d,%d %dx%d) changed(%d)",
         EINA_RECTANGLE_ARGS(&rect), EINA_RECTANGLE_ARGS(&viewport->cropped_source), changed);

   return changed;
}

Eina_Bool
e_devicemgr_viewport_apply(E_Client *ec)
{
   E_Viewport *viewport;
   E_Client *subc;
   Eina_List *l;

   if (!ec || !ec->comp_data || e_object_is_del(E_OBJECT(ec)))
     return EINA_FALSE;

   viewport = _e_devicemgr_viewport_get_viewport(ec->comp_data->scaler.viewport);

   if (viewport)
     _e_devicemgr_viewport_parent_check(viewport);

   if (viewport && ec->comp_data->buffer_ref.buffer)
     {
        E_Comp_Wl_Buffer_Viewport *vp = &ec->comp_data->scaler.buffer_viewport;
        Eina_Bool changed = EINA_FALSE, src_changed = EINA_FALSE;
        Eina_Rectangle rrect = {0,};
        int rtransform = 0;

        /* evas map follow screen coordinates. so all information including the
         * transform and destination also should follow screen coordinates.
         */
        changed |= _e_devicemgr_viewport_apply_transform(viewport, &rtransform);
        changed |= _e_devicemgr_viewport_apply_destination(viewport, &rrect);
        src_changed |= _e_devicemgr_viewport_apply_source(viewport);

        viewport->changed = EINA_FALSE;

        PDB("changed(%d) src_changed(%d)", changed, src_changed);

        if (changed || src_changed)
          {
             e_comp_wl_map_size_cal_from_buffer(viewport->ec);
             e_comp_wl_map_size_cal_from_viewport(viewport->ec);
             e_comp_wl_map_apply(viewport->ec);

             if (changed)
               {
                  PIN("send destination_changed: transform(%d) x(%d) y(%d) w(%d) h(%d)",
                      rtransform, rrect.x, rrect.y, rrect.w, rrect.h);
                  tizen_viewport_send_destination_changed(viewport->resource, rtransform,
                                                          rrect.x, rrect.y, rrect.w, rrect.h);
               }

            vp->changed = EINA_TRUE;
          }
     }
   else if (viewport)
     PDB("%p buffer", ec->comp_data->buffer_ref.buffer);

   EINA_LIST_FOREACH(ec->comp_data->sub.list, l, subc)
     e_devicemgr_viewport_apply(subc);

   EINA_LIST_FOREACH(ec->comp_data->sub.below_list, l, subc)
     e_devicemgr_viewport_apply(subc);

   return EINA_TRUE;
}

Eina_Bool
e_devicemgr_viewport_is_changed(E_Client *ec)
{
   E_Viewport *viewport;
   E_Client *subc;
   Eina_List *l;

   if (!ec || !ec->comp_data || e_object_is_del(E_OBJECT(ec)))
     return EINA_FALSE;

   viewport = _e_devicemgr_viewport_get_viewport(ec->comp_data->scaler.viewport);
   if(viewport && viewport->changed)
      return EINA_TRUE;

   EINA_LIST_FOREACH(ec->comp_data->sub.list, l, subc)
     if (e_devicemgr_viewport_is_changed(subc))
       return EINA_TRUE;

   EINA_LIST_FOREACH(ec->comp_data->sub.below_list, l, subc)
     if (e_devicemgr_viewport_is_changed(subc))
       return EINA_TRUE;

   return EINA_FALSE;
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
   E_Client *topmost = _topmost_parent_get(ec);
   E_Comp_Wl_Buffer_Viewport *vp = &ec->comp_data->scaler.buffer_viewport;

   if (vp->changed)
     _e_devicemgr_viewport_set_changed(viewport);

   _e_devicemgr_viewport_parent_check(viewport);

   if (!viewport->changed) return;
   if (!ec->comp_data->buffer_ref.buffer) return;
   if (viewport->epc && !viewport->epc->comp_data->buffer_ref.buffer) return;

   PDB("apply: topmost(%p)", topmost);

   if (!e_devicemgr_viewport_apply(topmost))
     {
        PER("failed to apply tizen_viewport");
        return;
     }
}

static Eina_Bool
_e_devicemgr_viewport_cb_topmost_rotate(void *data, int type, void *event)
{
   E_Viewport *viewport = data;
   E_Client *ec = viewport->ec;
   E_Client *topmost = _topmost_parent_get(ec);
   E_Event_Client *ev = event;

   if (topmost != ev->ec)
      return ECORE_CALLBACK_PASS_ON;

   PDB("rorate start: topmost(%p)", topmost);
   e_devicemgr_viewport_apply(topmost);
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
   viewport->window = e_client_util_win_get(ec);

   _e_devicemgr_viewport_parent_check(viewport);

   viewport->client_hook_del = e_client_hook_add(E_CLIENT_HOOK_DEL, _client_cb_del, viewport);
   viewport->client_hook_move = e_client_hook_add(E_CLIENT_HOOK_MOVE_UPDATE, _client_cb_move, viewport);
   viewport->client_hook_resize = e_client_hook_add(E_CLIENT_HOOK_RESIZE_UPDATE, _client_cb_resize, viewport);

   viewport->subsurf_hook_create = e_comp_wl_hook_add(E_COMP_WL_HOOK_SUBSURFACE_CREATE, _subsurface_cb_create, viewport);

   viewport->topmost_rotate_hdl =
     ecore_event_handler_add(E_EVENT_CLIENT_ROTATION_CHANGE_END,
                             _e_devicemgr_viewport_cb_topmost_rotate, viewport);

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

   PIN("tizen_viewport@%d viewport(%p) created", id, viewport);

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

   return 1;
}

void
e_devicemgr_viewport_fini(void)
{
   e_info_server_hook_set("viewport", NULL, NULL);
}
