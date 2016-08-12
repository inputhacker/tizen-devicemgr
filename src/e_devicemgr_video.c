#include <tdm.h>
#include <values.h>
#include "e_devicemgr_video.h"
#include "e_devicemgr_dpms.h"
#include "e_devicemgr_viewport.h"

//#define DUMP_BUFFER

static int _video_detail_log_dom = -1;

#define BUFFER_MAX_COUNT   3
#define MIN_WIDTH   32

#define VER(fmt,arg...)   ERR("window(0x%08"PRIxPTR"): "fmt, video->window, ##arg)
#define VWR(fmt,arg...)   WRN("window(0x%08"PRIxPTR"): "fmt, video->window, ##arg)
#define VIN(fmt,arg...)   INF("window(0x%08"PRIxPTR"): "fmt, video->window, ##arg)
#define VDB(fmt,arg...)   DBG("window(0x%08"PRIxPTR"): "fmt, video->window, ##arg)

#define DET(...)          EINA_LOG_DOM_DBG(_video_detail_log_dom, __VA_ARGS__)
#define VDT(fmt,arg...)   DET("window(0x%08"PRIxPTR"): "fmt, video->window, ##arg)

#define GEO_FMT   "%d,%d (%d,%d %dx%d) ~ (%d,%d %dx%d) tran(%d)"
#define GEO_ARG(g) \
   (g)->input_w, (g)->input_h, \
   (g)->input_r.x, (g)->input_r.y, (g)->input_r.w, (g)->input_r.h, \
   (g)->output_r.x, (g)->output_r.y, (g)->output_r.w, (g)->output_r.h, \
   (g)->transform

typedef struct _E_Video_Fb
{
   E_Devmgr_Buf *mbuf;
   Eina_Rectangle visible_r;        /* frame buffer's visible rect */
   unsigned int transform;

   E_Video *video;

   /* in case of non-converting */
   E_Comp_Wl_Buffer_Ref buffer_ref;
} E_Video_Fb;

struct _E_Video
{
   struct wl_resource *video_object;
   struct wl_resource *surface;
   E_Client *ec;
   Ecore_Window window;
   Ecore_Drm_Output *drm_output;
   tdm_output *output;
   tdm_layer *layer;

   /* input info */
   tbm_format tbmfmt;
   Eina_List *input_buffer_list;

   struct
     {
        int input_w, input_h;    /* input buffer's size */
        Eina_Rectangle input_r;  /* input buffer's content rect */
        Eina_Rectangle output_r; /* video plane rect */
        uint transform;          /* rotate, flip */
     } geo, old_geo;

   E_Comp_Wl_Buffer *old_comp_buffer;

   /* converter info */
   tbm_format pp_tbmfmt;
   tdm_pp *pp;
   Eina_Rectangle pp_r;    /* converter dst content rect */
   Eina_List *pp_buffer_list;
   Eina_List *next_buffer;

   int output_align;
   int pp_align;
   int video_align;

   /* vblank handling */
   Eina_Bool   wait_vblank;
   Eina_List  *waiting_list;
   E_Video_Fb *current_fb;

   Eina_Bool  need_punch;
   Eina_Bool  cb_registered;
   Eina_Rectangle eo_geo;
};

static Eina_List *video_list;

static void _e_video_set(E_Video *video, E_Client *ec);
static void _e_video_destroy(E_Video *video);
static void _e_video_render(E_Video *video, const char *func);
static Eina_Bool _e_video_frame_buffer_show(E_Video *video, E_Video_Fb *vfb);
static void _e_video_frame_buffer_destroy(E_Video_Fb *vfb);
static void _e_video_buffer_show(E_Video *video, E_Devmgr_Buf *mbuf, Eina_Rectangle *visible, unsigned int transform);
static void _e_video_input_buffer_cb_release(tbm_surface_h surface, void *user_data);
static void _e_video_pp_buffer_cb_release(tbm_surface_h surface, void *user_data);

static int
gcd(int a, int b)
{
   if (a % b == 0)
      return b;
   return gcd(b, a % b);
}

static int
lcm(int a, int b)
{
    return a * b / gcd(a, b);
}

static void
buffer_transform(int width, int height, uint32_t transform, int32_t scale,
                 int sx, int sy, int *dx, int *dy)
{
   switch (transform)
     {
      case WL_OUTPUT_TRANSFORM_NORMAL:
      default:
        *dx = sx, *dy = sy;
        break;
      case WL_OUTPUT_TRANSFORM_FLIPPED:
        *dx = width - sx, *dy = sy;
        break;
      case WL_OUTPUT_TRANSFORM_90:
        *dx = height - sy, *dy = sx;
        break;
      case WL_OUTPUT_TRANSFORM_FLIPPED_90:
        *dx = height - sy, *dy = width - sx;
        break;
      case WL_OUTPUT_TRANSFORM_180:
        *dx = width - sx, *dy = height - sy;
        break;
      case WL_OUTPUT_TRANSFORM_FLIPPED_180:
        *dx = sx, *dy = height - sy;
        break;
      case WL_OUTPUT_TRANSFORM_270:
        *dx = sy, *dy = width - sx;
        break;
      case WL_OUTPUT_TRANSFORM_FLIPPED_270:
        *dx = sy, *dy = sx;
        break;
     }

   *dx *= scale;
   *dy *= scale;
}

static E_Video*
find_video_with_surface(struct wl_resource *surface)
{
   E_Video *video;
   Eina_List *l;
   EINA_LIST_FOREACH(video_list, l, video)
     {
        if (video->surface == surface)
          return video;
     }
   return NULL;
}

static E_Client*
find_topmost_parent_get(E_Client *ec)
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
_e_video_input_buffer_cb_free(E_Devmgr_Buf *mbuf, void *data)
{
   E_Video *video = data;
   E_Video_Fb *vfb;
   Eina_List *l, *ll;

   VDT("Buffer(%p) to be free, refcnt(%d)", mbuf, mbuf->ref_cnt);
   EINA_SAFETY_ON_NULL_RETURN(mbuf->comp_buffer);

   video->input_buffer_list = eina_list_remove(video->input_buffer_list, mbuf);

   e_comp_wl_buffer_reference(&mbuf->buffer_ref, NULL);

   /* if pp exists, it means that input buffer is not a frame buffer. */
   if (video->pp)
     return;

   /* if current fb is destroyed */
   if (video->current_fb && video->current_fb->mbuf == mbuf)
     {
        _e_video_frame_buffer_show(video, NULL);
        _e_video_frame_buffer_destroy(video->current_fb);
        video->current_fb = NULL;
        VDB("current fb destroyed");
        return;
     }

   /* if waiting fb is destroyed */
   EINA_LIST_FOREACH_SAFE(video->waiting_list, l, ll, vfb)
     if (vfb->mbuf == mbuf)
       {
           video->waiting_list = eina_list_remove(video->waiting_list, vfb);
          _e_video_frame_buffer_destroy(vfb);
          return;
       }
}

