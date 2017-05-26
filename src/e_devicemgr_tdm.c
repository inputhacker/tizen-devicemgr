#define E_COMP_WL
#include <e.h>
#include <Ecore_Drm.h>
#include <tdm_helper.h>
#include "e_mod_main.h"
#include "e_devicemgr_privates.h"
#include "e_devicemgr_tdm.h"
#include "e_devicemgr_dpms.h"

#define CHECKING_PRIMARY_ZPOS

E_DevMgr_Display *e_devmgr_dpy;
static Eina_List *e_devmgr_dpy_layers;

static int
_e_devicemgr_drm_fd_get(void)
{
   int fd;
   Eina_List *devs;
   Ecore_Drm_Device *dev;

   devs = eina_list_clone(ecore_drm_devices_get());
   EINA_SAFETY_ON_NULL_RETURN_VAL(devs, -1);

   if ((dev = eina_list_nth(devs, 0)))
     {
        fd = ecore_drm_device_fd_get(dev);
        if (fd >= 0)
          {
             eina_list_free(devs);
             return fd;
          }
     }

   eina_list_free(devs);
   return -1;
}

int
e_devicemgr_tdm_init(void)
{
   tdm_display_capability capabilities;
   tdm_error ret;

   if (e_devmgr_dpy)
   {
      ERR("tdm already init");
      return 1;
   }

   e_devmgr_dpy = calloc(1, sizeof(E_DevMgr_Display));
   if (!e_devmgr_dpy)
      return 0;

   e_devmgr_dpy->drm_fd = _e_devicemgr_drm_fd_get();
   if (e_devmgr_dpy->drm_fd < 0)
   {
      e_devicemgr_tdm_fini();
      return 0;
   }

   e_devmgr_dpy->bufmgr = tbm_bufmgr_init(e_devmgr_dpy->drm_fd);
   if (!e_devmgr_dpy->bufmgr)
     {
        ERR("bufmgr init failed");
        e_devicemgr_tdm_fini();
        return 0;
     }

   e_devmgr_dpy->tdm = tdm_display_init(NULL);
   if (!e_devmgr_dpy->tdm)
     {
        ERR("tdm init failed");
        e_devicemgr_tdm_fini();
        return 0;
     }

   ret = tdm_display_get_capabilities(e_devmgr_dpy->tdm, &capabilities);
   if (ret != TDM_ERROR_NONE)
     {
        ERR("tdm get_capabilities failed");
        e_devicemgr_tdm_fini();
        return 0;
     }

   if (capabilities & TDM_DISPLAY_CAPABILITY_PP)
      e_devmgr_dpy->pp_available = EINA_TRUE;

   if (capabilities & TDM_DISPLAY_CAPABILITY_CAPTURE)
      e_devmgr_dpy->capture_available = EINA_TRUE;

   e_comp->wl_comp_data->available_hw_accel.underlay = EINA_TRUE;
   DBG("enable HW underlay");

   e_comp->wl_comp_data->available_hw_accel.scaler = EINA_TRUE;
   DBG("enable HW scaler");

   return 1;
}

void
e_devicemgr_tdm_fini(void)
{
   e_comp->wl_comp_data->available_hw_accel.underlay = EINA_FALSE;
   e_comp->wl_comp_data->available_hw_accel.scaler = EINA_FALSE;

   if (e_devmgr_dpy->tdm)
      tdm_display_deinit(e_devmgr_dpy->tdm);

   if (e_devmgr_dpy->bufmgr)
      tbm_bufmgr_deinit(e_devmgr_dpy->bufmgr);

   if (e_devmgr_dpy->drm_fd >= 0)
      close(e_devmgr_dpy->drm_fd);

   free(e_devmgr_dpy);
   e_devmgr_dpy = NULL;
}

void
e_devicemgr_tdm_update(void)
{
   if (!e_devmgr_dpy || !e_devmgr_dpy->tdm)
      return;

   tdm_display_update(e_devmgr_dpy->tdm);
}

tdm_output*
e_devicemgr_tdm_output_get(Ecore_Drm_Output *output)
{
   Ecore_Drm_Device *dev;
   Ecore_Drm_Output *o;
   Eina_List *devs;
   Eina_List *l, *ll;
   int pipe = 0;

   if (!output)
     {
        int i, count = 0;
        tdm_display_get_output_count(e_devmgr_dpy->tdm, &count);
        for (i = 0; i < count; i++)
          {
             tdm_output *toutput = tdm_display_get_output(e_devmgr_dpy->tdm, i, NULL);
             tdm_output_conn_status status = TDM_OUTPUT_CONN_STATUS_DISCONNECTED;

             if (!toutput)
               continue;

             tdm_output_get_conn_status(toutput, &status);
             if (status != TDM_OUTPUT_CONN_STATUS_DISCONNECTED)
               {
                  tdm_output_type type;
                  tdm_output_get_output_type(toutput, &type);
                  INF("found tdm output: type(%d)", type);
                  return toutput;
               }
          }

        ERR("not found tdm output");

        return NULL;
     }

   devs = eina_list_clone(ecore_drm_devices_get());
   EINA_LIST_FOREACH(devs, l, dev)
     {
        pipe = 0;
        EINA_LIST_FOREACH(dev->outputs, ll, o)
          {
             if (o == output)
                goto found;
             pipe++;
          }
     }
found:
   eina_list_free(devs);

   return tdm_display_get_output(e_devmgr_dpy->tdm, pipe, NULL);
}

