#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define E_COMP_WL
#include <sys/mman.h>
#include <e.h>
#include <Ecore_Drm.h>
#include <pixman.h>
#include <png.h>
#include <tdm_helper.h>
#include "e_mod_main.h"
#include "e_devicemgr_privates.h"
#include "e_devicemgr_tdm.h"
#include "e_devicemgr_buffer.h"

//#define DEBUG_LIFECYCLE

#define PNG_DEPTH 8

#define BER(fmt,arg...)   ERR("%d: "fmt, mbuf ? mbuf->stamp : 0, ##arg)
#define BWR(fmt,arg...)   WRN("%d: "fmt, mbuf ? mbuf->stamp : 0, ##arg)
#define BIN(fmt,arg...)   INF("%d: "fmt, mbuf ? mbuf->stamp : 0, ##arg)
#define BDB(fmt,arg...)   DBG("%d: "fmt, mbuf ? mbuf->stamp : 0, ##arg)

#define MBUF_RETURN_IF_FAIL(cond) \
   {if (!(cond)) { BER("'%s' failed. (%s)", #cond, func); return; }}
#define MBUF_RETURN_VAL_IF_FAIL(cond, val) \
   {if (!(cond)) { BER("'%s' failed. (%s)", #cond, func); return val; }}

#ifdef DEBUG_LIFECYCLE
#undef BDB
#define BDB BIN
#endif

typedef struct _MBufFreeFuncInfo
{
   MBuf_Free_Func func;
   void *data;
} MBufFreeFuncInfo;

static void _e_devmgr_buffer_cb_destroy(struct wl_listener *listener, void *data);
static void _e_devmgr_buffer_free(E_Devmgr_Buf *mbuf, const char *func);
#define e_devmgr_buffer_free(b) _e_devmgr_buffer_free(b,__FUNCTION__)

static Eina_List *mbuf_lists;

static E_Devmgr_Buf*
_find_mbuf(uint stamp)
{
   E_Devmgr_Buf *mbuf;
   Eina_List *l;

   if (!mbuf_lists)
     return NULL;

   EINA_LIST_FOREACH(mbuf_lists, l, mbuf)
     {
        if (mbuf->stamp == stamp)
          return mbuf;
     }

   return NULL;
}

static tbm_bo
_handle_to_bo(uint handle)
{
   struct drm_gem_flink arg = {0,};
   tbm_bo bo;

   arg.handle = handle;
   if (drmIoctl(e_devmgr_dpy->drm_fd, DRM_IOCTL_GEM_FLINK, &arg))
     {
        ERR("failed flink id (gem:%d)\n", handle);
        return NULL;
     }

   bo = tbm_bo_import(e_devmgr_dpy->bufmgr, arg.name);
   if (!bo)
     {
        ERR("failed import (gem:%d, name:%d)\n", handle, arg.name);
        return NULL;
     }

   return bo;
}

static Eina_Bool
_e_devicemgr_buffer_access_data_begin(E_Devmgr_Buf *mbuf)
{
   EINA_SAFETY_ON_NULL_RETURN_VAL(mbuf, EINA_FALSE);

   mbuf->ptrs[0] = mbuf->ptrs[1] = mbuf->ptrs[2] = NULL;

   if (mbuf->type == TYPE_SHM)
     {
        struct wl_shm_buffer *shm_buffer = wl_shm_buffer_get(mbuf->resource);
        EINA_SAFETY_ON_NULL_RETURN_VAL(shm_buffer, EINA_FALSE);
        mbuf->ptrs[0] = wl_shm_buffer_get_data(shm_buffer);
        EINA_SAFETY_ON_NULL_RETURN_VAL(mbuf->ptrs[0], EINA_FALSE);
        return EINA_TRUE;
     }
   else if (mbuf->type == TYPE_TBM)
     {
        int i, j;
        tbm_bo bos[4] = {0,};

        for (i = 0; i < 3; i++)
          {
             tbm_bo_handle bo_handles;

             bos[i] = tbm_surface_internal_get_bo(mbuf->tbm_surface, i);
             if (!bos[i]) continue;

             bo_handles = tbm_bo_map(bos[i], TBM_DEVICE_CPU, TBM_OPTION_READ);
             if (!bo_handles.ptr)
               {
                  for (j = 0; j < i; j++)
                    tbm_bo_unmap(bos[j]);
                  return EINA_FALSE;
               }

             mbuf->ptrs[i] = bo_handles.ptr;
          }

        EINA_SAFETY_ON_NULL_RETURN_VAL(mbuf->ptrs[0], EINA_FALSE);

        switch(mbuf->tbmfmt)
          {
           case TBM_FORMAT_YVU420:
           case TBM_FORMAT_YUV420:
             if (!mbuf->ptrs[1])
                mbuf->ptrs[1] = mbuf->ptrs[0];
             if (!mbuf->ptrs[2])
                mbuf->ptrs[2] = mbuf->ptrs[1];
             break;
           case TBM_FORMAT_NV12:
           case TBM_FORMAT_NV21:
             if (!mbuf->ptrs[1])
               mbuf->ptrs[1] = mbuf->ptrs[0];
             break;
           default:
             break;
          }

        return EINA_TRUE;
     }

   return EINA_FALSE;
}

static void
_e_devicemgr_buffer_access_data_end(E_Devmgr_Buf *mbuf)
{
   EINA_SAFETY_ON_NULL_RETURN(mbuf);

   if (mbuf->type == TYPE_SHM)
     {
        mbuf->ptrs[0] = NULL;
     }
   else if (mbuf->type == TYPE_TBM)
     {
        int i;
        tbm_bo bos[4] = {0,};

        for (i = 0; i < 3; i++)
          {
             bos[i] = tbm_surface_internal_get_bo(mbuf->tbm_surface, i);
             if (!bos[i]) continue;

             tbm_bo_unmap(bos[i]);
             mbuf->ptrs[i] = NULL;
          }
     }
}

static E_Devmgr_Buf*
_e_devmgr_buffer_create_res(struct wl_resource *resource, const char *func)
{
   E_Devmgr_Buf *mbuf = NULL;
   struct wl_shm_buffer *shm_buffer;
   tbm_surface_h tbm_surface;

   EINA_SAFETY_ON_NULL_RETURN_VAL(resource, NULL);

   mbuf = calloc(1, sizeof(E_Devmgr_Buf));
   EINA_SAFETY_ON_FALSE_GOTO(mbuf != NULL, create_fail);

   mbuf->ref_cnt = 1;
   mbuf->stamp = e_devmgr_buffer_get_mills();
   while (_find_mbuf(mbuf->stamp))
     mbuf->stamp++;
   mbuf->func = strdup(func);

   mbuf->resource = resource;

   if ((shm_buffer = wl_shm_buffer_get(resource)))
     {
        uint32_t tbmfmt = wl_shm_buffer_get_format(shm_buffer);

        mbuf->type = TYPE_SHM;

        if (tbmfmt == WL_SHM_FORMAT_ARGB8888)
          mbuf->tbmfmt = TBM_FORMAT_ARGB8888;
        else if (tbmfmt == WL_SHM_FORMAT_XRGB8888)
          mbuf->tbmfmt = TBM_FORMAT_XRGB8888;
        else
          mbuf->tbmfmt = tbmfmt;

        mbuf->width = wl_shm_buffer_get_width(shm_buffer);
        mbuf->height = wl_shm_buffer_get_height(shm_buffer);
        mbuf->pitches[0] = wl_shm_buffer_get_stride(shm_buffer);

        mbuf->width_from_pitch = mbuf->width;
        mbuf->height_from_size = mbuf->height;;
     }
   else if ((tbm_surface = wayland_tbm_server_get_surface(e_comp->wl_comp_data->tbm.server, resource)))
     {
        int i;

        mbuf->type = TYPE_TBM;
        mbuf->tbm_surface = tbm_surface;
        tbm_surface_internal_ref(tbm_surface);

        mbuf->tbmfmt = tbm_surface_get_format(tbm_surface);
        mbuf->width = tbm_surface_get_width(tbm_surface);
        mbuf->height = tbm_surface_get_height(tbm_surface);

        for (i = 0; i < 3; i++)
          {
             uint32_t size = 0, offset = 0, pitch = 0;
             tbm_bo bo;

             bo = tbm_surface_internal_get_bo(tbm_surface, i);
             if (bo)
               {
                  mbuf->handles[i] = tbm_bo_get_handle(bo, TBM_DEVICE_DEFAULT).u32;
                  EINA_SAFETY_ON_FALSE_GOTO(mbuf->handles[i] > 0, create_fail);

                  mbuf->names[i] = tbm_bo_export(bo);
                  EINA_SAFETY_ON_FALSE_GOTO(mbuf->names[i] > 0, create_fail);
               }

             tbm_surface_internal_get_plane_data(tbm_surface, i, &size, &offset, &pitch);
             mbuf->pitches[i] = pitch;
             mbuf->offsets[i] = offset;
          }

        tdm_helper_get_buffer_full_size(tbm_surface, &mbuf->width_from_pitch, &mbuf->height_from_size);
     }
   else
     {
        ERR("unknown buffer resource");
        goto create_fail;
     }

   mbuf_lists = eina_list_append(mbuf_lists, mbuf);

   BDB("type(%d) %dx%d(%dx%d), %c%c%c%c, name(%d,%d,%d) hnd(%d,%d,%d), pitch(%d,%d,%d), offset(%d,%d,%d): %s",
       mbuf->type, mbuf->width_from_pitch, mbuf->height_from_size,
       mbuf->width, mbuf->height, FOURCC_STR(mbuf->tbmfmt),
       mbuf->names[0], mbuf->names[1], mbuf->names[2],
       mbuf->handles[0], mbuf->handles[1], mbuf->handles[2],
       mbuf->pitches[0], mbuf->pitches[1], mbuf->pitches[2],
       mbuf->offsets[0], mbuf->offsets[1], mbuf->offsets[2],
       func);

   return mbuf;

create_fail:
   e_devmgr_buffer_free(mbuf);

   return NULL;
}

E_Devmgr_Buf*
_e_devmgr_buffer_create(struct wl_resource *resource, const char *func)
{
   E_Devmgr_Buf *mbuf = _e_devmgr_buffer_create_res(resource, func);
   EINA_SAFETY_ON_NULL_RETURN_VAL(mbuf, NULL);

   mbuf->destroy_listener.notify = _e_devmgr_buffer_cb_destroy;
   wl_resource_add_destroy_listener(resource, &mbuf->destroy_listener);

   return mbuf;
}

E_Devmgr_Buf*
_e_devmgr_buffer_create_comp(E_Comp_Wl_Buffer *comp_buffer, const char *func)
{
   E_Devmgr_Buf *mbuf;

   EINA_SAFETY_ON_NULL_RETURN_VAL(comp_buffer, NULL);

   mbuf = _e_devmgr_buffer_create_res(comp_buffer->resource, func);
   EINA_SAFETY_ON_NULL_RETURN_VAL(mbuf, NULL);

   mbuf->comp_buffer = comp_buffer;

   mbuf->destroy_listener.notify = _e_devmgr_buffer_cb_destroy;
   wl_resource_add_destroy_listener(comp_buffer->resource, &mbuf->destroy_listener);

   return mbuf;
}

E_Devmgr_Buf*
_e_devmgr_buffer_create_tbm(tbm_surface_h tbm_surface, const char *func)
{
   E_Devmgr_Buf *mbuf;
   int i;

   EINA_SAFETY_ON_NULL_RETURN_VAL(tbm_surface, NULL);

   mbuf = calloc(1, sizeof(E_Devmgr_Buf));
   EINA_SAFETY_ON_FALSE_GOTO(mbuf != NULL, create_fail);

   mbuf->ref_cnt = 1;
   mbuf->stamp = e_devmgr_buffer_get_mills();
   while (_find_mbuf(mbuf->stamp))
     mbuf->stamp++;
   mbuf->func = strdup(func);

   mbuf->type = TYPE_TBM;
   mbuf->tbm_surface = tbm_surface;
   tbm_surface_internal_ref(tbm_surface);

   mbuf->tbmfmt = tbm_surface_get_format(tbm_surface);
   mbuf->width = tbm_surface_get_width(tbm_surface);
   mbuf->height = tbm_surface_get_height(tbm_surface);

   for (i = 0; i < 3; i++)
     {
        uint32_t size = 0, offset = 0, pitch = 0;
        tbm_bo bo;

        bo = tbm_surface_internal_get_bo(tbm_surface, i);
        if (bo)
          {
             mbuf->handles[i] = tbm_bo_get_handle(bo, TBM_DEVICE_DEFAULT).u32;
             EINA_SAFETY_ON_FALSE_GOTO(mbuf->handles[i] > 0, create_fail);

             mbuf->names[i] = tbm_bo_export(bo);
             EINA_SAFETY_ON_FALSE_GOTO(mbuf->names[i] > 0, create_fail);
          }

        tbm_surface_internal_get_plane_data(tbm_surface, i, &size, &offset, &pitch);
        mbuf->pitches[i] = pitch;
        mbuf->offsets[i] = offset;
     }

   tdm_helper_get_buffer_full_size(tbm_surface, &mbuf->width_from_pitch, &mbuf->height_from_size);

   mbuf_lists = eina_list_append(mbuf_lists, mbuf);

   BDB("type(%d) %dx%d(%dx%d), %c%c%c%c, name(%d,%d,%d) hnd(%d,%d,%d), pitch(%d,%d,%d), offset(%d,%d,%d): %s",
       mbuf->type, mbuf->width_from_pitch, mbuf->height_from_size,
       mbuf->width, mbuf->height, FOURCC_STR(mbuf->tbmfmt),
       mbuf->names[0], mbuf->names[1], mbuf->names[2],
       mbuf->handles[0], mbuf->handles[1], mbuf->handles[2],
       mbuf->pitches[0], mbuf->pitches[1], mbuf->pitches[2],
       mbuf->offsets[0], mbuf->offsets[1], mbuf->offsets[2],
       func);

   return mbuf;

create_fail:
   e_devmgr_buffer_free(mbuf);

   return NULL;
}

E_Devmgr_Buf*
_e_devmgr_buffer_create_hnd(uint handle, int width, int height, int pitch, const char *func)
{
   E_Devmgr_Buf *mbuf = NULL;
   tbm_surface_h tbm_surface;
   tbm_surface_info_s info = {0,};
   tbm_bo bo = NULL;

   EINA_SAFETY_ON_FALSE_RETURN_VAL(handle > 0, NULL);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(width > 0, NULL);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(height > 0, NULL);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(pitch > 0, NULL);

   mbuf = calloc(1, sizeof(E_Devmgr_Buf));
   EINA_SAFETY_ON_NULL_GOTO(mbuf, create_fail);

   mbuf->ref_cnt = 1;
   mbuf->stamp = e_devmgr_buffer_get_mills();
   while (_find_mbuf(mbuf->stamp))
     mbuf->stamp++;
   mbuf->func = strdup(func);

   bo = _handle_to_bo(handle);
   EINA_SAFETY_ON_NULL_GOTO(bo, create_fail);

   info.width = width;
   info.height = height;
   info.format = TBM_FORMAT_ARGB8888;
   info.bpp = tbm_surface_internal_get_bpp(info.format);
   info.num_planes = 1;
   info.planes[0].stride = pitch;

   tbm_surface = tbm_surface_internal_create_with_bos(&info, &bo, 1);
   EINA_SAFETY_ON_NULL_GOTO(tbm_surface, create_fail);

   mbuf->type = TYPE_TBM;
   mbuf->tbm_surface = tbm_surface;

   mbuf->tbmfmt = info.format;
   mbuf->width = info.width;
   mbuf->height = info.height;
   mbuf->pitches[0] = info.planes[0].stride;
   mbuf->handles[0] = handle;

   mbuf->width_from_pitch = mbuf->pitches[0]>>2;

   mbuf->names[0] = tbm_bo_export(bo);
   EINA_SAFETY_ON_FALSE_GOTO(mbuf->names[0] > 0, create_fail);

   tbm_bo_unref(bo);

   mbuf_lists = eina_list_append(mbuf_lists, mbuf);

   BDB("type(%d) %dx%d %c%c%c%c nm(%d,%d,%d) hnd(%d,%d,%d) pitch(%d,%d,%d) offset(%d,%d,%d): %s",
       mbuf->type, mbuf->width, mbuf->height, FOURCC_STR(mbuf->tbmfmt),
       mbuf->names[0], mbuf->names[1], mbuf->names[2],
       mbuf->handles[0], mbuf->handles[1], mbuf->handles[2],
       mbuf->pitches[0], mbuf->pitches[1], mbuf->pitches[2],
       mbuf->offsets[0], mbuf->offsets[1], mbuf->offsets[2],
       func);

   return mbuf;

create_fail:
   if (bo)
     tbm_bo_unref(bo);
   e_devmgr_buffer_free(mbuf);
   return NULL;
}

E_Devmgr_Buf*
_e_devmgr_buffer_alloc(int width, int height, tbm_format tbmfmt, Eina_Bool scanout, const char *func)
{
   E_Devmgr_Buf *mbuf = NULL;
   tbm_surface_h tbm_surface = NULL;
   int i;

   EINA_SAFETY_ON_FALSE_RETURN_VAL(width > 0, NULL);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(height > 0, NULL);

   mbuf = calloc(1, sizeof(E_Devmgr_Buf));
   EINA_SAFETY_ON_FALSE_GOTO(mbuf != NULL, alloc_fail);

   mbuf->ref_cnt = 1;
   mbuf->stamp = e_devmgr_buffer_get_mills();
   while (_find_mbuf(mbuf->stamp))
     mbuf->stamp++;
   mbuf->func = strdup(func);

   if (scanout)
     tbm_surface = tbm_surface_internal_create_with_flags(width, height, tbmfmt, TBM_BO_SCANOUT);
   else
     tbm_surface = tbm_surface_internal_create_with_flags(width, height, tbmfmt, TBM_BO_DEFAULT);
   EINA_SAFETY_ON_NULL_GOTO(tbm_surface, alloc_fail);

   mbuf->type = TYPE_TBM;
   mbuf->tbm_surface = tbm_surface;
   tbm_surface_internal_ref(tbm_surface);

   mbuf->tbmfmt = tbmfmt;
   mbuf->width = width;
   mbuf->height = height;

   for (i = 0; i < 3; i++)
     {
        uint32_t size = 0, offset = 0, pitch = 0;
        tbm_bo bo;

        bo = tbm_surface_internal_get_bo(tbm_surface, i);
        if (bo)
          {
             mbuf->handles[i] = tbm_bo_get_handle(bo, TBM_DEVICE_DEFAULT).u32;
             EINA_SAFETY_ON_FALSE_GOTO(mbuf->handles[i] > 0, alloc_fail);

             mbuf->names[i] = tbm_bo_export(bo);
             EINA_SAFETY_ON_FALSE_GOTO(mbuf->names[i] > 0, alloc_fail);
          }

        tbm_surface_internal_get_plane_data(tbm_surface, i, &size, &offset, &pitch);
        mbuf->pitches[i] = pitch;
        mbuf->offsets[i] = offset;
     }

   tdm_helper_get_buffer_full_size(tbm_surface, &mbuf->width_from_pitch, &mbuf->height_from_size);

   tbm_surface_internal_unref(tbm_surface);

   mbuf_lists = eina_list_append(mbuf_lists, mbuf);

   BDB("type(%d) %dx%d(%dx%d) %c%c%c%c nm(%d,%d,%d) hnd(%d,%d,%d) pitch(%d,%d,%d) offset(%d,%d,%d): %s",
       mbuf->type, mbuf->width_from_pitch, mbuf->height_from_size,
       mbuf->width, mbuf->height, FOURCC_STR(mbuf->tbmfmt),
       mbuf->names[0], mbuf->names[1], mbuf->names[2],
       mbuf->handles[0], mbuf->handles[1], mbuf->handles[2],
       mbuf->pitches[0], mbuf->pitches[1], mbuf->pitches[2],
       mbuf->offsets[0], mbuf->offsets[1], mbuf->offsets[2],
       func);

   return mbuf;

alloc_fail:
   if (tbm_surface)
     tbm_surface_internal_unref(tbm_surface);
   e_devmgr_buffer_free(mbuf);
   return NULL;
}

E_Devmgr_Buf*
_e_devmgr_buffer_ref(E_Devmgr_Buf *mbuf, const char *func)
{
   if (!mbuf)
     return NULL;

   EINA_SAFETY_ON_FALSE_RETURN_VAL(MBUF_IS_VALID(mbuf), NULL);

   mbuf->ref_cnt++;
   BDB("count(%d) ref: %s", mbuf->ref_cnt, func);

   return mbuf;
}

void
_e_devmgr_buffer_unref(E_Devmgr_Buf *mbuf, const char *func)
{
   if (!mbuf)
     return;

   MBUF_RETURN_IF_FAIL(_e_devmgr_buffer_valid(mbuf, func));

   mbuf->ref_cnt--;
   BDB("count(%d) unref: %s", mbuf->ref_cnt, func);

   if (!mbuf->buffer_destroying && mbuf->ref_cnt == 0)
     _e_devmgr_buffer_free(mbuf, func);
}

static void
_e_devmgr_buffer_free(E_Devmgr_Buf *mbuf, const char *func)
{
   MBufFreeFuncInfo *info;
   Eina_List *l, *ll;

   if (!mbuf)
     return;

   MBUF_RETURN_IF_FAIL(_e_devmgr_buffer_valid(mbuf, func));

   BDB("mbuf(%p) tbm_surface(%p) freed: %s", mbuf, mbuf->tbm_surface, func);

   mbuf->buffer_destroying = EINA_TRUE;

   if (mbuf->destroy_listener.notify)
     {
        wl_list_remove(&mbuf->destroy_listener.link);
        mbuf->destroy_listener.notify = NULL;
     }

   EINA_LIST_FOREACH_SAFE(mbuf->free_funcs, l, ll, info)
     {
        /* call before tmb_bo_unref */
        mbuf->free_funcs = eina_list_remove_list(mbuf->free_funcs, l);
        if (info->func)
          info->func(mbuf, info->data);
        free(info);
     }

#if 0
   /* DO not check ref_count here. Even if ref_count is not 0, mbuf can be
    * be destroyed by wl_buffer_destroy forcely. video or screenmirror should add
    * the mbuf free function and handle the destroying mbuf situation.
    */
   if (!mbuf->buffer_destroying)
     MBUF_RETURN_IF_FAIL(mbuf->ref_cnt == 0);
#endif

   /* make sure all operation is done */
   MBUF_RETURN_IF_FAIL(mbuf->in_use == EINA_FALSE);

   if (mbuf->type == TYPE_TBM && mbuf->tbm_surface)
     {
        tbm_surface_internal_unref(mbuf->tbm_surface);
        mbuf->tbm_surface = NULL;
     }

   mbuf_lists = eina_list_remove(mbuf_lists, mbuf);

   mbuf->stamp = 0;

   if (mbuf->func)
     {
        free(mbuf->func);
     }

   free(mbuf);
}

static void
_e_devmgr_buffer_cb_destroy(struct wl_listener *listener, void *data)
{
   E_Devmgr_Buf *mbuf = container_of(listener, E_Devmgr_Buf, destroy_listener);

   if (!mbuf) return;

   mbuf->comp_buffer = NULL;

   if (mbuf->buffer_destroying == EINA_FALSE)
     {
       mbuf->destroy_listener.notify = NULL;
       e_devmgr_buffer_free(mbuf);
     }
}

void
e_devmgr_buffer_clear(E_Devmgr_Buf *mbuf)
{
   EINA_SAFETY_ON_NULL_RETURN(mbuf);

   if (!_e_devicemgr_buffer_access_data_begin(mbuf))
     {
        BER("can't access ptr");
        return;
     }

   switch(mbuf->tbmfmt)
     {
      case TBM_FORMAT_ARGB8888:
      case TBM_FORMAT_XRGB8888:
        memset(mbuf->ptrs[0], 0, mbuf->pitches[0] * mbuf->height);
        break;
      case TBM_FORMAT_YVU420:
      case TBM_FORMAT_YUV420:
        memset((char*)mbuf->ptrs[0] + mbuf->offsets[0], 0x10, mbuf->pitches[0] * mbuf->height);
        memset((char*)mbuf->ptrs[1] + mbuf->offsets[1], 0x80, mbuf->pitches[1] * (mbuf->height >> 1));
        memset((char*)mbuf->ptrs[2] + mbuf->offsets[2], 0x80, mbuf->pitches[2] * (mbuf->height >> 1));
        break;
      case TBM_FORMAT_NV12:
      case TBM_FORMAT_NV21:
        memset((char*)mbuf->ptrs[0] + mbuf->offsets[0], 0x10, mbuf->pitches[0] * mbuf->height);
        memset((char*)mbuf->ptrs[1] + mbuf->offsets[1], 0x80, mbuf->pitches[1] * (mbuf->height >> 1));
        break;
      case TBM_FORMAT_YUYV:
        {
           int *ibuf = (int*)mbuf->ptrs[0];
           int i, size = mbuf->pitches[0] * mbuf->height / 4;

           for (i = 0 ; i < size ; i++)
             ibuf[i] = 0x10801080;
        }
        break;
      case TBM_FORMAT_UYVY:
        {
           int *ibuf = (int*)mbuf->ptrs[0];
           int i, size = mbuf->pitches[0] * mbuf->height / 4;

           for (i = 0 ; i < size ; i++)
             ibuf[i] = 0x80108010; /* YUYV -> 0xVYUY */
        }
        break;
      default:
        BWR("can't clear %c%c%c%c buffer", FOURCC_STR(mbuf->tbmfmt));
        break;
     }

   _e_devicemgr_buffer_access_data_end(mbuf);
}

Eina_Bool
_e_devmgr_buffer_valid(E_Devmgr_Buf *mbuf, const char *func)
{
   E_Devmgr_Buf *temp;
   Eina_List *l;

   MBUF_RETURN_VAL_IF_FAIL(mbuf != NULL, EINA_FALSE);
   MBUF_RETURN_VAL_IF_FAIL(mbuf->stamp != 0, EINA_FALSE);

   EINA_LIST_FOREACH(mbuf_lists, l, temp)
     {
        if (temp->stamp == mbuf->stamp)
            return EINA_TRUE;
     }

   BDB("mbuf(%p) invalid", mbuf);

   return EINA_FALSE;
}

static MBufFreeFuncInfo*
_e_devmgr_buffer_free_func_find(E_Devmgr_Buf *mbuf, MBuf_Free_Func func, void *data)
{
   MBufFreeFuncInfo *info;
   Eina_List *l;

   EINA_LIST_FOREACH(mbuf->free_funcs, l, info)
     {
        if (info->func == func && info->data == data)
            return info;
     }

   return NULL;
}

void
e_devmgr_buffer_free_func_add(E_Devmgr_Buf *mbuf, MBuf_Free_Func func, void *data)
{
   MBufFreeFuncInfo *info;

   EINA_SAFETY_ON_FALSE_RETURN(MBUF_IS_VALID(mbuf));
   EINA_SAFETY_ON_NULL_RETURN(func);

   info = _e_devmgr_buffer_free_func_find(mbuf, func, data);
   if (info)
     return;

   info = calloc(1, sizeof(MBufFreeFuncInfo));
   EINA_SAFETY_ON_NULL_RETURN(info);

   info->func = func;
   info->data = data;

   mbuf->free_funcs = eina_list_append(mbuf->free_funcs, info);
}

void
e_devmgr_buffer_free_func_del(E_Devmgr_Buf *mbuf, MBuf_Free_Func func, void *data)
{
   MBufFreeFuncInfo *info;

   EINA_SAFETY_ON_FALSE_RETURN(MBUF_IS_VALID(mbuf));
   EINA_SAFETY_ON_NULL_RETURN(func);

   info = _e_devmgr_buffer_free_func_find(mbuf, func, data);
   if (!info)
     return;

   mbuf->free_funcs = eina_list_remove(mbuf->free_funcs, info);

   free(info);
}

static pixman_format_code_t
_e_devmgr_buffer_pixman_format_get(E_Devmgr_Buf *mbuf)
{
   switch(mbuf->tbmfmt)
     {
      case TBM_FORMAT_ARGB8888:
        return PIXMAN_a8r8g8b8;
      case TBM_FORMAT_XRGB8888:
        return PIXMAN_x8r8g8b8;
      default:
        return 0;
     }
   return 0;
}

void
e_devmgr_buffer_convert(E_Devmgr_Buf *srcbuf, E_Devmgr_Buf *dstbuf,
                        int sx, int sy, int sw, int sh,
                        int dx, int dy, int dw, int dh,
                        Eina_Bool over, int rotate, int hflip, int vflip)
{
   pixman_image_t *src_img = NULL, *dst_img = NULL;
   pixman_format_code_t src_format, dst_format;
   double scale_x, scale_y;
   int rotate_step;
   pixman_transform_t t;
   struct pixman_f_transform ft;
   pixman_op_t op;
   int src_stride, dst_stride;
   int buf_width;

   EINA_SAFETY_ON_FALSE_RETURN(MBUF_IS_VALID(srcbuf));
   EINA_SAFETY_ON_FALSE_RETURN(MBUF_IS_VALID(dstbuf));

   if (!_e_devicemgr_buffer_access_data_begin(srcbuf))
     return;
   if (!_e_devicemgr_buffer_access_data_begin(dstbuf))
     {
        _e_devicemgr_buffer_access_data_end(srcbuf);
        return;
     }

   /* not handle buffers which have 2 more gem handles */
   EINA_SAFETY_ON_NULL_GOTO(srcbuf->ptrs[0], cant_convert);
   EINA_SAFETY_ON_NULL_GOTO(dstbuf->ptrs[0], cant_convert);
   EINA_SAFETY_ON_FALSE_RETURN(!srcbuf->ptrs[1]);
   EINA_SAFETY_ON_FALSE_RETURN(!dstbuf->ptrs[1]);

   src_format = _e_devmgr_buffer_pixman_format_get(srcbuf);
   EINA_SAFETY_ON_FALSE_GOTO(src_format > 0, cant_convert);
   dst_format = _e_devmgr_buffer_pixman_format_get(dstbuf);
   EINA_SAFETY_ON_FALSE_GOTO(dst_format > 0, cant_convert);

   buf_width = IS_RGB(srcbuf->tbmfmt)?(srcbuf->pitches[0]/4):srcbuf->pitches[0];
   src_stride = IS_RGB(srcbuf->tbmfmt)?(srcbuf->pitches[0]):buf_width * (PIXMAN_FORMAT_BPP(src_format) / 8);
   src_img = pixman_image_create_bits(src_format, buf_width, srcbuf->height,
                                      (uint32_t*)srcbuf->ptrs[0], src_stride);
   EINA_SAFETY_ON_NULL_GOTO(src_img, cant_convert);

   buf_width = IS_RGB(dstbuf->tbmfmt)?(dstbuf->pitches[0]/4):dstbuf->pitches[0];
   dst_stride = IS_RGB(srcbuf->tbmfmt)?(dstbuf->pitches[0]):buf_width * (PIXMAN_FORMAT_BPP(dst_format) / 8);
   dst_img = pixman_image_create_bits(dst_format, buf_width, dstbuf->height,
                                      (uint32_t*)dstbuf->ptrs[0], dst_stride);
   EINA_SAFETY_ON_NULL_GOTO(dst_img, cant_convert);

   pixman_f_transform_init_identity(&ft);

   if (hflip)
     {
        pixman_f_transform_scale(&ft, NULL, -1, 1);
        pixman_f_transform_translate(&ft, NULL, dw, 0);
     }

   if (vflip)
     {
        pixman_f_transform_scale(&ft, NULL, 1, -1);
        pixman_f_transform_translate(&ft, NULL, 0, dh);
     }

   rotate_step = (rotate + 360) / 90 % 4;
   if (rotate_step > 0)
     {
        int c, s, tx = 0, ty = 0;
        switch (rotate_step)
          {
           case 1:
              c = 0, s = -1, tx = -dw;
              break;
           case 2:
              c = -1, s = 0, tx = -dw, ty = -dh;
              break;
           case 3:
              c = 0, s = 1, ty = -dh;
              break;
          }
        pixman_f_transform_translate(&ft, NULL, tx, ty);
        pixman_f_transform_rotate(&ft, NULL, c, s);
     }

   if (rotate_step % 2 == 0)
     {
        scale_x = (double)sw / dw;
        scale_y = (double)sh / dh;
     }
   else
     {
        scale_x = (double)sw / dh;
        scale_y = (double)sh / dw;
     }

   pixman_f_transform_scale(&ft, NULL, scale_x, scale_y);
   pixman_f_transform_translate(&ft, NULL, sx, sy);
   pixman_transform_from_pixman_f_transform(&t, &ft);
   pixman_image_set_transform(src_img, &t);

   if (!over) op = PIXMAN_OP_SRC;
   else op = PIXMAN_OP_OVER;

   pixman_image_composite(op, src_img, NULL, dst_img, 0, 0, 0, 0,
                          dx, dy, dw, dh);
cant_convert:
   if (src_img) pixman_image_unref(src_img);
   if (dst_img) pixman_image_unref(dst_img);

   _e_devicemgr_buffer_access_data_end(srcbuf);
   _e_devicemgr_buffer_access_data_end(dstbuf);
}

Eina_Bool
e_devmgr_buffer_copy(E_Devmgr_Buf *srcbuf, E_Devmgr_Buf *dstbuf)
{
   int i, j, c_height;
   unsigned char *s, *d;

   EINA_SAFETY_ON_FALSE_RETURN_VAL(MBUF_IS_VALID(srcbuf), EINA_FALSE);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(MBUF_IS_VALID(dstbuf), EINA_FALSE);

   if (!_e_devicemgr_buffer_access_data_begin(srcbuf))
     return EINA_FALSE;
   if (!_e_devicemgr_buffer_access_data_begin(dstbuf))
     {
        _e_devicemgr_buffer_access_data_end(srcbuf);
        return EINA_FALSE;
     }

   switch (srcbuf->tbmfmt)
     {
      case TBM_FORMAT_ARGB8888:
      case TBM_FORMAT_XRGB8888:
      case TBM_FORMAT_YUV422:
      case TBM_FORMAT_YVU422:
        s = (unsigned char*)srcbuf->ptrs[0];
        d = (unsigned char*)dstbuf->ptrs[0];
        for (i = 0; i < srcbuf->height; i++)
          {
             memcpy(d, s, srcbuf->pitches[0]);
             s += srcbuf->pitches[0];
             d += dstbuf->pitches[0];
          }
        break;
      case TBM_FORMAT_YUV420:
      case TBM_FORMAT_YVU420:
        for (i = 0; i < 3; i++)
          {
             s = (unsigned char*)srcbuf->ptrs[i] + srcbuf->offsets[i];
             d = (unsigned char*)dstbuf->ptrs[i] + dstbuf->offsets[i];
             c_height = (i == 0) ? srcbuf->height : srcbuf->height / 2;
             for (j = 0; j < c_height; j++)
               {
                  memcpy(d, s, srcbuf->pitches[i]);
                  s += srcbuf->pitches[i];
                  d += dstbuf->pitches[i];
               }
          }
        break;
      case TBM_FORMAT_NV12:
      case TBM_FORMAT_NV21:
        for (i = 0; i < 2; i++)
          {
             s = (unsigned char*)srcbuf->ptrs[i] + srcbuf->offsets[i];
             d = (unsigned char*)dstbuf->ptrs[i] + dstbuf->offsets[i];
             c_height = (i == 0) ? srcbuf->height : srcbuf->height / 2;
             for (j = 0; j < c_height; j++)
               {
                  memcpy(d, s, srcbuf->pitches[i]);
                  s += srcbuf->pitches[i];
                  d += dstbuf->pitches[i];
               }
          }
        break;
      default:
        ERR("not implemented for %c%c%c%c", FOURCC_STR(srcbuf->tbmfmt));
        _e_devicemgr_buffer_access_data_end(srcbuf);
        _e_devicemgr_buffer_access_data_end(dstbuf);

        return EINA_FALSE;
     }

   _e_devicemgr_buffer_access_data_end(srcbuf);
   _e_devicemgr_buffer_access_data_end(dstbuf);

   return EINA_TRUE;
}

typedef struct _ColorTable
{
   tbm_format tbmfmt;
   E_Devmgr_Buf_Color_Type type;
} ColorTable;

static ColorTable color_table[] =
{
   { TBM_FORMAT_ARGB8888,  TYPE_RGB    },
   { TBM_FORMAT_XRGB8888,  TYPE_RGB    },
   { TBM_FORMAT_YVU420,    TYPE_YUV420 },
   { TBM_FORMAT_YUV420,    TYPE_YUV420 },
   { TBM_FORMAT_NV12MT,    TYPE_YUV420 },
   { TBM_FORMAT_NV12,      TYPE_YUV420 },
   { TBM_FORMAT_NV21,      TYPE_YUV420 },
   { TBM_FORMAT_YUYV,      TYPE_YUV422 },
   { TBM_FORMAT_UYVY,      TYPE_YUV422 },
};

E_Devmgr_Buf_Color_Type
e_devmgr_buffer_color_type(tbm_format tbmfmt)
{
   int i, size;

   size = sizeof(color_table) / sizeof(ColorTable);

   for (i = 0; i < size; i++)
     if (color_table[i].tbmfmt == tbmfmt)
       return color_table[i].type;

   return TYPE_NONE;
}

static void
_dump_raw(const char * file, void *data1, int size1, void *data2, int size2, void *data3, int size3)
{
   unsigned int * blocks;
   FILE * fp = fopen(file, "w+");
   EINA_SAFETY_ON_NULL_RETURN(fp);

   blocks = (unsigned int*)data1;
   fwrite(blocks, 1, size1, fp);

   if (size2 > 0)
     {
        blocks = (unsigned int*)data2;
        fwrite(blocks, 1, size2, fp);
     }

   if (size3 > 0)
     {
        blocks = (unsigned int*)data3;
        fwrite(blocks, 1, size3, fp);
     }

   fclose(fp);
}

static void
_dump_png(const char* file, const void * data, int width, int height)
{
   FILE *fp = fopen(file, "wb");
   EINA_SAFETY_ON_NULL_RETURN(fp);

   png_structp pPngStruct = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
   if (!pPngStruct)
     {
        fclose(fp);
        return;
     }

   png_infop pPngInfo = png_create_info_struct(pPngStruct);
   if (!pPngInfo)
     {
        png_destroy_write_struct(&pPngStruct, NULL);
        fclose(fp);
        return;
     }

   png_init_io(pPngStruct, fp);
   png_set_IHDR(pPngStruct,
                pPngInfo,
                width,
                height,
                PNG_DEPTH,
                PNG_COLOR_TYPE_RGBA,
                PNG_INTERLACE_NONE,
                PNG_COMPRESSION_TYPE_DEFAULT,
                PNG_FILTER_TYPE_DEFAULT);

   png_set_bgr(pPngStruct);
   png_write_info(pPngStruct, pPngInfo);

   const int pixel_size = 4; // RGBA
   png_bytep *row_pointers = png_malloc(pPngStruct, height * sizeof(png_byte *));

   unsigned int* blocks = (unsigned int*)data;
   int y = 0;
   int x = 0;

   for (; y < height; ++y)
     {
        png_bytep row = png_malloc(pPngStruct, sizeof(png_byte)*width * pixel_size);
        row_pointers[y] = (png_bytep)row;
        for (x = 0; x < width; ++x)
          {
             unsigned int curBlock = blocks[y * width + x];
             row[x * pixel_size] = (curBlock & 0xFF);
             row[1 + x * pixel_size] = (curBlock >> 8) & 0xFF;
             row[2 + x * pixel_size] = (curBlock >> 16) & 0xFF;
             row[3 + x * pixel_size] = (curBlock >> 24) & 0xFF;
          }
     }

   png_write_image(pPngStruct, row_pointers);
   png_write_end(pPngStruct, pPngInfo);

   for (y = 0; y < height; y++)
     png_free(pPngStruct, row_pointers[y]);
   png_free(pPngStruct, row_pointers);

   png_destroy_write_struct(&pPngStruct, &pPngInfo);

   fclose(fp);
}

void
e_devmgr_buffer_dump(E_Devmgr_Buf *mbuf, const char *prefix, int nth, Eina_Bool raw)
{
   char path[128];
   const char *dir = "/tmp/dump";

   if (!mbuf) return;

   if (!_e_devicemgr_buffer_access_data_begin(mbuf))
     {
        BER("can't access ptr");
        return;
     }

   if (IS_RGB(mbuf->tbmfmt))
     snprintf(path, sizeof(path), "%s/%s_%c%c%c%c_%dx%d_%03d.%s", dir, prefix,
              FOURCC_STR(mbuf->tbmfmt), mbuf->width_from_pitch, mbuf->height,
              nth, raw?"raw":"png");
   else
     snprintf(path, sizeof(path), "%s/%s_%c%c%c%c_%dx%d_%03d.yuv", dir, prefix,
              FOURCC_STR(mbuf->tbmfmt), mbuf->pitches[0], mbuf->height, nth);

   BDB("dump %s", path);

   switch(mbuf->tbmfmt)
     {
      case TBM_FORMAT_ARGB8888:
      case TBM_FORMAT_XRGB8888:
        if (raw)
          _dump_raw(path, mbuf->ptrs[0], mbuf->pitches[0] * mbuf->height, NULL, 0, NULL, 0);
        else
          _dump_png(path, mbuf->ptrs[0], mbuf->width_from_pitch, mbuf->height);
        break;
      case TBM_FORMAT_YVU420:
      case TBM_FORMAT_YUV420:
        _dump_raw(path,
                  (char*)mbuf->ptrs[0] + mbuf->offsets[0], mbuf->pitches[0] * mbuf->height,
                  (char*)mbuf->ptrs[1] + mbuf->offsets[1], mbuf->pitches[1] * (mbuf->height >> 1),
                  (char*)mbuf->ptrs[2] + mbuf->offsets[2], mbuf->pitches[2] * (mbuf->height >> 1));
        break;
      case TBM_FORMAT_NV12:
      case TBM_FORMAT_NV21:
        _dump_raw(path,
                  (char*)mbuf->ptrs[0] + mbuf->offsets[0], mbuf->pitches[0] * mbuf->height,
                  (((char*)mbuf->ptrs[1]) + mbuf->offsets[1]), mbuf->pitches[1] * (mbuf->height >> 1),
                  NULL, 0);
        break;
      case TBM_FORMAT_YUYV:
      case TBM_FORMAT_UYVY:
        _dump_raw(path,
                  (char*)mbuf->ptrs[0] + mbuf->offsets[0], mbuf->pitches[0] * mbuf->height,
                  NULL, 0,
                  NULL, 0);
        break;
      default:
        BWR("can't clear %c%c%c%c buffer", FOURCC_STR(mbuf->tbmfmt));
        break;
     }

   _e_devicemgr_buffer_access_data_end(mbuf);
}

uint
e_devmgr_buffer_get_mills(void)
{
   struct timespec tp;

   clock_gettime(CLOCK_MONOTONIC, &tp);

   return (tp.tv_sec * 1000) + (tp.tv_nsec / 1000000L);
}

int
e_devmgr_buffer_list_length(void)
{
   return eina_list_count(mbuf_lists);
}

void
e_devmgr_buffer_list_print(const char *log_path)
{
   E_Devmgr_Buf *mbuf;
   Eina_List *l;
   FILE *log_fl;

   log_fl = fopen(log_path, "a");
   if (!log_fl)
     {
        ERR("failed: open file(%s)", log_path);
        return;
     }

   setvbuf(log_fl, NULL, _IOLBF, 512);

   fprintf(log_fl, "* Devicemgr Buffers:\n");
   fprintf(log_fl, "stamp\tsize\tformat\thandles\tpitches\toffsets\tcreator\tin_use\n");
   EINA_LIST_FOREACH(mbuf_lists, l, mbuf)
     {
        fprintf(log_fl, "%d\t%dx%d\t%c%c%c%c\t%d,%d,%d\t%d,%d,%d\t%d,%d,%d\t%s\t%d\n",
                mbuf->stamp, mbuf->width, mbuf->height, FOURCC_STR(mbuf->tbmfmt),
                mbuf->handles[0], mbuf->handles[1], mbuf->handles[2],
                mbuf->pitches[0], mbuf->pitches[1], mbuf->pitches[2],
                mbuf->offsets[0], mbuf->offsets[1], mbuf->offsets[2],
                mbuf->func, mbuf->in_use);
     }

   fclose(log_fl);
}

void
e_devmgr_buffer_size_get(E_Client *ec, int *bw, int *bh)
{
   E_Comp_Wl_Buffer *buffer = e_pixmap_resource_get(ec->pixmap);

   *bw = *bh = 0;

   if (!buffer)
     {
        INF("no buffer. using ec(%p) size(%dx%d)", ec, ec->w, ec->h);
        *bw = ec->w;
        *bh = ec->h;
        return;
     }

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

void
e_devmgr_buffer_transform_scale_size_get(E_Client *ec, int *bw, int *bh)
{
   E_Comp_Wl_Buffer *buffer = e_pixmap_resource_get(ec->pixmap);
   E_Comp_Wl_Buffer_Viewport *vp = &ec->comp_data->scaler.buffer_viewport;
   int w, h, transform;

   *bw = *bh = 0;

   if (!buffer)
     {
        INF("no buffer. using ec(%p) size(%dx%d)", ec, ec->w, ec->h);
        *bw = ec->w;
        *bh = ec->h;
        return;
     }

   if (buffer->type == E_COMP_WL_BUFFER_TYPE_VIDEO)
     {
        tbm_surface_h tbm_surface = wayland_tbm_server_get_surface(NULL, buffer->resource);

        w = tbm_surface_get_width(tbm_surface);
        h = tbm_surface_get_height(tbm_surface);
     }
   else
     {
        w = buffer->w;
        h = buffer->h;
     }

   transform = e_comp_wl_output_buffer_transform_get(ec);

   switch (transform)
     {
      case WL_OUTPUT_TRANSFORM_90:
      case WL_OUTPUT_TRANSFORM_270:
      case WL_OUTPUT_TRANSFORM_FLIPPED_90:
      case WL_OUTPUT_TRANSFORM_FLIPPED_270:
        *bw = h / vp->buffer.scale;
        *bh = w / vp->buffer.scale;
        break;
      default:
        *bw = w / vp->buffer.scale;
        *bh = h / vp->buffer.scale;
        break;
     }
}