static E_Devmgr_Buf*
_e_video_input_buffer_get(E_Video *video, E_Comp_Wl_Buffer *comp_buffer, Eina_Bool scanout)
{
   E_Devmgr_Buf *mbuf;

   mbuf = e_devmgr_buffer_create(comp_buffer->resource);
   EINA_SAFETY_ON_NULL_RETURN_VAL(mbuf, NULL);

   if (video->pp)
     {
        if (video->pp_align != -1 && (mbuf->width_from_pitch % video->pp_align))
          {
             E_Devmgr_Buf *temp, *temp2;
             int aligned_width = ROUNDUP(mbuf->width_from_pitch, video->pp_align);

             temp = e_devmgr_buffer_alloc(aligned_width, mbuf->height, mbuf->tbmfmt, scanout);
             if (!temp)
               {
                  e_devmgr_buffer_unref(mbuf);
                  return NULL;
               }

             VDB("copy mbuf(%d,%dx%d) => mbuf(%d,%dx%d)",
                 MSTAMP(mbuf), mbuf->width_from_pitch, mbuf->height,
                 MSTAMP(temp), temp->width, temp->height);

             e_devmgr_buffer_copy(mbuf, temp);
             temp2 = mbuf;
             mbuf = temp;
             e_devmgr_buffer_unref(temp2);

             video->geo.input_w = mbuf->width_from_pitch;
#ifdef DUMP_BUFFER
             static int i;
             e_devmgr_buffer_dump(mbuf, "copy", i++, 0);
#endif
          }
     }

   mbuf->comp_buffer = comp_buffer;
   e_comp_wl_buffer_reference(&mbuf->buffer_ref, mbuf->comp_buffer);

   video->input_buffer_list = eina_list_append(video->input_buffer_list, mbuf);
   e_devmgr_buffer_free_func_add(mbuf, _e_video_input_buffer_cb_free, video);

   VDT("Client(%s):PID(%d) RscID(%d), Buffer(%p) created, refcnt:%d"
       " scanout=%d", e_client_util_name_get(video->ec) ?: "No Name" ,
       video->ec->netwm.pid, wl_resource_get_id(video->surface), mbuf,
       mbuf->ref_cnt, scanout);

   return mbuf;
}

static void
_e_video_input_buffer_valid(E_Video *video, E_Comp_Wl_Buffer *comp_buffer)
{
   E_Devmgr_Buf *mbuf;
   Eina_List *l;

   EINA_LIST_FOREACH(video->input_buffer_list, l, mbuf)
     {
        tbm_surface_h tbm_surf;
        tbm_bo bo;
        uint32_t size = 0, offset = 0, pitch = 0;

        if (!mbuf->comp_buffer) continue;
        if (mbuf->resource == comp_buffer->resource)
          {
             WRN("got wl_buffer@%d twice", wl_resource_get_id(comp_buffer->resource));
             return;
          }

        tbm_surf = wayland_tbm_server_get_surface(e_comp->wl_comp_data->tbm.server, comp_buffer->resource);
        bo = tbm_surface_internal_get_bo(tbm_surf, 0);
        tbm_surface_internal_get_plane_data(tbm_surf, 0, &size, &offset, &pitch);

        if (mbuf->names[0] == tbm_bo_export(bo) && mbuf->offsets[0] == offset)
          {
             WRN("can tearing: wl_buffer@%d, wl_buffer@%d are same. gem_name(%d)",
                 wl_resource_get_id(mbuf->resource),
                 wl_resource_get_id(comp_buffer->resource), mbuf->names[0]);
             return;
          }
     }
}

static void
_e_video_pp_buffer_cb_free(E_Devmgr_Buf *mbuf, void *data)
{
   E_Video *video = data;

   video->pp_buffer_list = eina_list_remove(video->pp_buffer_list, mbuf);

   VDB("mbuf(%d) list_remove", MSTAMP(mbuf));
}

static E_Devmgr_Buf*
_e_video_pp_buffer_get(E_Video *video, int width, int height)
{
   E_Devmgr_Buf *mbuf;
   Eina_List *l;
   int i = 0;
   int aligned_width;

   if (video->video_align != -1)
     aligned_width = ROUNDUP(width, video->video_align);
   else
     aligned_width = width;

   if (video->pp_buffer_list)
     {
        mbuf = eina_list_data_get(video->pp_buffer_list);
        EINA_SAFETY_ON_NULL_RETURN_VAL(mbuf, NULL);

        /* if we need bigger pp_buffers, destroy all pp_buffers and create */
        if (aligned_width != mbuf->width_from_pitch || height != mbuf->height)
          {
             E_Video_Fb *vfb;
             Eina_List *ll;

             VIN("pp buffer changed: %dx%d => %dx%d",
                 mbuf->width_from_pitch, mbuf->height,
                 aligned_width, height);

             EINA_LIST_FOREACH_SAFE(video->pp_buffer_list, l, ll, mbuf)
               {
                  video->pp_buffer_list = eina_list_remove(video->pp_buffer_list, mbuf);
                  e_devmgr_buffer_unref(mbuf);
               }
             if (video->pp_buffer_list)
               NEVER_GET_HERE();

             EINA_LIST_FOREACH_SAFE(video->waiting_list, l, ll, vfb)
               {
                  video->waiting_list = eina_list_remove(video->waiting_list, vfb);
                  _e_video_frame_buffer_destroy(vfb);
               }
             if (video->waiting_list)
               NEVER_GET_HERE();
          }
     }

   if (!video->pp_buffer_list)
     {
        for (i = 0; i < BUFFER_MAX_COUNT; i++)
          {
             mbuf = e_devmgr_buffer_alloc(aligned_width, height, video->pp_tbmfmt, EINA_TRUE);
             EINA_SAFETY_ON_NULL_RETURN_VAL(mbuf, NULL);

             e_devmgr_buffer_free_func_add(mbuf, _e_video_pp_buffer_cb_free, video);
             video->pp_buffer_list = eina_list_append(video->pp_buffer_list, mbuf);
          }

        VIN("pp buffer created: %dx%d, %c%c%c%c",
            mbuf->width_from_pitch, height, FOURCC_STR(video->pp_tbmfmt));

        video->next_buffer = video->pp_buffer_list;
     }

   EINA_SAFETY_ON_NULL_RETURN_VAL(video->pp_buffer_list, NULL);
   EINA_SAFETY_ON_NULL_RETURN_VAL(video->next_buffer, NULL);

   l = video->next_buffer;
   while ((mbuf = video->next_buffer->data))
     {
        video->next_buffer = (video->next_buffer->next) ? video->next_buffer->next : video->pp_buffer_list;

        if (!mbuf->showing)
          return mbuf;

        if (l == video->next_buffer)
          {
             VWR("all video framebuffers in use (max:%d)", BUFFER_MAX_COUNT);
             return NULL;
          }
     }

   return NULL;
}

static Ecore_Drm_Output*
_e_video_drm_output_find(E_Client *ec)
{
   Ecore_Drm_Device *dev;
   Ecore_Drm_Output *output;
   Eina_List *devs;
   Eina_List *l, *ll;

   devs = eina_list_clone(ecore_drm_devices_get());
   EINA_LIST_FOREACH(devs, l, dev)
     EINA_LIST_FOREACH(dev->outputs, ll, output)
       {
          int x, y, w, h;
          ecore_drm_output_position_get(output, &x, &y);
          ecore_drm_output_current_resolution_get(output, &w, &h, NULL);
          if (x <= ec->x && y <= ec->y && ec->x < x + w && ec->y < y + h)
            {
               eina_list_free(devs);
               return output;
            }
       }
   eina_list_free(devs);
   return NULL;
}