tdm_layer*
e_devicemgr_tdm_video_layer_get(tdm_output *output)
{
   int i, count = 0;
#ifdef CHECKING_PRIMARY_ZPOS
   int primary_idx = 0, primary_zpos = 0;
   tdm_layer *primary_layer;
#endif

   EINA_SAFETY_ON_NULL_RETURN_VAL(output, NULL);

   tdm_output_get_layer_count(output, &count);
   for (i = 0; i < count; i++)
     {
        tdm_layer *layer = tdm_output_get_layer(output, i, NULL);
        tdm_layer_capability capabilities = 0;
        EINA_SAFETY_ON_NULL_RETURN_VAL(layer, NULL);

        tdm_layer_get_capabilities(layer, &capabilities);
        if (capabilities & TDM_LAYER_CAPABILITY_VIDEO)
          return layer;
     }

#ifdef CHECKING_PRIMARY_ZPOS
   tdm_output_get_primary_index(output, &primary_idx);
   primary_layer = tdm_output_get_layer(output, primary_idx, NULL);
   EINA_SAFETY_ON_NULL_RETURN_VAL(primary_layer, NULL);
   tdm_layer_get_zpos(primary_layer, &primary_zpos);
#endif

   for (i = 0; i < count; i++)
     {
        tdm_layer *layer = tdm_output_get_layer(output, i, NULL);
        tdm_layer_capability capabilities = 0;
        EINA_SAFETY_ON_NULL_RETURN_VAL(layer, NULL);

        tdm_layer_get_capabilities(layer, &capabilities);
        if (capabilities & TDM_LAYER_CAPABILITY_OVERLAY)
          {
#ifdef CHECKING_PRIMARY_ZPOS
             int zpos = 0;
             tdm_layer_get_zpos(layer, &zpos);
             if (zpos >= primary_zpos) continue;
#endif
             return layer;
          }
     }

   return NULL;
}

tdm_layer*
e_devicemgr_tdm_avaiable_video_layer_get(tdm_output *output)
{
   Eina_Bool has_video_layer = EINA_FALSE;
   int i, count = 0;
#ifdef CHECKING_PRIMARY_ZPOS
   int primary_idx = 0, primary_zpos = 0;
   tdm_layer *primary_layer;
#endif

   EINA_SAFETY_ON_NULL_RETURN_VAL(output, NULL);

   /* check video layers first */
   tdm_output_get_layer_count(output, &count);
   for (i = 0; i < count; i++)
     {
        tdm_layer *layer = tdm_output_get_layer(output, i, NULL);
        tdm_layer_capability capabilities = 0;
        EINA_SAFETY_ON_NULL_RETURN_VAL(layer, NULL);

        tdm_layer_get_capabilities(layer, &capabilities);
        if (capabilities & TDM_LAYER_CAPABILITY_VIDEO)
          {
             has_video_layer = EINA_TRUE;
             if (!e_devicemgr_tdm_get_layer_usable(layer)) continue;
             return layer;
          }
     }

   /* if a output has video layers, it means that there is no available video layer for video */
   if (has_video_layer)
     return NULL;

   /* check graphic layers second */
#ifdef CHECKING_PRIMARY_ZPOS
   tdm_output_get_primary_index(output, &primary_idx);
   primary_layer = tdm_output_get_layer(output, primary_idx, NULL);
   EINA_SAFETY_ON_NULL_RETURN_VAL(primary_layer, NULL);
   tdm_layer_get_zpos(primary_layer, &primary_zpos);
#endif

   for (i = 0; i < count; i++)
     {
        tdm_layer *layer = tdm_output_get_layer(output, i, NULL);
        tdm_layer_capability capabilities = 0;
        EINA_SAFETY_ON_NULL_RETURN_VAL(layer, NULL);

        tdm_layer_get_capabilities(layer, &capabilities);
        if (capabilities & TDM_LAYER_CAPABILITY_OVERLAY)
          {
#ifdef CHECKING_PRIMARY_ZPOS
             int zpos = 0;
             tdm_layer_get_zpos(layer, &zpos);
             if (zpos >= primary_zpos) continue;
#endif
             if (!e_devicemgr_tdm_get_layer_usable(layer)) continue;
             return layer;
          }
     }

   return NULL;
}

void
e_devicemgr_tdm_set_layer_usable(tdm_layer *layer, Eina_Bool usable)
{
   if (usable)
     e_devmgr_dpy_layers = eina_list_remove(e_devmgr_dpy_layers, layer);
   else
     {
        tdm_layer *used_layer;
        Eina_List *l = NULL;
        EINA_LIST_FOREACH(e_devmgr_dpy_layers, l, used_layer)
          if (used_layer == layer) return;
        e_devmgr_dpy_layers = eina_list_append(e_devmgr_dpy_layers, layer);
     }
}

Eina_Bool
e_devicemgr_tdm_get_layer_usable(tdm_layer *layer)
{
   tdm_layer *used_layer;
   Eina_List *l = NULL;
   EINA_LIST_FOREACH(e_devmgr_dpy_layers, l, used_layer)
     if (used_layer == layer)
       return EINA_FALSE;
   return EINA_TRUE;
}