static Eina_Bool
_e_video_geometry_cal_viewport(E_Video *video)
{
   E_Client *ec = video->ec;
   E_Comp_Wl_Buffer_Viewport *vp = &ec->comp_data->scaler.buffer_viewport;
   E_Comp_Wl_Subsurf_Data *sdata;
   int x1, y1, x2, y2;
   int tx1, ty1, tx2, ty2;
   E_Comp_Wl_Buffer *comp_buffer;
   tbm_surface_h tbm_surf;
   uint32_t size = 0, offset = 0, pitch = 0;
   int bw, bh;
   int width_from_buffer, height_from_buffer;

   EINA_SAFETY_ON_NULL_RETURN_VAL(ec, EINA_FALSE);

   comp_buffer = e_pixmap_resource_get(video->ec->pixmap);
   EINA_SAFETY_ON_NULL_RETURN_VAL(comp_buffer, EINA_FALSE);

   tbm_surf = wayland_tbm_server_get_surface(e_comp->wl_comp_data->tbm.server, comp_buffer->resource);
   EINA_SAFETY_ON_NULL_RETURN_VAL(tbm_surf, EINA_FALSE);

   tbm_surface_internal_get_plane_data(tbm_surf, 0, &size, &offset, &pitch);

   /* input geometry */
   if (IS_RGB(video->tbmfmt))
      video->geo.input_w = pitch / 4;
   else
      video->geo.input_w = pitch;

   video->geo.input_h = tbm_surface_get_height(tbm_surf);

   bw = tbm_surface_get_width(tbm_surf);
   bh = tbm_surface_get_height(tbm_surf);

   switch (vp->buffer.transform)
     {
      case WL_OUTPUT_TRANSFORM_90:
      case WL_OUTPUT_TRANSFORM_270:
      case WL_OUTPUT_TRANSFORM_FLIPPED_90:
      case WL_OUTPUT_TRANSFORM_FLIPPED_270:
        width_from_buffer = bh / vp->buffer.scale;
        height_from_buffer = bw / vp->buffer.scale;
        break;
      default:
        width_from_buffer = bw / vp->buffer.scale;
        height_from_buffer = bh / vp->buffer.scale;
        break;
     }


   if (vp->buffer.src_width == wl_fixed_from_int(-1))
     {
        x1 = 0.0;
        y1 = 0.0;
        x2 = width_from_buffer;
        y2 = height_from_buffer;
     }
   else
     {
        x1 = wl_fixed_to_int(vp->buffer.src_x);
        y1 = wl_fixed_to_int(vp->buffer.src_y);
        x2 = wl_fixed_to_int(vp->buffer.src_x + vp->buffer.src_width);
        y2 = wl_fixed_to_int(vp->buffer.src_y + vp->buffer.src_height);
     }

   VDB("transform(%d) scale(%d) buffer(%dx%d) src(%d,%d %d,%d) viewport(%dx%d)",
       vp->buffer.transform, vp->buffer.scale,
       width_from_buffer, height_from_buffer,
       x1, y1, x2 - x1, y2 - y1,
       ec->comp_data->width_from_viewport, ec->comp_data->height_from_viewport);

   buffer_transform(width_from_buffer, height_from_buffer,
                    vp->buffer.transform, vp->buffer.scale, x1, y1, &tx1, &ty1);
   buffer_transform(width_from_buffer, height_from_buffer,
                    vp->buffer.transform, vp->buffer.scale, x2, y2, &tx2, &ty2);

   video->geo.input_r.x = (tx1 <= tx2) ? tx1 : tx2;
   video->geo.input_r.y = (ty1 <= ty2) ? ty1 : ty2;
   video->geo.input_r.w = (tx1 <= tx2) ? tx2 - tx1 : tx1 - tx2;
   video->geo.input_r.h = (ty1 <= ty2) ? ty2 - ty1 : ty1 - ty2;

   /* output geometry */
   if ((sdata = ec->comp_data->sub.data))
     {
        if (sdata->parent)
          {
             video->geo.output_r.x = sdata->parent->x + sdata->position.x;
             video->geo.output_r.y = sdata->parent->y + sdata->position.y;
          }
        else
          {
             video->geo.output_r.x = sdata->position.x;
             video->geo.output_r.y = sdata->position.y;
          }
     }
   else
     {
        video->geo.output_r.x = ec->x;
        video->geo.output_r.y = ec->y;
     }

   video->geo.output_r.w = ec->comp_data->width_from_viewport;
   video->geo.output_r.w = (video->geo.output_r.w + 1) & ~1;
   video->geo.output_r.h = ec->comp_data->height_from_viewport;

   e_comp_object_frame_xy_unadjust(ec->frame,
                                   video->geo.output_r.x, video->geo.output_r.y,
                                   &video->geo.output_r.x, &video->geo.output_r.y);
   e_comp_object_frame_wh_unadjust(ec->frame,
                                   video->geo.output_r.w, video->geo.output_r.h,
                                   &video->geo.output_r.w, &video->geo.output_r.h);

   switch(vp->buffer.transform)
   {
   case WL_OUTPUT_TRANSFORM_90:
     video->geo.transform = TDM_TRANSFORM_270;
     break;
   case WL_OUTPUT_TRANSFORM_180:
     video->geo.transform = TDM_TRANSFORM_180;
     break;
   case WL_OUTPUT_TRANSFORM_270:
     video->geo.transform = TDM_TRANSFORM_90;
     break;
   case WL_OUTPUT_TRANSFORM_FLIPPED:
     video->geo.transform = TDM_TRANSFORM_FLIPPED;
     break;
   case WL_OUTPUT_TRANSFORM_FLIPPED_90:
     video->geo.transform = TDM_TRANSFORM_FLIPPED_270;
     break;
   case WL_OUTPUT_TRANSFORM_FLIPPED_180:
     video->geo.transform = TDM_TRANSFORM_FLIPPED_180;
     break;
   case WL_OUTPUT_TRANSFORM_FLIPPED_270:
     video->geo.transform = TDM_TRANSFORM_FLIPPED_90;
     break;
   case WL_OUTPUT_TRANSFORM_NORMAL:
   default:
     video->geo.transform = TDM_TRANSFORM_NORMAL;
     break;
   }

   VDB("geometry(%dx%d  %d,%d %dx%d  %d,%d %dx%d  %d)",
       video->geo.input_w, video->geo.input_h,
       EINA_RECTANGLE_ARGS(&video->geo.input_r),
       EINA_RECTANGLE_ARGS(&video->geo.output_r),
       video->geo.transform);

   return EINA_TRUE;
}

static void
_e_video_geometry_cal_map(E_Video *video)
{
   E_Client *ec = video->ec;
   const Evas_Map *m;
   Evas_Coord x1, x2, y1, y2;
   Eina_Rectangle old_output_r = video->geo.output_r;

   EINA_SAFETY_ON_NULL_RETURN(ec);
   EINA_SAFETY_ON_NULL_RETURN(video->ec->frame);

   m = evas_object_map_get(ec->frame);
   if (!m) return;

   /* If frame has map, it means that ec's geometry is decided by map's geometry.
    * ec->x,y,w,h and ec->client.x,y,w,h is not useful.
    */

   evas_map_point_coord_get(m, 0, &x1, &y1, NULL);
   evas_map_point_coord_get(m, 2, &x2, &y2, NULL);

   video->geo.output_r.x = x1;
   video->geo.output_r.y = y1;
   video->geo.output_r.w = x2 - x1;
   video->geo.output_r.w = (video->geo.output_r.w + 1) & ~1;
   video->geo.output_r.h = y2 - y1;

   VDB("frame(%p) m(%p) output(%d,%d %dx%d) => (%d,%d %dx%d)", ec->frame, m,
       EINA_RECTANGLE_ARGS(&old_output_r), EINA_RECTANGLE_ARGS(&video->geo.output_r));
}

static void
_e_video_geometry_cal_to_input(int output_w, int output_h, int input_w, int input_h,
                               tdm_transform trasnform, int ox, int oy, int *ix, int *iy)
{
   float ratio_w, ratio_h;

   switch(trasnform)
     {
      case TDM_TRANSFORM_NORMAL:
      default:
        *ix = ox, *iy = oy;
        break;
      case TDM_TRANSFORM_90:
        *ix = oy, *iy = output_w - ox;
        break;
      case TDM_TRANSFORM_180:
        *ix = output_w - ox, *iy = output_h - oy;
        break;
      case TDM_TRANSFORM_270:
        *ix = output_h - oy, *iy = ox;
        break;
      case TDM_TRANSFORM_FLIPPED:
        *ix = output_w - ox, *iy = oy;
        break;
      case TDM_TRANSFORM_FLIPPED_90:
        *ix = oy, *iy = ox;
        break;
      case TDM_TRANSFORM_FLIPPED_180:
        *ix = ox, *iy = output_h - oy;
        break;
      case TDM_TRANSFORM_FLIPPED_270:
        *ix = output_h - oy, *iy = output_w - ox;
        break;
     }
   if (trasnform & 0x1)
     {
        ratio_w = (float)input_w / output_h;
        ratio_h = (float)input_h / output_w;
     }
   else
     {
        ratio_w = (float)input_w / output_w;
        ratio_h = (float)input_h / output_h;
     }
   *ix *= ratio_w;
   *iy *= ratio_h;
}

static void
_e_video_geometry_cal_to_input_rect(E_Video * video, Eina_Rectangle *srect, Eina_Rectangle *drect)
{
   int xf1, yf1, xf2, yf2;

   /* first transform box coordinates if the scaler is set */

   xf1 = srect->x;
   yf1 = srect->y;
   xf2 = srect->x + srect->w;
   yf2 = srect->y + srect->h;

   _e_video_geometry_cal_to_input(video->geo.output_r.w, video->geo.output_r.h,
                                  video->geo.input_r.w, video->geo.input_r.h,
                                  video->geo.transform, xf1, yf1, &xf1, &yf1);
   _e_video_geometry_cal_to_input(video->geo.output_r.w, video->geo.output_r.h,
                                  video->geo.input_r.w, video->geo.input_r.h,
                                  video->geo.transform, xf2, yf2, &xf2, &yf2);

   drect->x = MIN(xf1, xf2);
   drect->y = MIN(yf1, yf2);
   drect->w = MAX(xf1, xf2) - drect->x;
   drect->h = MAX(yf1, yf2) - drect->y;
}

static Eina_Bool
_e_video_geometry_cal(E_Video * video)
{
   Eina_Rectangle screen = {0,};
   Eina_Rectangle output_r, input_r;

   /* get geometry information with buffer scale, transform and viewport. */
   if (!_e_video_geometry_cal_viewport(video))
     return EINA_FALSE;

   _e_video_geometry_cal_map(video);

   ecore_drm_output_current_resolution_get(video->drm_output, &screen.w, &screen.h, NULL);

   if (video->geo.output_r.x >= 0 && video->geo.output_r.y >= 0 &&
       (video->geo.output_r.x + video->geo.output_r.w) <= screen.w &&
       (video->geo.output_r.y + video->geo.output_r.h) <= screen.h)
     return EINA_TRUE;

   /* TODO: need to improve */

   output_r = video->geo.output_r;
   if (!eina_rectangle_intersection(&output_r, &screen))
     {
        VER("output_r(%d,%d %dx%d) screen(%d,%d %dx%d) => intersect(%d,%d %dx%d)",
            EINA_RECTANGLE_ARGS(&video->geo.output_r),
            EINA_RECTANGLE_ARGS(&screen), EINA_RECTANGLE_ARGS(&output_r));
        return EINA_TRUE;
     }

   output_r.x -= video->geo.output_r.x;
   output_r.y -= video->geo.output_r.y;

   _e_video_geometry_cal_to_input_rect(video, &output_r, &input_r);

   VDB("output(%d,%d %dx%d) input(%d,%d %dx%d)",
       EINA_RECTANGLE_ARGS(&output_r), EINA_RECTANGLE_ARGS(&input_r));

   output_r.x += video->geo.output_r.x;
   output_r.y += video->geo.output_r.y;

   output_r.x = output_r.x & ~1;
   output_r.w = (output_r.w + 1) & ~1;

   input_r.x = input_r.x & ~1;
   input_r.w = (input_r.w + 1) & ~1;

   video->geo.output_r = output_r;
   video->geo.input_r = input_r;

   return EINA_TRUE;
}

static void
_e_video_format_info_get(E_Video *video)
{
   E_Comp_Wl_Buffer *comp_buffer;
   tbm_surface_h tbm_surf;

   comp_buffer = e_pixmap_resource_get(video->ec->pixmap);
   EINA_SAFETY_ON_NULL_RETURN(comp_buffer);

   tbm_surf = wayland_tbm_server_get_surface(e_comp->wl_comp_data->tbm.server, comp_buffer->resource);
   EINA_SAFETY_ON_NULL_RETURN(tbm_surf);

   video->tbmfmt = tbm_surface_get_format(tbm_surf);
   VDB("video format: %c%c%c%c", FOURCC_STR(video->tbmfmt));
}

static E_Video_Fb*
_e_video_frame_buffer_create(E_Video *video, E_Devmgr_Buf *mbuf, Eina_Rectangle *visible, unsigned int transform)
{
   E_Video_Fb *vfb = calloc(1, sizeof(E_Video_Fb));
   EINA_SAFETY_ON_NULL_RETURN_VAL(vfb, NULL);

   vfb->mbuf = e_devmgr_buffer_ref(mbuf);
   vfb->video = video;
   vfb->visible_r = *visible;
   vfb->transform = transform;

   if (mbuf->comp_buffer)
     e_comp_wl_buffer_reference(&vfb->buffer_ref, mbuf->comp_buffer);

   return vfb;
}

static void
_e_video_frame_buffer_destroy(E_Video_Fb *vfb)
{
   E_Video *video;

   if (!vfb) return;

   video = vfb->video;
   if (vfb->mbuf)
     {
        vfb->mbuf->showing = EINA_FALSE;
        VDB("mbuf(%d) hidden", MSTAMP(vfb->mbuf));
        e_devmgr_buffer_unref(vfb->mbuf);
     }
   e_comp_wl_buffer_reference(&vfb->buffer_ref, NULL);
   free (vfb);
}

static void
_e_video_commit_handler(tdm_output *output, unsigned int sequence,
                        unsigned int tv_sec, unsigned int tv_usec,
                        void *user_data)
{
   E_Video *video;
   Eina_List *l;
   E_Video_Fb *vfb;

   EINA_LIST_FOREACH(video_list, l, video)
     {
        if (video == user_data) break;
     }

   if (!video) return;

   video->wait_vblank = EINA_FALSE;

   vfb = eina_list_nth(video->waiting_list, 0);
   if (!vfb) return;

   video->waiting_list = eina_list_remove(video->waiting_list, vfb);

   if (video->current_fb)
     {
        _e_video_frame_buffer_destroy(video->current_fb);
        video->current_fb = NULL;
     }

   video->current_fb = vfb;

   VDB("mbuf(%d) showing", MSTAMP(vfb->mbuf));

   if ((vfb = eina_list_nth(video->waiting_list, 0)))
     _e_video_frame_buffer_show(video, vfb);
}

static Eina_Bool
_e_video_frame_buffer_show(E_Video *video, E_Video_Fb *vfb)
{
   tdm_info_layer info, old_info;
   tdm_error ret;

   if (!vfb)
     {
        tdm_layer_unset_buffer(video->layer);
        tdm_output_commit(video->output, 0, NULL, NULL);
        return EINA_TRUE;
     }

   CLEAR(old_info);
   ret = tdm_layer_get_info(video->layer, &old_info);
   EINA_SAFETY_ON_FALSE_GOTO(ret == TDM_ERROR_NONE, show_fail);

   CLEAR(info);
   info.src_config.size.h = vfb->mbuf->width_from_pitch;
   info.src_config.size.v = vfb->mbuf->height;
   info.src_config.pos.x = vfb->visible_r.x;
   info.src_config.pos.y = vfb->visible_r.y;
   info.src_config.pos.w = vfb->visible_r.w;
   info.src_config.pos.h = vfb->visible_r.h;
   info.dst_pos.x = video->geo.output_r.x;
   info.dst_pos.y = video->geo.output_r.y;
   info.dst_pos.w = video->geo.output_r.w;
   info.dst_pos.h = video->geo.output_r.h;
   info.transform = vfb->transform;

   if (memcmp(&old_info, &info, sizeof(tdm_info_layer)))
     {
        ret = tdm_layer_set_info(video->layer, &info);
        EINA_SAFETY_ON_FALSE_GOTO(ret == TDM_ERROR_NONE, show_fail);
     }

   ret = tdm_layer_set_buffer(video->layer, vfb->mbuf->tbm_surface);
   EINA_SAFETY_ON_FALSE_GOTO(ret == TDM_ERROR_NONE, show_fail);

   ret = tdm_output_commit(video->output, 0, _e_video_commit_handler, video);
   EINA_SAFETY_ON_FALSE_GOTO(ret == TDM_ERROR_NONE, show_fail);

   if (video->need_punch)
     {
       E_Client *topmost = find_topmost_parent_get(video->ec);
       Eina_Bool do_punch = EINA_TRUE;

       /* FIXME: the mask obj can be drawn at the wrong position in the beginnig
        * time. It happens caused by window manager policy.
        */
       if ((topmost->fullscreen || topmost->maximized) &&
           (video->geo.output_r.x == 0 || video->geo.output_r.y == 0))
         {
            int bw, bh;

            e_pixmap_size_get(topmost->pixmap, &bw, &bh);

            if (bw > 100 && bh > 100 &&
                video->geo.output_r.w < 100 && video->geo.output_r.h < 100)
              {
                 VIN("don't punch. (%dx%d, %dx%d)",
                     bw, bh, video->geo.output_r.w, video->geo.output_r.h);
                 do_punch = EINA_FALSE;
              }
          }

       if (do_punch)
         {
            e_comp_object_mask_set(video->ec->frame, EINA_TRUE);
            video->need_punch = EINA_FALSE;
            VIN("punched");
         }
     }

   VDT("Client(%s):PID(%d) RscID(%d), Buffer(%p, refcnt:%d) is shown."
       "Geometry details are : buffer size(%dx%d) src(%d,%d, %dx%d)"
       " dst(%d,%d, %dx%d), transform(%d), need_punch(%d)",
       e_client_util_name_get(video->ec) ?: "No Name" , video->ec->netwm.pid,
       wl_resource_get_id(video->surface), vfb->mbuf, vfb->mbuf->ref_cnt,
       info.src_config.size.h, info.src_config.size.v, info.src_config.pos.x,
       info.src_config.pos.y, info.src_config.pos.w, info.src_config.pos.h,
       info.dst_pos.x, info.dst_pos.y, info.dst_pos.w, info.dst_pos.h, info.transform,
       video->need_punch);


   return EINA_TRUE;
show_fail:
   tdm_layer_unset_buffer(video->layer);
   tdm_output_commit(video->output, 0, NULL, NULL);
   return EINA_FALSE;
}

static void
_e_video_buffer_show(E_Video *video, E_Devmgr_Buf *mbuf, Eina_Rectangle *visible, unsigned int transform)
{
   E_Video_Fb *vfb;

   vfb = _e_video_frame_buffer_create(video, mbuf, visible, transform);
   EINA_SAFETY_ON_NULL_RETURN(vfb);

   video->waiting_list = eina_list_append(video->waiting_list, vfb);

   vfb->mbuf->showing = EINA_TRUE;
   VDB("mbuf(%d) waiting", MSTAMP(vfb->mbuf));

   /* There are waiting fbs more than 2 */
   if (eina_list_nth(video->waiting_list, 1))
     {
        VDT("There are waiting fbs more than 2");
        return;
     }

   if (e_devicemgr_dpms_get(video->drm_output))
     goto no_commit;

   if (!_e_video_frame_buffer_show(video, vfb))
     goto no_commit;

   return;

no_commit:
   _e_video_commit_handler(NULL, 0, 0, 0, video);
   return;
}

static void
_e_video_cb_evas_resize(void *data, Evas *e EINA_UNUSED, Evas_Object *obj, void *event_info EINA_UNUSED)
{
   E_Video *video = data;
   Evas_Coord w = 0, h = 0;

   evas_object_geometry_get(obj, NULL, NULL, &w, &h);
   if (video->eo_geo.w == w && video->eo_geo.h == h)
     return;

   video->eo_geo.w = w;
   video->eo_geo.h = h;

   _e_video_render(video, __FUNCTION__);
}

static void
_e_video_cb_evas_move(void *data, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event_info EINA_UNUSED)
{
   E_Video *video = data;
   Evas_Coord x = 0, y = 0;

   evas_object_geometry_get(obj, &x, &y, NULL, NULL);
   if (video->eo_geo.x == x && video->eo_geo.y == y)
     return;

   video->eo_geo.x = x;
   video->eo_geo.y = y;

   _e_video_render(video, __FUNCTION__);
}

static E_Video*
_e_video_create(struct wl_resource *video_object, struct wl_resource *surface)
{
   E_Video *video;
   E_Client *ec;

   ec = wl_resource_get_user_data(surface);
   EINA_SAFETY_ON_NULL_RETURN_VAL(ec, NULL);

   video = calloc(1, sizeof *video);
   EINA_SAFETY_ON_NULL_RETURN_VAL(video, NULL);

   video->video_object = video_object;
   video->surface = surface;
   video->output_align = -1;
   video->pp_align = -1;
   video->video_align = -1;

   VIN("create. ec(%p) wl_surface@%d", ec, wl_resource_get_id(video->surface));

   ec->comp_data->video_client = 1;

   video_list = eina_list_append(video_list, video);

   _e_video_set(video, ec);

   return video;
}

static void
_e_video_set(E_Video *video, E_Client *ec)
{
   int ominw = -1, ominh = -1, omaxw = -1, omaxh = -1;
   int pminw = -1, pminh = -1, pmaxw = -1, pmaxh = -1;
   int i, count = 0;
   const tdm_prop *props;
   E_Client *topmost;
   tdm_error ret;

   if (!video || !ec)
      return;

   if (video->ec)
     {
        VWR("already has ec");
        return;
     }

   EINA_SAFETY_ON_TRUE_RETURN(e_object_is_del(E_OBJECT(ec)));

   topmost = find_topmost_parent_get(ec);
   if (topmost && topmost->argb)
     video->need_punch = EINA_TRUE;

   video->ec = ec;
   video->window = e_client_util_win_get(ec);

   video->drm_output = _e_video_drm_output_find(video->ec);
   EINA_SAFETY_ON_NULL_RETURN(video->drm_output);

   /* TODO: find proper output */
   video->output = e_devicemgr_tdm_output_get(video->drm_output);
   EINA_SAFETY_ON_NULL_RETURN(video->output);

   video->layer = e_devicemgr_tdm_avaiable_video_layer_get(video->output);
   if (!video->layer)
      video->layer = e_devicemgr_tdm_avaiable_overlay_layer_get(video->output);
   EINA_SAFETY_ON_NULL_RETURN(video->layer);

   tdm_output_get_available_size(video->output, &ominw, &ominh, &omaxw, &omaxh, &video->output_align);
   ret = tdm_display_get_pp_available_size(e_devmgr_dpy->tdm, &pminw, &pminh, &pmaxw, &pmaxh, &video->pp_align);

   if (ret != TDM_ERROR_NONE)
     {
        video->video_align = video->output_align;
        tizen_video_object_send_size(video->video_object,
                                     ominw, ominh, omaxw, omaxh, video->output_align);
     }
   else
     {
        int minw = -1, minh = -1, maxw = -1, maxh = -1;

        minw = MAX(ominw, pminw);
        minh = MAX(ominh, pminh);

        if (omaxw != -1 && pmaxw == -1)
           maxw = omaxw;
        else if (omaxw == -1 && pmaxw != -1)
           maxw = pmaxw;
        else
           maxw = MIN(omaxw, pmaxw);

        if (omaxh != -1 && pmaxh == -1)
           maxw = omaxh;
        else if (omaxh == -1 && pmaxh != -1)
           maxw = pmaxh;
        else
           maxh = MIN(omaxh, pmaxh);

        if (video->output_align != -1 && video->pp_align == -1)
           video->video_align = video->output_align;
        else if (video->output_align == -1 && video->pp_align != -1)
           video->video_align = video->pp_align;
        else if (video->output_align == -1 && video->pp_align == -1)
           video->video_align = video->pp_align;
        else if (video->output_align > 0 && video->pp_align > 0)
           video->video_align = lcm(video->output_align, video->pp_align);
        else
           ERR("invalid align: %d, %d", video->output_align, video->pp_align);

        tizen_video_object_send_size(video->video_object,
                                     minw, minh, maxw, maxh, video->video_align);
        VIN("align width: output(%d) pp(%d) video(%d)",
            video->output_align, video->pp_align, video->video_align);
     }

   tdm_layer_get_available_properties(video->layer, &props, &count);
   for (i = 0; i < count; i++)
     {
        tdm_value value;
        tdm_layer_get_property(video->layer, props[i].id, &value);
        tizen_video_object_send_attribute(video->video_object, props[i].name, value.u32);
     }

   VIN("prepare. wl_surface@%d", wl_resource_get_id(video->surface));
}

static void
_e_video_destroy(E_Video *video)
{
   E_Devmgr_Buf *mbuf;
   E_Video_Fb *vfb;
   Eina_List *l = NULL, *ll = NULL;

   if (!video)
      return;

   if (video->cb_registered)
     {
        evas_object_event_callback_del_full(video->ec->frame, EVAS_CALLBACK_RESIZE,
                                            _e_video_cb_evas_resize, video);
        evas_object_event_callback_del_full(video->ec->frame, EVAS_CALLBACK_MOVE,
                                            _e_video_cb_evas_move, video);
     }

   wl_resource_set_destructor(video->video_object, NULL);

   /* hide video plane first */
   _e_video_frame_buffer_show(video, NULL);

   if (video->current_fb)
     {
        _e_video_frame_buffer_destroy(video->current_fb);
        video->current_fb = NULL;
     }

   EINA_LIST_FOREACH_SAFE(video->waiting_list, l, ll, vfb)
     _e_video_frame_buffer_destroy(vfb);

   /* others */
   EINA_LIST_FOREACH_SAFE(video->input_buffer_list, l, ll, mbuf)
     {
        tdm_buffer_remove_release_handler(mbuf->tbm_surface,
                                          _e_video_input_buffer_cb_release, video);
        e_devmgr_buffer_unref(mbuf);
     }

   EINA_LIST_FOREACH_SAFE(video->pp_buffer_list, l, ll, mbuf)
     {
        tdm_buffer_remove_release_handler(mbuf->tbm_surface,
                                          _e_video_pp_buffer_cb_release, video);
        e_devmgr_buffer_unref(mbuf);
     }

   /* destroy converter second */
   if (video->pp)
     tdm_pp_destroy(video->pp);

   video_list = eina_list_remove(video_list, video);

   VIN("stop");

   free(video);

#if 0
   if (e_devmgr_buffer_list_length() > 0)
     e_devmgr_buffer_list_print();
#endif
}

static Eina_Bool
_e_video_check_if_pp_needed(E_Video *video)
{
   int i, count = 0;
   const tbm_format *formats;
   Eina_Bool found = EINA_FALSE;
   tdm_layer_capability capabilities = 0;

   tdm_layer_get_capabilities(video->layer, &capabilities);

   /* don't need pp if a layer has TDM_LAYER_CAPABILITY_VIDEO capability*/
   if (capabilities & TDM_LAYER_CAPABILITY_VIDEO)
      return EINA_FALSE;

   /* check formats */
   tdm_layer_get_available_formats(video->layer, &formats, &count);
   for (i = 0; i < count; i++)
   {
      VDB("layer format: %c%c%c%c", FOURCC_STR(formats[i]));
      if (formats[i] == video->tbmfmt)
        {
           found = EINA_TRUE;
           break;
        }
   }
   if (!found)
     {
        video->pp_tbmfmt = TBM_FORMAT_ARGB8888;
        return EINA_TRUE;
     }

   /* check size */
   if (capabilities & TDM_LAYER_CAPABILITY_SCANOUT)
      goto need_pp;

   if (video->geo.input_r.w != video->geo.output_r.w || video->geo.input_r.h != video->geo.output_r.h)
      if (!(capabilities & TDM_LAYER_CAPABILITY_SCALE))
         goto need_pp;

   /* check rotate */
   if (video->geo.transform)
      if (!(capabilities & TDM_LAYER_CAPABILITY_TRANSFORM))
         goto need_pp;

   return EINA_FALSE;

need_pp:
   video->pp_tbmfmt = video->tbmfmt;
   return EINA_TRUE;
}

static void
_e_video_input_buffer_cb_release(tbm_surface_h surface, void *user_data)
{
   E_Devmgr_Buf *mbuf = user_data;

   tdm_buffer_remove_release_handler(surface, _e_video_input_buffer_cb_release, mbuf);

   /* unref input_buffer because we don't need input buffer any more after pp done */
   e_devmgr_buffer_unref(mbuf);
}

static void
_e_video_pp_buffer_cb_release(tbm_surface_h surface, void *user_data)
{
   E_Video *video = (E_Video*)user_data;
   E_Devmgr_Buf *mbuf;
   Eina_List *l;

   EINA_LIST_FOREACH(video->pp_buffer_list, l, mbuf)
     {
        if (mbuf->tbm_surface == surface)
           break;
     }
   EINA_SAFETY_ON_NULL_RETURN(mbuf);

   tdm_buffer_remove_release_handler(surface, _e_video_pp_buffer_cb_release, video);

   _e_video_buffer_show(video, mbuf, &video->pp_r, 0);

   /* don't unref pp_buffer here because we will unref it when video destroyed */
#ifdef DUMP_BUFFER
   static int i;
   e_devmgr_buffer_dump(mbuf, "out", i++, 0);
#endif
}

static void
_e_video_render(E_Video *video, const char *func)
{
   E_Comp_Wl_Buffer *comp_buffer;
   E_Devmgr_Buf *pp_buffer = NULL;
   E_Devmgr_Buf *input_buffer = NULL;

   EINA_SAFETY_ON_NULL_RETURN(video->ec);

   /* buffer can be NULL when camera/video's mode changed. Do nothing and
    * keep previous frame in this case.
    */
   if (!video->ec->pixmap)
     return;

   comp_buffer = e_pixmap_resource_get(video->ec->pixmap);
   if (!comp_buffer)
     return;

   _e_video_format_info_get(video);

   /* not interested with other buffer type */
   if (!wayland_tbm_server_get_surface(NULL, comp_buffer->resource))
     return;

   if (!_e_video_geometry_cal(video))
     return;

   if (!memcmp(&video->old_geo, &video->geo, sizeof video->geo) &&
       video->old_comp_buffer == comp_buffer)
     return;

   DBG("====================================== %s", func);
   VDB("old["GEO_FMT" buf(%p)] new["GEO_FMT" buf(%p)]",
       GEO_ARG(&video->old_geo), video->old_comp_buffer,
       GEO_ARG(&video->geo), comp_buffer);

   _e_video_input_buffer_valid(video, comp_buffer);

   if (!_e_video_check_if_pp_needed(video))
     {
        /* 1. non converting case */
        input_buffer = _e_video_input_buffer_get(video, comp_buffer, EINA_TRUE);
        EINA_SAFETY_ON_NULL_GOTO(input_buffer, render_fail);

        _e_video_buffer_show(video, input_buffer, &video->geo.input_r, video->geo.transform);

        video->old_geo = video->geo;
        video->old_comp_buffer = comp_buffer;

#ifdef DUMP_BUFFER
        static int i;
        e_devmgr_buffer_dump(input_buffer, "render", i++, 0);
#endif
        goto done;
     }

   /* 2. converting case */
   if (!video->pp)
     {
        video->pp = tdm_display_create_pp(e_devmgr_dpy->tdm, NULL);
        EINA_SAFETY_ON_NULL_GOTO(video->pp, render_fail);

        tdm_display_get_pp_available_size(e_devmgr_dpy->tdm, NULL, NULL, NULL, NULL, &video->pp_align);
     }

   input_buffer = _e_video_input_buffer_get(video, comp_buffer, EINA_FALSE);
   EINA_SAFETY_ON_NULL_GOTO(input_buffer, render_fail);

   pp_buffer = _e_video_pp_buffer_get(video, video->geo.output_r.w, video->geo.output_r.h);
   EINA_SAFETY_ON_NULL_GOTO(pp_buffer, render_fail);

   if (memcmp(&video->old_geo, &video->geo, sizeof video->geo))
     {
        tdm_info_pp info;

        CLEAR(info);
        info.src_config.size.h = video->geo.input_w;
        info.src_config.size.v = video->geo.input_h;
        info.src_config.pos.x = video->geo.input_r.x;
        info.src_config.pos.y = video->geo.input_r.y;
        info.src_config.pos.w = video->geo.input_r.w;
        info.src_config.pos.h = video->geo.input_r.h;
        info.src_config.format = video->tbmfmt;
        info.dst_config.size.h = pp_buffer->width_from_pitch;
        info.dst_config.size.v = pp_buffer->height;
        info.dst_config.pos.w = video->geo.output_r.w;
        info.dst_config.pos.h = video->geo.output_r.h;
        info.dst_config.format = video->pp_tbmfmt;
        info.transform = video->geo.transform;

        if (tdm_pp_set_info(video->pp, &info))
          goto render_fail;

        CLEAR(video->pp_r);
        video->pp_r.w = info.dst_config.pos.w;
        video->pp_r.h = info.dst_config.pos.h;
     }
#ifdef DUMP_BUFFER
   static int i;
   e_devmgr_buffer_dump(input_buffer, "in", i++, 0);
#endif

   tdm_buffer_add_release_handler(input_buffer->tbm_surface,
                                  _e_video_input_buffer_cb_release, input_buffer);
   tdm_buffer_add_release_handler(pp_buffer->tbm_surface,
                                  _e_video_pp_buffer_cb_release, video);

   if (tdm_pp_attach(video->pp, input_buffer->tbm_surface, pp_buffer->tbm_surface))
     goto render_fail;

   if (tdm_pp_commit(video->pp))
     goto render_fail;

   video->old_geo = video->geo;
   video->old_comp_buffer = comp_buffer;

   goto done;

render_fail:
   if (input_buffer)
     {
        tdm_buffer_remove_release_handler(input_buffer->tbm_surface,
                                          _e_video_input_buffer_cb_release, input_buffer);

        e_devmgr_buffer_unref(input_buffer);
     }

   if (pp_buffer)
     {
        tdm_buffer_remove_release_handler(pp_buffer->tbm_surface,
                                          _e_video_pp_buffer_cb_release, input_buffer);
        /* don't unref pp_buffer here because we will unref it when video destroyed */
     }
done:
   if (!video->cb_registered)
     {
        evas_object_event_callback_add(video->ec->frame, EVAS_CALLBACK_RESIZE,
                                       _e_video_cb_evas_resize, video);
        evas_object_event_callback_add(video->ec->frame, EVAS_CALLBACK_MOVE,
                                       _e_video_cb_evas_move, video);
        video->cb_registered = EINA_TRUE;
     }
   DBG("======================================.");
}

static Eina_Bool
_e_video_cb_ec_buffer_change(void *data, int type, void *event)
{
   E_Client *ec;
   E_Event_Client *ev = event;
   E_Video *video;

   EINA_SAFETY_ON_NULL_RETURN_VAL(ev, ECORE_CALLBACK_PASS_ON);
   EINA_SAFETY_ON_NULL_RETURN_VAL(ev->ec, ECORE_CALLBACK_PASS_ON);

   ec = ev->ec;
   if (e_object_is_del(E_OBJECT(ec)))
     return ECORE_CALLBACK_PASS_ON;

#ifdef DUMP_BUFFER
   if (ec->comp_data->sub.data && ec->pixmap)
     {
        E_Comp_Wl_Buffer *comp_buffer = e_pixmap_resource_get(ec->pixmap);
        if (comp_buffer && wl_shm_buffer_get(comp_buffer->resource))
          {
             E_Devmgr_Buf *mbuf = e_devmgr_buffer_create(comp_buffer->resource);
             EINA_SAFETY_ON_NULL_RETURN_VAL(mbuf, ECORE_CALLBACK_PASS_ON);
             static int i;
             e_devmgr_buffer_dump(mbuf, "dump", i++, 0);
             e_devmgr_buffer_unref(mbuf);
          }
     }
#endif

   /* not interested with non video_surface,  */
   video = find_video_with_surface(ec->comp_data->surface);
   if (!video) return ECORE_CALLBACK_PASS_ON;

   _e_video_render(video, __FUNCTION__);

   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
_e_video_cb_ec_add(void *data, int type, void *event)
{
   E_Event_Client *ev = event;
   E_Client *ec;
   E_Video *video;

   EINA_SAFETY_ON_NULL_RETURN_VAL(ev, ECORE_CALLBACK_PASS_ON);
   EINA_SAFETY_ON_NULL_RETURN_VAL(ev->ec, ECORE_CALLBACK_PASS_ON);

   ec = ev->ec;
   if (!ec->comp_data) return ECORE_CALLBACK_PASS_ON;

   video = find_video_with_surface(ec->comp_data->surface);
   if (video)
      _e_video_set(video, ec);

   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
_e_video_cb_ec_remove(void *data, int type, void *event)
{
   E_Event_Client *ev = event;
   E_Client *ec;
   E_Video *video;

   EINA_SAFETY_ON_NULL_RETURN_VAL(ev, ECORE_CALLBACK_PASS_ON);
   EINA_SAFETY_ON_NULL_RETURN_VAL(ev->ec, ECORE_CALLBACK_PASS_ON);

   ec = ev->ec;
   if (!ec->comp_data) return ECORE_CALLBACK_PASS_ON;

   video = find_video_with_surface(ec->comp_data->surface);
   if (!video) return ECORE_CALLBACK_PASS_ON;

   _e_video_destroy(video);

   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
_e_video_cb_ec_hide(void *data, int type, void *event)
{
   E_Event_Client *ev = event;
   E_Client *ec, *topmost;
   E_Video *video;

   EINA_SAFETY_ON_NULL_RETURN_VAL(ev, ECORE_CALLBACK_PASS_ON);
   EINA_SAFETY_ON_NULL_RETURN_VAL(ev->ec, ECORE_CALLBACK_PASS_ON);

   ec = ev->ec;
   if (!ec->comp_data) return ECORE_CALLBACK_PASS_ON;

   video = find_video_with_surface(ec->comp_data->surface);
   if (!video) return ECORE_CALLBACK_PASS_ON;

   topmost = find_topmost_parent_get(ec);
   if (!topmost) return ECORE_CALLBACK_PASS_ON;

   /* if topmost parent is visible, we don't hide previous video frame. */
   if (topmost->visible) return ECORE_CALLBACK_PASS_ON;

   _e_video_frame_buffer_show(video, NULL);

   VIN("hide");

   return ECORE_CALLBACK_PASS_ON;
}

static void
_e_devicemgr_video_object_destroy(struct wl_resource *resource)
{
   E_Video *video = wl_resource_get_user_data(resource);
   EINA_SAFETY_ON_NULL_RETURN(video);

   VDT("Video from Client(%s):PID(%d) is being destroyed, details are: "
       "RscID(%d), Buffer(%p), Video_Format(%c%c%c%c), "
       "Buffer_Size(%dx%d), Src Rect(%d,%d, %dx%d), Dest Rect(%d,%d, %dx%d),"
       " Transformed(%d), Punched(%d)",
       e_client_util_name_get(video->ec) ?: "No Name" , video->ec->netwm.pid,
       wl_resource_get_id(video->surface),
       video->current_fb?video->current_fb->mbuf:0, FOURCC_STR(video->tbmfmt),
       video->geo.input_w, video->geo.input_h, video->geo.input_r.x ,
       video->geo.input_r.y, video->geo.input_r.w, video->geo.input_r.h,
       video->geo.output_r.x ,video->geo.output_r.y, video->geo.output_r.w,
       video->geo.output_r.h, video->geo.transform, video->need_punch);

   _e_video_destroy(video);
}

static void
_e_devicemgr_video_object_cb_destroy(struct wl_client *client, struct wl_resource *resource)
{
   wl_resource_destroy(resource);
}

static void
_e_devicemgr_video_object_cb_set_attribute(struct wl_client *client,
                                           struct wl_resource *resource,
                                           const char *name,
                                           int32_t value)
{
   E_Video *video;
   int i, count = 0;
   const tdm_prop *props;

   video = wl_resource_get_user_data(resource);
   EINA_SAFETY_ON_NULL_RETURN(video);

   tdm_layer_get_available_properties(video->layer, &props, &count);

   if(!strncmp(name, "mute", TDM_NAME_LEN) && video->ec)
      VDT("Client(%s):PID(%d) RscID(%d) Attribute:%s, Value:%d",
          e_client_util_name_get(video->ec) ?: "No Name",
          video->ec->netwm.pid, wl_resource_get_id(video->surface),
          name,value);

   for (i = 0; i < count; i++)
     {
        if (!strncmp(name, props[i].name, TDM_NAME_LEN))
          {
             tdm_value v;
             v.u32 = value;
             tdm_layer_set_property(video->layer, props[i].id, v);
             return;
          }
     }
}

static const struct tizen_video_object_interface _e_devicemgr_video_object_interface =
{
   _e_devicemgr_video_object_cb_destroy,
   _e_devicemgr_video_object_cb_set_attribute,
};

static void
_e_devicemgr_video_cb_get_object(struct wl_client *client,
                                 struct wl_resource *resource,
                                 uint32_t id,
                                 struct wl_resource *surface)
{
   int version = wl_resource_get_version(resource);
   E_Video *video = wl_resource_get_user_data(resource);
   struct wl_resource *res;

   if (video)
     {
        wl_resource_post_error(resource,
                               TIZEN_VIDEO_ERROR_OBJECT_EXISTS,
                               "a video object for that surface already exists");
        return;
     }

   res = wl_resource_create(client, &tizen_video_object_interface, version, id);
   if (res == NULL)
     {
        wl_client_post_no_memory(client);
        return;
     }

   if (!(video = _e_video_create(res, surface)))
     {
        wl_resource_destroy(res);
        wl_client_post_no_memory(client);
        return;
     }

   wl_resource_set_implementation(res, &_e_devicemgr_video_object_interface,
                                  video, _e_devicemgr_video_object_destroy);
}

static void
_e_devicemgr_video_cb_get_viewport(struct wl_client *client,
                                   struct wl_resource *resource,
                                   uint32_t id,
                                   struct wl_resource *subsurface)
{
   E_Client *ec;

   if (!(ec = wl_resource_get_user_data(subsurface))) return;
   if (!ec->comp_data) return;

   if (!ec->comp_data->sub.data)
     {
        wl_resource_post_error(resource,
                               WL_DISPLAY_ERROR_INVALID_OBJECT,
                               "wl_subsurface@%d is not subsurface",
                               wl_resource_get_id(subsurface));
        return;
     }

   if (ec->comp_data && ec->comp_data->scaler.viewport)
     {
        wl_resource_post_error(resource,
                               TIZEN_VIDEO_ERROR_VIEWPORT_EXISTS,
                               "a viewport for that subsurface already exists");
        return;
     }

   if (!e_devicemgr_viewport_create(resource, id, subsurface))
     {
        ERR("Failed to create viewport for wl_subsurface@%d",
            wl_resource_get_id(subsurface));
        wl_client_post_no_memory(client);
        return;
     }
}

static const struct tizen_video_interface _e_devicemgr_video_interface =
{
   _e_devicemgr_video_cb_get_object,
   _e_devicemgr_video_cb_get_viewport,
};

static void
_e_devicemgr_video_cb_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
   struct wl_resource *res;
   const tbm_format *formats = NULL;
   int i, count = 0;

   if (!(res = wl_resource_create(client, &tizen_video_interface, MIN(version, 1), id)))
     {
        ERR("Could not create tizen_video_interface resource: %m");
        wl_client_post_no_memory(client);
        return;
     }

   wl_resource_set_implementation(res, &_e_devicemgr_video_interface, NULL, NULL);

   /* 1st, use pp information. */
   if (e_devmgr_dpy->pp_available)
     {
        tdm_display_get_pp_available_formats(e_devmgr_dpy->tdm, &formats, &count);
        for (i = 0; i < count; i++)
           tizen_video_send_format(res, formats[i]);
     }
   else
     {
        tdm_output *output = e_devicemgr_tdm_output_get(NULL);
        tdm_layer *layer;

        EINA_SAFETY_ON_NULL_RETURN(output);

        /* 2nd, use video_layer information.
         * 3rd, use overlay_layer information.
         */
        layer = e_devicemgr_tdm_avaiable_video_layer_get(output);
        if (!layer)
           layer = e_devicemgr_tdm_avaiable_overlay_layer_get(output);
        EINA_SAFETY_ON_NULL_RETURN(layer);

        tdm_layer_get_available_formats(layer, &formats, &count);
        for (i = 0; i < count; i++)
           tizen_video_send_format(res, formats[i]);
     }
}

static Eina_List *video_hdlrs;

int
e_devicemgr_video_init(void)
{
   if (!e_comp_wl) return 0;
   if (!e_comp_wl->wl.disp) return 0;


   _video_detail_log_dom = eina_log_domain_register("e-devicemgr-video", EINA_COLOR_BLUE);
   if (_video_detail_log_dom < 0)
     {
        ERR("Failed eina_log_domain_register()..!\n");
        return 0;
     }


   /* try to add tizen_screenshooter to wayland globals */
   if (!wl_global_create(e_comp_wl->wl.disp, &tizen_video_interface, 1,
                         NULL, _e_devicemgr_video_cb_bind))
     {
        ERR("Could not add tizen_screenshooter to wayland globals");
        return 0;
     }

   E_LIST_HANDLER_APPEND(video_hdlrs, E_EVENT_CLIENT_BUFFER_CHANGE,
                         _e_video_cb_ec_buffer_change, NULL);
   E_LIST_HANDLER_APPEND(video_hdlrs, E_EVENT_CLIENT_ADD,
                         _e_video_cb_ec_add, NULL);
   E_LIST_HANDLER_APPEND(video_hdlrs, E_EVENT_CLIENT_REMOVE,
                         _e_video_cb_ec_remove, NULL);
   E_LIST_HANDLER_APPEND(video_hdlrs, E_EVENT_CLIENT_HIDE,
                         _e_video_cb_ec_hide, NULL);

   return 1;
}

void
e_devicemgr_video_fini(void)
{
   E_FREE_LIST(video_hdlrs, ecore_event_handler_del);

   eina_log_domain_unregister(_video_detail_log_dom);
   _video_detail_log_dom = -1;
}

Eina_List*
e_devicemgr_video_list_get(void)
{
   return video_list;
}

E_Video*
e_devicemgr_video_get(struct wl_resource *surface_resource)
{
   return find_video_with_surface(surface_resource);

}

E_Devmgr_Buf*
e_devicemgr_video_fb_get(E_Video *video)
{
   tdm_layer_capability capabilities = 0;

   EINA_SAFETY_ON_NULL_RETURN_VAL(video, NULL);

   if (tdm_layer_get_capabilities(video->layer, &capabilities) != TDM_ERROR_NONE)
      return NULL;

   if (capabilities & TDM_LAYER_CAPABILITY_VIDEO)
      return NULL;

   return (video->current_fb) ? video->current_fb->mbuf : NULL;
}

void
e_devicemgr_video_pos_get(E_Video *video, int *x, int *y)
{
   EINA_SAFETY_ON_NULL_RETURN(video);

   if (x) *x = video->geo.output_r.x;
   if (y) *y = video->geo.output_r.y;
}

Ecore_Drm_Output*
e_devicemgr_video_drm_output_get(E_Video *video)
{
   EINA_SAFETY_ON_NULL_RETURN_VAL(video, NULL);

   return video->drm_output;
}
