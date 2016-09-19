#include "e.h"
#include "e_mod_main.h"
#include "e_devicemgr_privates.h"
#include "e_devicemgr_tdm.h"
#include "e_devicemgr_eom.h"
#include "eom-server-protocol.h"
#include <Ecore_Drm.h>
#include <tdm.h>
#include <eom.h>
#include <tbm_bufmgr.h>
#include <tbm_surface.h>
#include <wayland-tbm-server.h>
#ifdef FRAMES
#include <time.h>
#endif

#define ALEN(array) (sizeof(array) / sizeof(array)[0])

#define EOMER(msg, ARG...) ERR("[eom module][%s:%d] ERR: " msg "\n", __FUNCTION__, __LINE__, ##ARG)
#define EOMWR(msg, ARG...) WRN("[eom module][%s:%d] WRN: " msg "\n", __FUNCTION__, __LINE__, ##ARG)
#define EOMIN(msg, ARG...) INF("[eom module][%s:%d] INF: " msg "\n", __FUNCTION__, __LINE__, ##ARG)
#define EOMDB(msg, ARG...) DBG("[eom module][%s:%d] DBG: " msg "\n", __FUNCTION__, __LINE__, ##ARG)

#define EOM_NUM_ATTR 3
#define EOM_CONNECT_CHECK_TIMEOUT 7.0
#define EOM_DELAY_CHECK_TIMEOUT 4.0

typedef struct _E_Eom E_Eom, *E_EomPtr;
typedef struct _E_Eom_Out_Mode E_EomOutMode, *E_EomOutModePtr;
typedef struct _E_Eom_Output E_EomOutput, *E_EomOutputPtr;
typedef struct _E_Eom_Client E_EomClient, *E_EomClientPtr;
typedef struct _E_Eom_Comp_Object_Intercept_Hook_Data E_EomCompObjectInterceptHookData;
typedef struct _E_Eom_Output_Buffer E_EomOutputBuffer, *E_EomOutputBufferPtr;
typedef struct _E_Eom_Buffer E_EomBuffer, *E_EomBufferPtr;
typedef void(*E_EomEndShowingEventPtr)(E_EomOutputPtr eom_output, tbm_surface_h srfc, void * user_data);

typedef enum {
   NONE,
   MIRROR,
   PRESENTATION,
   WAIT_PRESENTATION,    /* It is used for delayed runnig of Presentation mode */
} E_EomOutputState;

struct _E_Eom
{
   struct wl_global *global;

   tdm_display *dpy;
   tbm_bufmgr bufmgr;
   int fd;

   unsigned int output_count;
   Eina_List *outputs;
   Eina_List *clients;
   Eina_List *handlers;
   Eina_List *hooks;
   Eina_List *comp_object_intercept_hooks;

   /* Internal output data */
   int main_output_state;
   char *main_output_name;
   int width;
   int height;
   char check_first_boot;
   Ecore_Timer *timer;
};

struct _E_Eom_Output
{
   unsigned int id;
   eom_output_type_e type;
   eom_output_mode_e mode;
   unsigned int width;
   unsigned int height;
   unsigned int phys_width;
   unsigned int phys_height;

   const char *name;

   tdm_output *output;
   tdm_layer *layer;
   tdm_pp *pp;

   E_EomOutputState state;
   tdm_output_conn_status status;
   eom_output_attribute_e attribute;
   eom_output_attribute_state_e attribute_state;
   enum wl_eom_status connection;

   /* mirror mode data */
   tbm_surface_queue_h pp_queue;
   /* dst surface in current pp process*/
   tbm_surface_h pp_dst_surface;
   /* src surface in current pp process*/
   tbm_surface_h pp_src_surface;

   /* output buffers*/
   Eina_List * pending_buff;       /* can be deleted any time */
   E_EomOutputBufferPtr wait_buff; /* wait end of commit, can't be deleted */
   E_EomOutputBufferPtr show_buff; /* current showed buffer, can be deleted only after commit event with different buff */

   /* If attribute has been set while external output is disconnected
    * then show black screen and wait until EOM client start sending
    * buffers. After expiring of the delay start mirroring */
   Ecore_Timer *delay;
   Ecore_Timer *watchdog;
};

struct _E_Eom_Client
{
   struct wl_resource *resource;
   Eina_Bool current;

   /* EOM output the client related to */
   int output_id;
   /* E_Client the client related to */
   E_Client *ec;
};

struct _E_Eom_Output_Buffer
{
   E_EomOutputPtr eom_output;
   tbm_surface_h tbm_surface;
   E_EomEndShowingEventPtr cb_func;
   void *cb_user_data;
};

struct _E_Eom_Buffer
{
   E_Comp_Wl_Buffer *wl_buffer;
   E_Comp_Wl_Buffer_Ref comp_wl_buffer_ref;

   /* double reference to avoid sigterm crash */
   E_Comp_Wl_Buffer_Ref comp_wl_buffer_ref_2;
};

struct _E_Eom_Comp_Object_Intercept_Hook_Data
{
   E_Client *ec;
   E_Comp_Object_Intercept_Hook *hook;
};

/*
 * EOM Output Attributes
 * +-----------------+------------+-----------------+------------+
 * |                 |   normal   | exclusive_share | exclusive  |
 * +-----------------+------------+-----------------+------------+
 * | normal          |  possible  |    possible     |  possible  |
 * +-----------------+------------+-----------------+------------+
 * | exclusive_share | impossible |    possible     |  possible  |
 * +-----------------+------------+-----------------+------------+
 * | exclusive       | impossible |   impossible    | impossible |
 * +-----------------+------------+-----------------+------------+
 *
 * possible   = 1
 * impossible = 0
 */
static int eom_output_attributes[EOM_NUM_ATTR][EOM_NUM_ATTR] =
{
   {1, 1, 1},
   {0, 1, 1},
   {0, 0, 0},
};

static const char *eom_conn_types[] =
{
   "None", "VGA", "DVI-I", "DVI-D", "DVI-A",
   "Composite", "S-Video", "LVDS", "Component", "DIN",
   "DisplayPort", "HDMI-A", "HDMI-B", "TV", "eDP", "Virtual",
   "DSI",
};

static E_EomPtr g_eom = NULL;

static void _e_eom_cb_dequeuable(tbm_surface_queue_h queue, void *user_data);
static void _e_eom_cb_pp(tbm_surface_h surface, void *user_data);

static E_EomOutputBufferPtr
_e_eom_output_buff_create( E_EomOutputPtr eom_output, tbm_surface_h tbm_surface, E_EomEndShowingEventPtr cb_func, void *cb_user_data)
{
   E_EomOutputBufferPtr outbuff = E_NEW(E_EomOutputBuffer, 1);
   EINA_SAFETY_ON_NULL_RETURN_VAL(outbuff, NULL);
   EINA_SAFETY_ON_NULL_RETURN_VAL(tbm_surface, NULL);

   EOMDB("Allocate output buffer:%p", outbuff);

   outbuff->eom_output = eom_output;

   tbm_surface_internal_ref(tbm_surface);
   outbuff->tbm_surface = tbm_surface;

   outbuff->cb_func = cb_func;
   outbuff->cb_user_data = cb_user_data;

   return outbuff;
}

static void
_e_eom_output_buff_delete( E_EomOutputBufferPtr buff)
{
   if (buff)
     {
        tbm_surface_internal_unref(buff->tbm_surface);
        if (buff->cb_func)
          buff->cb_func(buff->eom_output, buff->tbm_surface, buff->cb_user_data);
        E_FREE(buff);
     }
}

static E_EomBuffer *
_e_eom_buffer_create(E_Comp_Wl_Buffer *wl_buffer)
{
   E_EomBuffer * eom_buffer = E_NEW(E_EomBuffer, 1);
   EINA_SAFETY_ON_NULL_RETURN_VAL(eom_buffer, NULL);

   eom_buffer->wl_buffer = wl_buffer;

   /* Forbid E sending 'wl_buffer_send_release' event to external clients */
   e_comp_wl_buffer_reference(&eom_buffer->comp_wl_buffer_ref, wl_buffer);

   /* double reference to avoid sigterm crash */
   e_comp_wl_buffer_reference(&eom_buffer->comp_wl_buffer_ref_2, wl_buffer);

   EOMDB("E_EomBuffer:%p wl_buffer:%p busy:%d", eom_buffer, wl_buffer, wl_buffer->busy);
   return eom_buffer;
}

static void
_e_eom_buffer_destroy(E_EomBuffer * eom_buffer)
{
   EINA_SAFETY_ON_NULL_RETURN(eom_buffer);

   EOMDB("wl_buffer:%p busy:%d", eom_buffer->wl_buffer, eom_buffer->wl_buffer->busy);

   eom_buffer->wl_buffer = NULL;

   e_comp_wl_buffer_reference(&eom_buffer->comp_wl_buffer_ref, NULL);

   /* double reference to avoid sigterm crash */
   e_comp_wl_buffer_reference(&eom_buffer->comp_wl_buffer_ref_2, NULL);

   E_FREE(eom_buffer);
}

static inline eom_output_mode_e
_e_eom_output_state_get_mode(E_EomOutputPtr output)
{
   if (output == NULL)
     return EOM_OUTPUT_MODE_NONE;
   return output->mode;
}

static inline void
_e_eom_output_state_set_mode(E_EomOutputPtr output, eom_output_mode_e mode)
{
   if (output == NULL)
     return;
   output->mode = mode;
}

static inline eom_output_attribute_e
_e_eom_output_state_get_attribute_state(E_EomOutputPtr output)
{
   if (output == NULL)
     return EOM_OUTPUT_ATTRIBUTE_STATE_NONE;
   return output->attribute_state;
}

static inline void
_e_eom_output_attribute_state_set(E_EomOutputPtr output, eom_output_attribute_e attribute_state)
{
   if (output == NULL)
     return;
   output->attribute_state = attribute_state;
}

static inline eom_output_attribute_e
_e_eom_output_state_get_attribute(E_EomOutputPtr output)
{
   if (output == NULL)
     return EOM_OUTPUT_ATTRIBUTE_NONE;
   return output->attribute;
}

static inline void
_e_eom_output_state_set_force_attribute(E_EomOutputPtr output, eom_output_attribute_e attribute)
{
   if (output == NULL)
     return;
   output->attribute = attribute;
}

static inline Eina_Bool
_e_eom_output_state_set_attribute(E_EomOutputPtr output, eom_output_attribute_e attribute)
{
   if (output == NULL)
     return EINA_FALSE;

   if (attribute == EOM_OUTPUT_ATTRIBUTE_NONE || output->attribute == EOM_OUTPUT_ATTRIBUTE_NONE)
     {
        output->attribute = attribute;
        return EINA_TRUE;
     }

   if (eom_output_attributes[output->attribute - 1][attribute - 1] == 1)
     {
        output->attribute = attribute;
        return EINA_TRUE;
     }

   return EINA_FALSE;
}

static inline tdm_output_conn_status
_e_eom_output_state_get_status(E_EomOutputPtr output)
{
   if (output == NULL)
     return TDM_OUTPUT_CONN_STATUS_DISCONNECTED;
   return output->status;
}

static inline void
_e_eom_output_state_set_status(E_EomOutputPtr output, tdm_output_conn_status status)
{
   if (output == NULL)
     return;
   output->status = status;
}

static tdm_layer *
_e_eom_output_get_layer(E_EomOutputPtr eom_output)
{
   int i = 0;
   int count = 0;
   tdm_layer *layer = NULL;
   tdm_error err = TDM_ERROR_NONE;
   tdm_layer_capability capa;
   tdm_info_layer layer_info;

   EINA_SAFETY_ON_NULL_RETURN_VAL(eom_output, NULL);
   EINA_SAFETY_ON_NULL_RETURN_VAL(eom_output->output, NULL);

   err = tdm_output_get_layer_count(eom_output->output, &count);
   if (err != TDM_ERROR_NONE)
     {
        EOMDB ("tdm_output_get_layer_count fail(%d)", err);
        return NULL;
     }

   for (i = 0; i < count; i++)
     {
        layer = (tdm_layer *)tdm_output_get_layer(eom_output->output, i, &err);
        EINA_SAFETY_ON_FALSE_RETURN_VAL(err == TDM_ERROR_NONE, NULL);

        err = tdm_layer_get_capabilities(layer, &capa);
        EINA_SAFETY_ON_FALSE_RETURN_VAL(err == TDM_ERROR_NONE, NULL);

        if (capa & TDM_LAYER_CAPABILITY_PRIMARY)
          {
             EOMDB("TDM_LAYER_CAPABILITY_PRIMARY layer found : %d", i);
             break;
          }
     }

   memset(&layer_info, 0x0, sizeof(tdm_info_layer));
   layer_info.src_config.size.h = eom_output->width;
   layer_info.src_config.size.v = eom_output->height;
   layer_info.src_config.pos.x = 0;
   layer_info.src_config.pos.y = 0;
   layer_info.src_config.pos.w = eom_output->width;
   layer_info.src_config.pos.h = eom_output->height;
   layer_info.src_config.format = TBM_FORMAT_ARGB8888;
   layer_info.dst_pos.x = 0;
   layer_info.dst_pos.y = 0;
   layer_info.dst_pos.w = eom_output->width;
   layer_info.dst_pos.h = eom_output->height;
   layer_info.transform = TDM_TRANSFORM_NORMAL;

   err = tdm_layer_set_info(layer, &layer_info);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(err == TDM_ERROR_NONE, NULL);

   return layer;
}

static tbm_surface_h
_e_eom_util_get_output_surface(const char *name)
{
   Ecore_Drm_Output *primary_output = NULL;
   Ecore_Drm_Device *dev;
   const Eina_List *l;
   tbm_surface_h tbm = NULL;
   tdm_output *tdm_output_obj = NULL;
   tdm_layer *layer = NULL;
   tdm_layer_capability capabilities = 0;
   tdm_error err = TDM_ERROR_NONE;
   int count = 0, i = 0;

   EINA_LIST_FOREACH(ecore_drm_devices_get(), l, dev)
     {
        primary_output = ecore_drm_device_output_name_find(dev, name);
        if (primary_output != NULL)
          break;
     }

   if (primary_output == NULL)
     {
        EOMER("Get primary output.(%s)", name);
        EINA_LIST_FOREACH(ecore_drm_devices_get(), l, dev)
          {
             primary_output = ecore_drm_output_primary_get(dev);
             if (primary_output != NULL)
               break;
          }

        if (primary_output == NULL)
          {
             EOMER("Get primary output.(%s)", name);
             return NULL;
          }
     }

   tdm_output_obj = tdm_display_get_output(g_eom->dpy, 0, &err);
   if (tdm_output_obj == NULL || err != TDM_ERROR_NONE)
     {
        EOMER("tdm_display_get_output 0 fail");
        return NULL;
     }
   err = tdm_output_get_layer_count(tdm_output_obj, &count);
   if (err != TDM_ERROR_NONE)
     {
        EOMER("tdm_output_get_layer_count fail");
        return NULL;
     }

   for (i = 0; i < count; i++)
     {
        layer = tdm_output_get_layer(tdm_output_obj, i, NULL);
        tdm_layer_get_capabilities(layer, &capabilities);
        if (capabilities & TDM_LAYER_CAPABILITY_PRIMARY)
          {
             tbm = tdm_layer_get_displaying_buffer(layer, &err);
             if (err != TDM_ERROR_NONE)
               {
                  EOMER("tdm_layer_get_displaying_buffer fail");
                  return NULL;
               }
             break;
          }
     }

   return tbm;
}

static Eina_Bool
_e_eom_pp_is_needed(int src_w, int src_h, int dst_w, int dst_h)
{
   if (src_w != dst_w)
     return EINA_TRUE;

   if (src_h != dst_h)
     return EINA_TRUE;

   return EINA_FALSE;
}

static void
_e_eom_util_calculate_fullsize(int src_h, int src_v, int dst_size_h, int dst_size_v,
                              int *dst_x, int *dst_y, int *dst_w, int *dst_h)
{
   double h_ratio, v_ratio;

   h_ratio = src_h / dst_size_h;
   v_ratio = src_v / dst_size_v;

   if (h_ratio == v_ratio)
     {
        *dst_x = 0;
        *dst_y = 0;
        *dst_w = dst_size_h;
        *dst_h = dst_size_v;
     }
   else if (h_ratio < v_ratio)
     {
        *dst_y = 0;
        *dst_h = dst_size_v;
        *dst_w = dst_size_v * src_h / src_v;
        *dst_x = (dst_size_h - *dst_w) / 2;
     }
   else /* (h_ratio > v_ratio) */
     {
        *dst_x = 0;
        *dst_w = dst_size_h;
        *dst_h = dst_size_h * src_h / src_v;
        *dst_y = (dst_size_v - *dst_h) / 2;
     }
}

static void
_e_eom_tbm_buffer_release_mirror_mod(E_EomOutputPtr eom_output, tbm_surface_h surface, void * unused)
{
   EOMDB("release eom_output:%p, tbm_surface_h:%p data:%p", eom_output, surface, unused);
   tbm_surface_queue_release(eom_output->pp_queue, surface);
}

static void
_e_eom_cb_output_commit(tdm_output *output EINA_UNUSED, unsigned int sequence EINA_UNUSED,
                        unsigned int tv_sec EINA_UNUSED, unsigned int tv_usec EINA_UNUSED,
                        void *user_data)
{
   E_EomOutputBufferPtr outbuff = NULL;
   E_EomOutputPtr eom_output = NULL;
   tdm_error err = TDM_ERROR_NONE;

   EINA_SAFETY_ON_NULL_RETURN(user_data);
   outbuff = (E_EomOutputBufferPtr)user_data;

   eom_output = outbuff->eom_output;
   EINA_SAFETY_ON_NULL_RETURN(eom_output);

   EOMDB("========================>  CM  END     tbm_buff:%p", outbuff->tbm_surface);

   /*it means that eom_output has been canceled*/
   if(eom_output->wait_buff == NULL)
     {
        _e_eom_output_buff_delete(outbuff);
        return;
     }

   EINA_SAFETY_ON_FALSE_RETURN(eom_output->wait_buff == outbuff);

   EOMDB("commit finish tbm_surface_h:%p", outbuff->tbm_surface);

   /* check if show buffer is present */
   if(eom_output->show_buff != NULL)
     {
        EOMDB("delete show buffer tbm_surface_h:%p", eom_output->show_buff->tbm_surface);
        _e_eom_output_buff_delete(eom_output->show_buff);
        eom_output->show_buff = NULL;
     }

   /* set wait_buffer as show_buff; */
   EOMDB("set wait_buffer as show_buff tbm_surface_h:%p", outbuff->tbm_surface);
   eom_output->wait_buff = NULL;
   eom_output->show_buff = outbuff;

   /* check if pending buffer is present */
   outbuff = eina_list_nth(eom_output->pending_buff, 0);
   if (outbuff != NULL)
     {
        eom_output->pending_buff = eina_list_remove(eom_output->pending_buff, outbuff);

        EOMDB("========================>  CM- START   tbm_buff:%p", outbuff->tbm_surface);
        EOMDB("do commit tdm_output:%p tdm_layer:%p tbm_surface_h:%p", eom_output->output,
                eom_output->layer, outbuff->tbm_surface);
        err = tdm_layer_set_buffer(eom_output->layer, outbuff->tbm_surface);
        EINA_SAFETY_ON_FALSE_GOTO(err == TDM_ERROR_NONE, error);

        err = tdm_output_commit(eom_output->output, 0, _e_eom_cb_output_commit, outbuff);
        EINA_SAFETY_ON_FALSE_GOTO(err == TDM_ERROR_NONE, error);

        eom_output->wait_buff = outbuff;
     }

   return;

error:

   if (outbuff)
     {
        EOMDB("========================>  CM- ENDERR  tbm_buff:%p", outbuff);
        _e_eom_output_buff_delete(outbuff);
     }
}

static Eina_Bool
_e_eom_output_show(E_EomOutputPtr eom_output, tbm_surface_h tbm_srfc,
                        E_EomEndShowingEventPtr cb_func, void *cb_user_data)
{
   tdm_error err = TDM_ERROR_NONE;

   /* create new output buffer */
   E_EomOutputBufferPtr outbuff = _e_eom_output_buff_create(eom_output, tbm_srfc, cb_func, cb_user_data);
   EINA_SAFETY_ON_NULL_RETURN_VAL(outbuff, EINA_FALSE);

   /* chack if output free to commit */
   if (eom_output->wait_buff == NULL) /* do commit */
     {
        EOMDB("========================>  CM  START   tbm_buff:%p", tbm_srfc);
        EOMDB("do commit tdm_output:%p tdm_layer:%p tbm_surface_h:%p", eom_output->output,
            eom_output->layer, outbuff->tbm_surface);
        err = tdm_layer_set_buffer(eom_output->layer, outbuff->tbm_surface);
        EINA_SAFETY_ON_FALSE_GOTO(err == TDM_ERROR_NONE, error);

        err = tdm_output_commit(eom_output->output, 0, _e_eom_cb_output_commit, outbuff);
        EINA_SAFETY_ON_FALSE_GOTO(err == TDM_ERROR_NONE, error2);

        eom_output->wait_buff = outbuff;
     }
   else /* add to pending queue */
     {
        eom_output->pending_buff = eina_list_append(eom_output->pending_buff , outbuff);

        EOMDB("add to pending list tdm_output:%p tdm_layer:%p tbm_surface_h:%p",
                eom_output->output, eom_output->layer, outbuff->tbm_surface);
     }

   return EINA_TRUE;

error2:

   tdm_layer_unset_buffer(eom_output->layer);

error:

   if (outbuff)
     _e_eom_output_buff_delete(outbuff);
   EOMDB("========================>  CM  ENDERR  tbm_buff:%p", tbm_srfc);
   return EINA_FALSE;
}

static void
_e_eom_pp_run(E_EomOutputPtr eom_output)
{
   tdm_error tdm_err = TDM_ERROR_NONE;
   tbm_surface_h dst_surface = NULL;
   tbm_surface_h src_surface = NULL;

   if (g_eom->main_output_state == 0)
     return;

   /* If a client has committed its buffer stop mirror mode */
   if (eom_output->state != MIRROR)
     return;

   if (!eom_output->pp || !eom_output->pp_queue)
     return;

   if (tbm_surface_queue_can_dequeue(eom_output->pp_queue, 0) )
     {
        tdm_err = tbm_surface_queue_dequeue(eom_output->pp_queue, &dst_surface);
        EINA_SAFETY_ON_FALSE_GOTO(tdm_err == TDM_ERROR_NONE, error);

        EOMDB("============================>  PP  START   tbm_buff:%p", dst_surface);

        src_surface = _e_eom_util_get_output_surface(g_eom->main_output_name);
        tdm_err = TDM_ERROR_OPERATION_FAILED;
        EINA_SAFETY_ON_NULL_GOTO(src_surface, error);

        tdm_err = tdm_buffer_add_release_handler(dst_surface, _e_eom_cb_pp, eom_output);
        EINA_SAFETY_ON_FALSE_GOTO(tdm_err == TDM_ERROR_NONE, error);

        eom_output->pp_dst_surface = dst_surface;
        tbm_surface_internal_ref(dst_surface);
        eom_output->pp_src_surface = src_surface;
        tbm_surface_internal_ref(src_surface);
        tdm_err = tdm_pp_attach(eom_output->pp, src_surface, dst_surface);
        EINA_SAFETY_ON_FALSE_GOTO(tdm_err == TDM_ERROR_NONE, error);

        tdm_err = tdm_pp_commit(eom_output->pp);
        EINA_SAFETY_ON_FALSE_GOTO(tdm_err == TDM_ERROR_NONE, error);

        EOMDB("do pp commit tdm_output:%p tbm_surface_h(src:%p dst:%p)", eom_output->output, src_surface, dst_surface);
     }
   else
     {
        EOMDB("all pp buffers are busy, wait release queue");
        tbm_surface_queue_add_dequeuable_cb(eom_output->pp_queue, _e_eom_cb_dequeuable, eom_output);
     }

   return;

error:

   EOMER("failed run pp tdm error: %d", tdm_err);

   if (eom_output->pp_src_surface)
     {
        tbm_surface_internal_unref(eom_output->pp_src_surface);
        eom_output->pp_src_surface = NULL;
     }
   if (eom_output->pp_dst_surface)
     {
        tbm_surface_internal_unref(eom_output->pp_dst_surface);
        eom_output->pp_dst_surface = NULL;
     }

   if (dst_surface)
     {
        EOMDB("============================>  PP  ENDERR  tbm_buff:%p", dst_surface);
        tdm_buffer_remove_release_handler(dst_surface, _e_eom_cb_pp, eom_output);
        tbm_surface_queue_release(eom_output->pp_queue, dst_surface);
     }
}

static void
_e_eom_cb_pp(tbm_surface_h surface, void *user_data)
{
   E_EomOutputPtr eom_output = NULL;

   EINA_SAFETY_ON_NULL_RETURN(user_data);
   eom_output = (E_EomOutputPtr)user_data;

   tdm_buffer_remove_release_handler(surface, _e_eom_cb_pp, eom_output);

   if (eom_output->pp_src_surface)
     {
        tbm_surface_internal_unref(eom_output->pp_src_surface);
        eom_output->pp_src_surface = NULL;
     }
   if (eom_output->pp_dst_surface)
     {
        tbm_surface_internal_unref(eom_output->pp_dst_surface);
        eom_output->pp_dst_surface = NULL;
     }

   EOMDB("==============================>  PP  END     tbm_buff:%p", surface);

   if (eom_output->pp_queue == NULL)
     return;

   if (g_eom->main_output_state == 0)
     {
        tbm_surface_queue_release(eom_output->pp_queue, surface);
        return;
     }

   /* If a client has committed its buffer stop mirror mode */
   if (eom_output->state != MIRROR)
     {
        tbm_surface_queue_release(eom_output->pp_queue, surface);
        return;
     }

   if(!_e_eom_output_show(eom_output, surface, _e_eom_tbm_buffer_release_mirror_mod, NULL))
     {
        EOMER("_e_eom_add_buff_to_show fail");
        tbm_surface_queue_release(eom_output->pp_queue, surface);
     }

   _e_eom_pp_run(eom_output);

   EOMDB("==============================<  PP");
}

static void
_e_eom_cb_dequeuable(tbm_surface_queue_h queue, void *user_data)
{
   E_EomOutputPtr eom_output = (E_EomOutputPtr)user_data;
   EINA_SAFETY_ON_NULL_RETURN(user_data);

   EOMDB("release before in queue");

   tbm_surface_queue_remove_dequeuable_cb(eom_output->pp_queue, _e_eom_cb_dequeuable, eom_output);

   _e_eom_pp_run(eom_output);
}

static Eina_Bool
_e_eom_pp_init(E_EomOutputPtr eom_output)
{
   tdm_error err = TDM_ERROR_NONE;
   tdm_info_pp pp_info;
   tdm_pp *pp = NULL;
   int x, y, w, h;

   if (eom_output->pp == NULL)
     {
        /* TODO: Add support for other formats */
        eom_output->pp_queue = tbm_surface_queue_create(3, eom_output->width,eom_output->height,
                                                        TBM_FORMAT_ARGB8888, TBM_BO_SCANOUT);
        EINA_SAFETY_ON_NULL_RETURN_VAL(eom_output->pp_queue, EINA_FALSE);

        pp = tdm_display_create_pp(g_eom->dpy, &err);
        EINA_SAFETY_ON_FALSE_RETURN_VAL(err == TDM_ERROR_NONE, EINA_FALSE);

        eom_output->pp = pp;

        /* TODO : consider rotation */
        _e_eom_util_calculate_fullsize(g_eom->width, g_eom->height,
                                       eom_output->width, eom_output->height,
                                       &x, &y, &w, &h);

        EOMDB("PP calculation: x:%d, y:%d, w:%d, h:%d", x, y, w, h);

        pp_info.src_config.size.h = g_eom->width;
        pp_info.src_config.size.v = g_eom->height;
        pp_info.src_config.pos.x = 0;
        pp_info.src_config.pos.y = 0;
        pp_info.src_config.pos.w = g_eom->width;
        pp_info.src_config.pos.h = g_eom->height;
        pp_info.src_config.format = TBM_FORMAT_ARGB8888;

        pp_info.dst_config.size.h = eom_output->width;
        pp_info.dst_config.size.v = eom_output->height;
        pp_info.dst_config.pos.x = x;
        pp_info.dst_config.pos.y = y;
        pp_info.dst_config.pos.w = w;
        pp_info.dst_config.pos.h = h;
        pp_info.dst_config.format = TBM_FORMAT_ARGB8888;

        /* TO DO : get rotation */
        pp_info.transform = TDM_TRANSFORM_NORMAL;
        pp_info.sync = 0;
        pp_info.flags = 0;

        err = tdm_pp_set_info(pp, &pp_info);
        EINA_SAFETY_ON_FALSE_RETURN_VAL(err == TDM_ERROR_NONE, EINA_FALSE);
     }

   _e_eom_pp_run(eom_output);

   return EINA_TRUE;
}

static void
_e_eom_pp_deinit(E_EomOutputPtr eom_output)
{
   if (eom_output->pp_queue)
     {
        EOMDB("flush and destroy queue");
        tbm_surface_queue_flush(eom_output->pp_queue);
        tbm_surface_queue_destroy(eom_output->pp_queue);
        eom_output->pp_queue = NULL;
     }

   if (eom_output->pp)
     {
        tdm_pp_destroy(eom_output->pp);
        eom_output->pp = NULL;
     }
}

static E_EomClientPtr
_e_eom_client_get_current_by_id(int id)
{
   Eina_List *l;
   E_EomClientPtr client;

   EINA_LIST_FOREACH(g_eom->clients, l, client)
     {
        if (client &&
            client->current == EINA_TRUE &&
            client->output_id == id)
          return client;
     }

   return NULL;
}

static Eina_Bool
_e_eom_output_start_mirror(E_EomOutputPtr eom_output)
{
   tdm_layer *hal_layer;
   tdm_info_layer layer_info;
   tdm_error tdm_err = TDM_ERROR_NONE;
   int ret = 0;

   if (eom_output->state == MIRROR)
     return EINA_TRUE;

   hal_layer = _e_eom_output_get_layer(eom_output);
   EINA_SAFETY_ON_NULL_GOTO(hal_layer, err);

   if (!_e_eom_pp_is_needed(g_eom->width, g_eom->height, eom_output->width, eom_output->height))
     {
        /* TODO: Internal and external outputs are equal */
        EOMDB("internal and external outputs are equal");
     }

   tdm_err = tdm_layer_get_info(hal_layer, &layer_info);
   EINA_SAFETY_ON_FALSE_GOTO(tdm_err == TDM_ERROR_NONE, err);

   EOMDB("layer info: %dx%d, pos (x:%d, y:%d, w:%d, h:%d,  dpos (x:%d, y:%d, w:%d, h:%d))",
           layer_info.src_config.size.h,  layer_info.src_config.size.v,
           layer_info.src_config.pos.x, layer_info.src_config.pos.y,
           layer_info.src_config.pos.w, layer_info.src_config.pos.h,
           layer_info.dst_pos.x, layer_info.dst_pos.y,
           layer_info.dst_pos.w, layer_info.dst_pos.h);

   eom_output->layer = hal_layer;

   tdm_err = tdm_output_set_dpms(eom_output->output, TDM_OUTPUT_DPMS_ON);
   EINA_SAFETY_ON_FALSE_GOTO(tdm_err == TDM_ERROR_NONE, err);

   _e_eom_output_state_set_mode(eom_output, EOM_OUTPUT_MODE_MIRROR);
   eom_output->state = MIRROR;

   ret = _e_eom_pp_init(eom_output);
   EINA_SAFETY_ON_FALSE_GOTO(ret == EINA_TRUE, err);

   return EINA_TRUE;

err:

   _e_eom_output_state_set_mode(eom_output, EOM_OUTPUT_MODE_NONE);
   eom_output->state = NONE;

   return EINA_FALSE;
}

static void
_e_eom_output_start_presentation(E_EomOutputPtr eom_output)
{
   tdm_layer *hal_layer;
   tdm_info_layer layer_info;
   tdm_error tdm_err = TDM_ERROR_NONE;

   hal_layer = _e_eom_output_get_layer(eom_output);
   EINA_SAFETY_ON_NULL_GOTO(hal_layer, err);

   tdm_err = tdm_layer_get_info(hal_layer, &layer_info);
   EINA_SAFETY_ON_FALSE_GOTO(tdm_err == TDM_ERROR_NONE, err);

   EOMDB("layer info: %dx%d, pos (x:%d, y:%d, w:%d, h:%d,  dpos (x:%d, y:%d, w:%d, h:%d))",
           layer_info.src_config.size.h,  layer_info.src_config.size.v,
           layer_info.src_config.pos.x, layer_info.src_config.pos.y,
           layer_info.src_config.pos.w, layer_info.src_config.pos.h,
           layer_info.dst_pos.x, layer_info.dst_pos.y,
           layer_info.dst_pos.w, layer_info.dst_pos.h);

   eom_output->layer = hal_layer;

   tdm_err = tdm_output_set_dpms(eom_output->output, TDM_OUTPUT_DPMS_ON);
   EINA_SAFETY_ON_FALSE_GOTO(tdm_err == TDM_ERROR_NONE, err);

   return;

err:

   _e_eom_output_state_set_mode(eom_output, EOM_OUTPUT_MODE_NONE);
   eom_output->state = NONE;

   return;
}

static void
_e_eom_output_all_buff_release(E_EomOutputPtr eom_output)
{
   Eina_List *l, *ll;
   E_EomOutputBufferPtr  buff = NULL;

   EINA_LIST_FOREACH_SAFE(eom_output->pending_buff, l, ll, buff)
     {
        EOMDB("delete pending tbm_buff:%p", buff->tbm_surface);
        eom_output->pending_buff = eina_list_remove_list(eom_output->pending_buff, l);
        _e_eom_output_buff_delete(buff);
     }

   eom_output->wait_buff = NULL;

   EOMDB("delete show tbm_buff:%p", eom_output->show_buff ? eom_output->show_buff->tbm_surface : NULL);
   _e_eom_output_buff_delete(eom_output->show_buff);
   eom_output->show_buff = NULL;
}

static void
_e_eom_output_deinit(E_EomOutputPtr eom_output)
{
   tdm_error err = TDM_ERROR_NONE;

   if (eom_output->state == NONE)
     return;

   _e_eom_output_state_set_status(eom_output, TDM_OUTPUT_CONN_STATUS_DISCONNECTED);
   _e_eom_output_state_set_mode(eom_output, EOM_OUTPUT_MODE_NONE);
   eom_output->state = NONE;

   if (eom_output->layer)
     {
        err = tdm_layer_unset_buffer(eom_output->layer);
        if (err != TDM_ERROR_NONE)
          EOMDB("fail unset buffer:%d", err);

        err = tdm_output_commit(eom_output->output, 0, NULL, eom_output);
        if (err != TDM_ERROR_NONE)
          EOMDB ("fail commit on deleting output err:%d", err);
    }

   _e_eom_output_all_buff_release(eom_output);

   _e_eom_pp_deinit(eom_output);

   err = tdm_output_set_dpms(eom_output->output, TDM_OUTPUT_DPMS_OFF);
   if (err != TDM_ERROR_NONE)
     EOMER("set DPMS off:%d", err);
}

static const tdm_output_mode *
_e_eom_output_get_best_mode(tdm_output *output)
{
   tdm_error ret = TDM_ERROR_NONE;
   const tdm_output_mode *modes;
   const tdm_output_mode *mode = NULL;
   unsigned int best_value = 0;
   unsigned int value;
   int i, count = 0;

   ret = tdm_output_get_available_modes(output, &modes, &count);
   if (ret != TDM_ERROR_NONE)
     {
        EOMER("tdm_output_get_available_modes fail(%d)", ret);
        return NULL;
     }

   for (i = 0; i < count; i++)
     {
        value = modes[i].vdisplay + modes[i].hdisplay;
        if (value >= best_value)
          {
             best_value = value;
             mode = &modes[i];
          }
     }

   EOMDB("bestmode : %s, (%dx%d) r(%d), f(%d), t(%d)",
           mode->name, mode->hdisplay, mode->vdisplay,
           mode->vrefresh, mode->flags, mode->type);

   return mode;
}

static int
_e_eom_output_get_position(void)
{
   tdm_output *output_main = NULL;
   tdm_error ret = TDM_ERROR_NONE;
   const tdm_output_mode *mode;
   int x = 0;

   output_main = tdm_display_get_output(g_eom->dpy, 0, &ret);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(ret == TDM_ERROR_NONE, 0);
   EINA_SAFETY_ON_NULL_RETURN_VAL(output_main, 0);

   ret = tdm_output_get_mode(output_main, &mode);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(ret == TDM_ERROR_NONE, 0);

   if (mode == NULL)
     x = 0;
   else
     x = mode->hdisplay;

   if (g_eom->outputs)
     {
        Eina_List *l;
        E_EomOutputPtr eom_output_tmp;

        EINA_LIST_FOREACH(g_eom->outputs, l, eom_output_tmp)
          {
             if (eom_output_tmp->status != TDM_OUTPUT_CONN_STATUS_DISCONNECTED)
               x += eom_output_tmp->width;
          }
     }

   return x;
}

static Eina_Bool
_e_eom_timer_delayed_presentation_mode(void *data)
{
   E_EomOutputPtr eom_output = NULL;

   EOMDB("timer called %s", __FUNCTION__);

   EINA_SAFETY_ON_NULL_RETURN_VAL(data, ECORE_CALLBACK_CANCEL);

   eom_output = (E_EomOutputPtr )data;
   eom_output->delay = NULL;

   _e_eom_output_start_mirror(eom_output);

   return ECORE_CALLBACK_CANCEL;
}

static int
_e_eom_output_connected(E_EomOutputPtr eom_output)
{
   tdm_output *output;
   tdm_error ret = TDM_ERROR_NONE;
   E_EomClientPtr iterator = NULL;
   Eina_List *l;
   const tdm_output_mode *mode;
   const char *maker = NULL, *model = NULL, *name = NULL;
   unsigned int mmWidth, mmHeight, subpixel;
   int x = 0;

   output = eom_output->output;

   ret = tdm_output_get_model_info(output, &maker, &model, &name);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(ret == TDM_ERROR_NONE, -1);

   ret = tdm_output_get_physical_size(output, &mmWidth, &mmHeight);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(ret == TDM_ERROR_NONE, -1);

   ret = tdm_output_get_subpixel(output, &subpixel);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(ret == TDM_ERROR_NONE, -1);

   /* XXX: TMD returns not correct Primary mode for external output,
    * therefore we have to find it by ourself */
   mode = _e_eom_output_get_best_mode(output);
   EINA_SAFETY_ON_NULL_RETURN_VAL(mode, -1);

   ret = tdm_output_set_mode(output, mode);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(ret == TDM_ERROR_NONE, -1);

   x = _e_eom_output_get_position();
   EOMDB("mode: %dx%d, phy(%dx%d), pos(%d,0), refresh:%d, subpixel:%d",
           mode->hdisplay, mode->vdisplay, mmWidth, mmHeight, x, mode->vrefresh, subpixel);

   if (!e_comp_wl_output_init(eom_output->name, maker, eom_output->name, x, 0,
                              mode->hdisplay, mode->vdisplay,
                              mmWidth, mmHeight, mode->vrefresh, subpixel, 0))
     {
        EOMER("Could not setup new output: %s", eom_output->name);
        return -1;
     }

   EOMDB("Setup new output: %s", eom_output->name);

   /* update eom_output connect */
   eom_output->width = mode->hdisplay;
   eom_output->height = mode->vdisplay;
   eom_output->phys_width = mmWidth;
   eom_output->phys_height = mmHeight;

   /* TODO: check output mode(presentation set) and HDMI type */

   if (eom_output->state == WAIT_PRESENTATION)
     {
        EOMDB("Start Presentation");

        if (eom_output->delay)
          ecore_timer_del(eom_output->delay);
        eom_output->delay = ecore_timer_add(EOM_DELAY_CHECK_TIMEOUT, _e_eom_timer_delayed_presentation_mode, eom_output);

        _e_eom_output_start_presentation(eom_output);
     }
   else
     {
        EOMDB("Start Mirroring");
        _e_eom_output_start_mirror(eom_output);
     }

   eom_output->connection = WL_EOM_STATUS_CONNECTION;

   /* If there were previously connected clients to the output - notify them */
   EINA_LIST_FOREACH(g_eom->clients, l, iterator)
     {
        if (iterator)
          {
             EOMDB("Send MIRROR ON notification to clients");

             if (iterator->current)
               wl_eom_send_output_info(iterator->resource, eom_output->id,
                                       eom_output->type, eom_output->mode,
                                       eom_output->width, eom_output->height,
                                       eom_output->phys_width, eom_output->phys_height,
                                       eom_output->connection,
                                       0,
                                       _e_eom_output_state_get_attribute(eom_output),
                                       EOM_OUTPUT_ATTRIBUTE_STATE_ACTIVE,
                                       EOM_ERROR_NONE);
             else
               wl_eom_send_output_info(iterator->resource, eom_output->id,
                                       eom_output->type, eom_output->mode,
                                       eom_output->width, eom_output->height,
                                       eom_output->phys_width, eom_output->phys_height,
                                       eom_output->connection,
                                       1, 0, 0, 0);
          }
     }

   return 0;
}

static void
_e_eom_output_disconnected(E_EomOutputPtr eom_output)
{
   E_EomClientPtr iterator = NULL;
   Eina_List *l;

   if (eom_output->delay)
     ecore_timer_del(eom_output->delay);

   if (eom_output->watchdog)
     ecore_timer_del(eom_output->watchdog);

   /* update eom_output disconnect */
   eom_output->width = 0;
   eom_output->height = 0;
   eom_output->phys_width = 0;
   eom_output->phys_height = 0;
   eom_output->connection = WL_EOM_STATUS_DISCONNECTION;

   _e_eom_output_deinit(eom_output);

   /* If there were previously connected clients to the output - notify them */
   EINA_LIST_FOREACH(g_eom->clients, l, iterator)
     {
        if (iterator)
          {
             EOMDB("Send MIRROR OFF notification to client: %p", iterator);
             if (iterator->current)
               wl_eom_send_output_info(iterator->resource, eom_output->id,
                                       eom_output->type, eom_output->mode,
                                       eom_output->width, eom_output->height,
                                       eom_output->phys_width, eom_output->phys_height,
                                       eom_output->connection,
                                       0,
                                       _e_eom_output_state_get_attribute(eom_output),
                                       EOM_OUTPUT_ATTRIBUTE_STATE_INACTIVE,
                                       EOM_ERROR_NONE);
             else
               wl_eom_send_output_info(iterator->resource, eom_output->id,
                                       eom_output->type, eom_output->mode,
                                       eom_output->width, eom_output->height,
                                       eom_output->phys_width, eom_output->phys_height,
                                       eom_output->connection,
                                       1, 0, 0, 0);
          }
     }

   e_comp_wl_output_remove(eom_output->name);
   EOMDB("Destory output: %s", eom_output->name);
   eina_stringshare_del(eom_output->name);
   eom_output->name = NULL;
}

static void
_e_eom_cb_tdm_output_status_change(tdm_output *output, tdm_output_change_type type, tdm_value value, void *user_data)
{
   tdm_output_type tdm_type;
   tdm_output_conn_status status, status_check;
   tdm_error ret = TDM_ERROR_NONE;
   const char *tmp_name;
   char new_name[DRM_CONNECTOR_NAME_LEN];
   E_EomOutputPtr eom_output = NULL, eom_output_tmp = NULL;
   Eina_List *l;

   g_eom->check_first_boot = 1;

   if (type == TDM_OUTPUT_CHANGE_DPMS || g_eom->main_output_state == 0)
     return;

   if (g_eom->outputs)
     {
        EINA_LIST_FOREACH(g_eom->outputs, l, eom_output_tmp)
          {
             if (eom_output_tmp->output == output)
               eom_output = eom_output_tmp;
          }
     }

   EINA_SAFETY_ON_NULL_RETURN(eom_output);

   ret = tdm_output_get_output_type(output, &tdm_type);
   EINA_SAFETY_ON_FALSE_RETURN(ret == TDM_ERROR_NONE);

   ret = tdm_output_get_conn_status(output, &status_check);
   EINA_SAFETY_ON_FALSE_RETURN(ret == TDM_ERROR_NONE);

   status = value.u32;

   EOMDB("id (%d), type(%d, %d), status(%d, %d)", eom_output->id, type, tdm_type, status_check, status);

   eom_output->type = (eom_output_type_e)tdm_type;
   eom_output->status = status;

   if (status == TDM_OUTPUT_CONN_STATUS_CONNECTED)
     {
        if (tdm_type < ALEN(eom_conn_types))
          tmp_name = eom_conn_types[tdm_type];
        else
          tmp_name = "unknown";

        /* TODO: What if there will more then one output of same type.
         * e.g. "HDMI and HDMI" "LVDS and LVDS"*/
        snprintf(new_name, sizeof(new_name), "%s-%d", tmp_name, 0);

        eom_output->name = eina_stringshare_add(new_name);

        e_comp_override_add();

        _e_eom_output_connected(eom_output);
     }
   else if (status == TDM_OUTPUT_CONN_STATUS_DISCONNECTED)
     {
        e_comp_override_del();

        _e_eom_output_disconnected(eom_output);
     }
}

static Eina_Bool
_e_eom_output_init(tdm_display *dpy)
{
   E_EomOutputPtr new_output = NULL;
   tdm_output *output = NULL;
   tdm_output_type type;
   tdm_output_conn_status status;
   const tdm_output_mode *mode = NULL;
   tdm_error ret = TDM_ERROR_NONE;
   unsigned int mmWidth, mmHeight;
   int i, count;

   ret = tdm_display_get_output_count(dpy, &count);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(ret == TDM_ERROR_NONE, EINA_FALSE);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(count > 1, EINA_FALSE);

   g_eom->output_count = count - 1;
   EOMDB("external output count : %d", g_eom->output_count);

   /* skip main output id:0 */
   /* start from 1 */
   for (i = 1; i < count; i++)
     {
        output = tdm_display_get_output(dpy, i, &ret);
        EINA_SAFETY_ON_FALSE_GOTO(ret == TDM_ERROR_NONE, err);
        EINA_SAFETY_ON_NULL_GOTO(output, err);

        ret = tdm_output_get_output_type(output, &type);
        EINA_SAFETY_ON_FALSE_GOTO(ret == TDM_ERROR_NONE, err);

        new_output = E_NEW(E_EomOutput, 1);
        EINA_SAFETY_ON_NULL_GOTO(new_output, err);

        ret = tdm_output_get_conn_status(output, &status);
        if (ret != TDM_ERROR_NONE)
          {
             EOMER("tdm_output_get_conn_status fail(%d)", ret);
             free(new_output);
             goto err;
          }

        new_output->id = i;
        new_output->type = type;
        new_output->status = status;
        new_output->mode = EOM_OUTPUT_MODE_NONE;
        new_output->connection = WL_EOM_STATUS_NONE;
        new_output->output = output;

        ret = tdm_output_add_change_handler(output, _e_eom_cb_tdm_output_status_change, NULL);
        if (ret != TDM_ERROR_NONE)
          {
              EOMER("tdm_output_add_change_handler fail(%d)", ret);
              free(new_output);
              goto err;
          }

        if (status == TDM_OUTPUT_CONN_STATUS_DISCONNECTED)
          {
             EOMDB("create(%d)output, type:%d, status:%d",
                     new_output->id, new_output->type, new_output->status);
             g_eom->outputs = eina_list_append(g_eom->outputs, new_output);
             continue;
          }

        new_output->status = TDM_OUTPUT_CONN_STATUS_CONNECTED;

        ret = tdm_output_get_mode(output, &mode);
        if (ret != TDM_ERROR_NONE)
          {
             EOMER("tdm_output_get_mode fail(%d)", ret);
             free(new_output);
             goto err;
          }

        if (mode == NULL)
          {
             new_output->width = 0;
             new_output->height = 0;
          }
        else
          {
             new_output->width = mode->hdisplay;
             new_output->height = mode->vdisplay;
          }

        ret = tdm_output_get_physical_size(output, &mmWidth, &mmHeight);
        if (ret != TDM_ERROR_NONE)
          {
             EOMER("tdm_output_get_conn_status fail(%d)", ret);
             free(new_output);
             goto err;
          }

        new_output->phys_width = mmWidth;
        new_output->phys_height = mmHeight;

        EOMDB("create(%d)output, type:%d, status:%d, w:%d, h:%d, mm_w:%d, mm_h:%d",
                new_output->id, new_output->type, new_output->status,
                new_output->width, new_output->height, new_output->phys_width, new_output->phys_height);

        g_eom->outputs = eina_list_append(g_eom->outputs, new_output);
     }

   return EINA_TRUE;

err:
   if (g_eom->outputs)
     {
        Eina_List *l;
        E_EomOutputPtr output;

        EINA_LIST_FOREACH(g_eom->outputs, l, output)
          free(output);

        eina_list_free(g_eom->outputs);
     }

   return EINA_FALSE;
}

static Eina_Bool
_e_eom_init_internal()
{
   g_eom->dpy = e_devmgr_dpy->tdm;
   EINA_SAFETY_ON_NULL_GOTO(g_eom->dpy, err);

   g_eom->bufmgr = e_devmgr_dpy->bufmgr;
   EINA_SAFETY_ON_NULL_GOTO(g_eom->bufmgr, err);

   if (_e_eom_output_init(g_eom->dpy) != EINA_TRUE)
     {
        EOMER("_e_eom_output_init fail");
        goto err;
     }

   return EINA_TRUE;

err:

   if (g_eom->bufmgr)
     g_eom->bufmgr = NULL;

   if (g_eom->dpy)
     g_eom->dpy = NULL;

   return EINA_FALSE;
}

static void
_e_eom_deinit()
{
   Ecore_Event_Handler *h = NULL;

   if (g_eom == NULL) return;

   if (g_eom->handlers)
     {
        EINA_LIST_FREE(g_eom->handlers, h)
          ecore_event_handler_del(h);
     }

   if (g_eom->dpy)
     g_eom->dpy = NULL;

   if (g_eom->bufmgr)
     g_eom->bufmgr = NULL;

   if (g_eom->global)
     wl_global_destroy(g_eom->global);
   g_eom->global = NULL;

   E_FREE(g_eom);
}

static E_EomClientPtr
_e_eom_client_get_by_resource(struct wl_resource *resource)
{
   Eina_List *l;
   E_EomClientPtr client;

   EINA_LIST_FOREACH(g_eom->clients, l, client)
     {
        if (client && client->resource == resource)
          return client;
     }

   return NULL;
}

static E_EomOutputPtr
_e_eom_output_get_by_id(int id)
{
   Eina_List *l;
   E_EomOutputPtr output;

   EINA_LIST_FOREACH(g_eom->outputs, l, output)
     {
        if (output && output->id == id)
          return output;
     }

   return NULL;
}

static E_EomOutputPtr
_e_eom_output_by_ec_child_get(E_Client *ec)
{
   E_EomOutputPtr eom_output = NULL;
   E_EomClientPtr eom_client = NULL;
   E_Client *parent = NULL;
   Eina_List *l;

   EINA_LIST_FOREACH(g_eom->outputs, l, eom_output)
     {
        eom_client = _e_eom_client_get_current_by_id(eom_output->id);
        if (!eom_client)
          continue;

        if (eom_client->ec == ec)
          return eom_output;

        if (!ec->comp_data || !ec->comp_data->sub.data)
          continue;

        parent = ec->comp_data->sub.data->parent;
        while (parent)
          {
             if (parent == eom_client->ec)
               return eom_output;

             if (!parent->comp_data || !parent->comp_data->sub.data)
               break;

             parent = parent->comp_data->sub.data->parent;
          }
     }

   return NULL;
}

static void
_e_eom_cb_wl_eom_client_destory(struct wl_resource *resource)
{
   E_EomClientPtr client = NULL, iterator = NULL;
   E_EomOutputPtr output = NULL;
   Eina_List *l = NULL;
   Eina_Bool ret;

   EOMDB("=======================>  CLENT UNBIND");

   EINA_SAFETY_ON_NULL_RETURN(resource);

   client = _e_eom_client_get_by_resource(resource);
   EINA_SAFETY_ON_NULL_RETURN(client);

   g_eom->clients = eina_list_remove(g_eom->clients, client);

   if (client->current == EINA_FALSE)
     goto end2;

   output = _e_eom_output_get_by_id(client->output_id);
   EINA_SAFETY_ON_NULL_GOTO(output, end2);

   ret = _e_eom_output_state_set_attribute(output, EOM_OUTPUT_ATTRIBUTE_NONE);
   (void)ret;

   if (output->state == NONE)
     goto end;

   if (output->state == WAIT_PRESENTATION)
     {
        output->state = NONE;
        goto end;
     }

   /* If a client has been disconnected and mirror mode has not
    * been restored, start mirror mode
    */
   _e_eom_output_start_mirror(output);

end:

   /* Notify eom clients which are binded to a concrete output that the
    * state and mode of the output has been changed */
   EINA_LIST_FOREACH(g_eom->clients, l, iterator)
     {
        if (iterator && iterator != client && iterator->output_id == output->id)
          {
             wl_eom_send_output_attribute(iterator->resource, output->id,
                                          _e_eom_output_state_get_attribute(output),
                                          _e_eom_output_state_get_attribute_state(output),
                                          EOM_OUTPUT_MODE_NONE);

             wl_eom_send_output_mode(iterator->resource, output->id,
                                     _e_eom_output_state_get_mode(output));
          }
     }

end2:

   free(client);
}

static void
_e_eom_cb_wl_request_set_attribute(struct wl_client *client, struct wl_resource *resource, uint32_t output_id, uint32_t attribute)
{
   eom_error_e eom_error = EOM_ERROR_NONE;
   E_EomClientPtr eom_client = NULL, current_eom_client = NULL, iterator = NULL;
   E_EomOutputPtr eom_output = NULL;
   Eina_Bool ret = EINA_FALSE;
   Eina_List *l;

   eom_client = _e_eom_client_get_by_resource(resource);
   EINA_SAFETY_ON_NULL_RETURN(eom_client);

   /* Bind the client with a concrete output */
   eom_client->output_id = output_id;

   eom_output = _e_eom_output_get_by_id(output_id);
   EINA_SAFETY_ON_NULL_GOTO(eom_output, no_output);

   EOMDB("Set attribute:%d", attribute);

   if (eom_client->current == EINA_TRUE && eom_output->id == eom_client->output_id)
     {
        /* Current client can set any flag it wants */
        _e_eom_output_state_set_force_attribute(eom_output, attribute);
     }
   else if (eom_output->id == eom_client->output_id)
     {
        /* A client is trying to set new attribute */
        ret = _e_eom_output_state_set_attribute(eom_output, attribute);
        if (ret == EINA_FALSE)
          {
             EOMDB("set attribute FAILED");

             eom_error = EOM_ERROR_INVALID_PARAMETER;
             goto end;
          }
     }
   else
     return;

   /* If client has set EOM_OUTPUT_ATTRIBUTE_NONE switching to mirror mode */
   if (attribute == EOM_OUTPUT_ATTRIBUTE_NONE && eom_output->state != MIRROR)
     {
        eom_client->current = EINA_FALSE;

        _e_eom_output_state_set_mode(eom_output, EOM_OUTPUT_MODE_NONE);
        _e_eom_output_state_set_attribute(eom_output, EOM_OUTPUT_ATTRIBUTE_NONE);

        if (eom_output->status == TDM_OUTPUT_CONN_STATUS_DISCONNECTED)
          {
             EOMDB("output:%d is disconnected", output_id);
             goto end;
          }

        ret = _e_eom_output_start_mirror(eom_output);
        EINA_SAFETY_ON_FALSE_GOTO(ret == EINA_TRUE, end);

        /* If mirror mode has been ran notify all clients about that */
        EOMDB("client set NONE attribute, send new info to previous current client");
        EINA_LIST_FOREACH(g_eom->clients, l, iterator)
          {
             if (iterator && iterator->output_id == output_id)
               {
                  wl_eom_send_output_attribute(iterator->resource, eom_output->id,
                                               _e_eom_output_state_get_attribute(eom_output),
                                               _e_eom_output_state_get_attribute_state(eom_output),
                                               EOM_ERROR_NONE);

                  wl_eom_send_output_mode(iterator->resource, eom_output->id,
                                          _e_eom_output_state_get_mode(eom_output));
               }
          }

        return;
     }

end:

   /* If client was not able to set attribute send LOST event to it */
   if (eom_error == EOM_ERROR_INVALID_PARAMETER)
     {
        EOMDB("client failed to set attribute");

        wl_eom_send_output_attribute(eom_client->resource, eom_output->id,
                                     _e_eom_output_state_get_attribute(eom_output),
                                     EOM_OUTPUT_ATTRIBUTE_STATE_LOST,
                                     eom_error);
        return;
     }

   /* Send changes to the caller-client */
   wl_eom_send_output_attribute(eom_client->resource, eom_output->id,
                                _e_eom_output_state_get_attribute(eom_output),
                                _e_eom_output_state_get_attribute_state(eom_output),
                                eom_error);

   current_eom_client = _e_eom_client_get_current_by_id(eom_output->id);
   EOMDB("Substitute current client: new:%p, old:%p",eom_client, current_eom_client );

   /* Send changes to previous current client */
   if (eom_client->current == EINA_FALSE && current_eom_client)
     {
        current_eom_client->current = EINA_FALSE;

        /* Actually deleting of buffers right here is a hack intended to
         * send release events of buffers to current client, since it could
         * be locked until it get 'release' event */
        EOMDB("Send changes to previous current client, and delete buffers");
        _e_eom_output_all_buff_release(eom_output);

        wl_eom_send_output_attribute(current_eom_client->resource, eom_output->id,
                                     _e_eom_output_state_get_attribute(eom_output),
                                     EOM_OUTPUT_ATTRIBUTE_STATE_LOST,
                                     EOM_ERROR_NONE);
     }

   /* Set the client as current client of the eom_output */
   eom_client->current= EINA_TRUE;

   if (eom_output->status == TDM_OUTPUT_CONN_STATUS_DISCONNECTED)
     eom_output->state = WAIT_PRESENTATION;

   return;

/* Get here if EOM does not have output referred by output_id */
no_output:

   wl_eom_send_output_attribute(eom_client->resource, output_id,
                                EOM_OUTPUT_ATTRIBUTE_NONE,
                                EOM_OUTPUT_ATTRIBUTE_STATE_NONE,
                                EOM_ERROR_NO_SUCH_DEVICE);

   wl_eom_send_output_mode(eom_client->resource, output_id,
                           EOM_OUTPUT_MODE_NONE);

   wl_eom_send_output_type(eom_client->resource, output_id,
                           EOM_OUTPUT_ATTRIBUTE_STATE_NONE,
                           TDM_OUTPUT_CONN_STATUS_DISCONNECTED);
   return;
}

static Eina_Bool
_e_eom_cb_comp_object_redirected(void *data, E_Client *ec)
{
   E_EomCompObjectInterceptHookData *hook_data;

   EOMDB("_e_eom_cb_comp_object_redirected");
   EINA_SAFETY_ON_NULL_RETURN_VAL(data, EINA_TRUE);

   hook_data = (E_EomCompObjectInterceptHookData* )data;

   if (!hook_data->ec || !hook_data->hook)
     return EINA_TRUE;

   if (hook_data->ec != ec)
     return EINA_TRUE;

   /* Hide the window from Enlightenment main screen */
   e_client_redirected_set(ec, EINA_FALSE);

   e_comp_object_intercept_hook_del(hook_data->hook);

   g_eom->comp_object_intercept_hooks = eina_list_remove(g_eom->comp_object_intercept_hooks, hook_data);

   free(hook_data);

   return EINA_TRUE;
}

static Eina_Bool
_e_eom_util_add_comp_object_redirected_hook(E_Client *ec)
{
   E_EomCompObjectInterceptHookData *hook_data = NULL;
   E_Comp_Object_Intercept_Hook *hook = NULL;

   hook_data = E_NEW(E_EomCompObjectInterceptHookData, 1);
   EINA_SAFETY_ON_NULL_GOTO(hook_data, err);

   hook_data->ec = ec;

   hook = e_comp_object_intercept_hook_add(E_COMP_OBJECT_INTERCEPT_HOOK_SHOW_HELPER,
                                           _e_eom_cb_comp_object_redirected, hook_data);
   EINA_SAFETY_ON_NULL_GOTO(hook, err);

   hook_data->hook = hook;

   g_eom->comp_object_intercept_hooks = eina_list_append(g_eom->comp_object_intercept_hooks, hook_data);

   EOMDB("_e_eom_redirected_hook have been added");
   return EINA_TRUE;

err:

   if (hook_data)
     free(hook_data);
   return EINA_FALSE;
}

static void
_e_eom_window_set_internal(struct wl_resource *resource, int output_id, E_Client *ec)
{
   E_EomOutputPtr eom_output = NULL;
   E_EomClientPtr eom_client = NULL;
   E_Comp_Client_Data *cdata = NULL;
   Eina_Bool ret = EINA_FALSE;

   if (resource == NULL || output_id <= 0 || ec == NULL)
     return;

   cdata = ec->comp_data;
   EINA_SAFETY_ON_NULL_RETURN(cdata);
   EINA_SAFETY_ON_NULL_RETURN(cdata->shell.configure_send);

   eom_client = _e_eom_client_get_by_resource(resource);
   EINA_SAFETY_ON_NULL_RETURN(eom_client);

   eom_output = _e_eom_output_get_by_id(output_id);
   EINA_SAFETY_ON_NULL_RETURN(eom_output);

   ret = _e_eom_util_add_comp_object_redirected_hook(ec);
   EINA_SAFETY_ON_FALSE_RETURN(ret == EINA_TRUE);

   EOMDB("e_comp_object_redirected_set (ec:%p)(ec->frame:%p)\n", ec, ec->frame);

/* Send reconfigure event to a client which will resize its window to
 * external output resolution in respond */
   cdata->shell.configure_send(ec->comp_data->shell.surface, 0, eom_output->width, eom_output->height);

/* ec is used in buffer_change callback for distinguishing external ec and its buffers */
   eom_client->ec = ec;

   if (eom_output->status == TDM_OUTPUT_CONN_STATUS_DISCONNECTED)
     {
        wl_eom_send_output_set_window(resource, eom_output->id, WL_EOM_ERROR_NO_OUTPUT);
        return;
     }

   if (eom_client->current == EINA_TRUE)
     wl_eom_send_output_set_window(resource, eom_output->id, WL_EOM_ERROR_NONE);
   else
     wl_eom_send_output_set_window(resource, eom_output->id, WL_EOM_ERROR_OUTPUT_OCCUPIED);
}

static void
_e_eom_cb_wl_request_set_xdg_window(struct wl_client *client, struct wl_resource *resource, uint32_t output_id, struct wl_resource *surface)
{
   E_Client *ec = NULL;

   if (resource == NULL || output_id <= 0 || surface == NULL)
     return;

   EOMDB("set xdg output id:%d resource:%p surface:%p", output_id, resource, surface);

   if (!(ec = wl_resource_get_user_data(surface)))
     {
        wl_resource_post_error(surface,WL_DISPLAY_ERROR_INVALID_OBJECT, "No Client For Shell Surface");
        return;
     }

   _e_eom_window_set_internal(resource, output_id, ec);
}

static void
_e_eom_cb_wl_request_set_shell_window(struct wl_client *client, struct wl_resource *resource, uint32_t output_id, struct wl_resource *surface)
{
   E_Client *ec = NULL;

   if (resource == NULL || output_id <= 0 || surface == NULL)
     return;

   EOMDB("set shell output id:%d resource:%p surface:%p", output_id, resource, surface);

   if (!(ec = wl_resource_get_user_data(surface)))
     {
        wl_resource_post_error(surface,WL_DISPLAY_ERROR_INVALID_OBJECT, "No Client For Shell Surface");
        return;
     }

   _e_eom_window_set_internal(resource, output_id, ec);
}

static void
_e_eom_cb_wl_request_get_output_info(struct wl_client *client, struct wl_resource *resource, uint32_t output_id)
{
   EOMDB("output:%d", output_id);

   if (g_eom->outputs)
     {
        Eina_List *l;
        E_EomOutputPtr output = NULL;

        EINA_LIST_FOREACH(g_eom->outputs, l, output)
          {
             if (output->id == output_id)
               {
                  EOMDB("send - id : %d, type : %d, mode : %d, w : %d, h : %d, w_mm : %d, h_mm : %d, conn : %d",
                          output->id, output->type, output->mode, output->width, output->height,
                          output->phys_width, output->phys_height, output->status);

                  wl_eom_send_output_info(resource, output->id, output->type, output->mode, output->width, output->height,
                                          output->phys_width, output->phys_height, output->connection,
                                          1, 0, 0, 0);
               }
          }
     }
}

static const struct wl_eom_interface _e_eom_wl_implementation =
{
   _e_eom_cb_wl_request_set_attribute,
   _e_eom_cb_wl_request_set_xdg_window,
   _e_eom_cb_wl_request_set_shell_window,
   _e_eom_cb_wl_request_get_output_info
};

static void
_e_eom_cb_wl_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
   struct wl_resource *resource = NULL;
   E_EomClientPtr new_client = NULL;
   E_EomPtr eom = NULL;
   E_EomOutputPtr output = NULL;
   Eina_List *l;

   EINA_SAFETY_ON_NULL_RETURN(data);
   eom = data;

   resource = wl_resource_create(client, &wl_eom_interface, MIN(version, 1), id);
   if (resource == NULL)
     {
        EOMER("resource is null. (version :%d, id:%d)", version, id);
        wl_client_post_no_memory(client);
        return;
     }

   wl_resource_set_implementation(resource, &_e_eom_wl_implementation, eom, _e_eom_cb_wl_eom_client_destory);

   EOMDB("send - output count : %d", g_eom->output_count);

   wl_eom_send_output_count(resource, g_eom->output_count);

   if (g_eom->outputs)
     {
        EINA_LIST_FOREACH(g_eom->outputs, l, output)
          {
             EOMDB("send - id : %d, type : %d, mode : %d, w : %d, h : %d, w_mm : %d, h_mm : %d, conn : %d",
                     output->id, output->type, output->mode, output->width, output->height,
                     output->phys_width, output->phys_height, output->status);
             wl_eom_send_output_info(resource, output->id, output->type, output->mode, output->width, output->height,
                                     output->phys_width, output->phys_height, output->connection,
                                     1, 0, 0, 0);
          }
     }

   new_client = E_NEW(E_EomClient, 1);
   EINA_SAFETY_ON_NULL_RETURN(new_client);

   new_client->resource = resource;
   new_client->current = EINA_FALSE;
   new_client->output_id = -1;
   new_client->ec = NULL;

   g_eom->clients = eina_list_append(g_eom->clients, new_client);

   EOMDB("=======================>  BIND CLENT");
}

static Eina_Bool
_e_eom_cb_ecore_drm_activate(void *data EINA_UNUSED, int type EINA_UNUSED, void *event)
{

   Ecore_Drm_Event_Activate *e = NULL;

   if ((!event) || (!data))
     return ECORE_CALLBACK_PASS_ON;

   e = event;

   EOMDB("e->active:%d", e->active);

   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
_e_eom_boot_connection_check(void *data)
{
   E_EomOutputPtr eom_output;
   tdm_output *output = NULL;
   tdm_output_type tdm_type = TDM_OUTPUT_TYPE_Unknown;
   tdm_output_conn_status status = TDM_OUTPUT_CONN_STATUS_DISCONNECTED;
   tdm_error ret = TDM_ERROR_NONE;
   const char *tmp_name;
   char new_name[DRM_CONNECTOR_NAME_LEN];
   Eina_List *l;

   if (g_eom->check_first_boot != 0)
     {
        g_eom->timer = NULL;
        return ECORE_CALLBACK_CANCEL;
     }

   g_eom->check_first_boot = 1;

   if (g_eom->outputs)
     {
        EINA_LIST_FOREACH(g_eom->outputs, l, eom_output)
          {
             if (eom_output->id == 0)
               continue;

             output = eom_output->output;
             if (output == NULL)
               {
                  EOMER("output is null fail");
                  continue;
               }

             ret = tdm_output_get_conn_status(output, &status);
             if (ret != TDM_ERROR_NONE)
               {
                  EOMER("tdm_output_get_conn_status fail(%d)", ret);
                  continue;
               }

             if (status == TDM_OUTPUT_CONN_STATUS_DISCONNECTED)
               continue;

             ret = tdm_output_get_output_type(output, &tdm_type);
             if (ret != TDM_ERROR_NONE)
               {
                  EOMER("tdm_output_get_output_type fail(%d)", ret);
                  continue;
               }

             if (tdm_type < ALEN(eom_conn_types))
               tmp_name = eom_conn_types[tdm_type];
             else
               tmp_name = "unknown";
             /* TODO: What if there will more then one output of same type.
              * e.g. "HDMI and HDMI" "LVDS and LVDS"*/
             snprintf(new_name, sizeof(new_name), "%s-%d", tmp_name, 0);

             eom_output->type = (eom_output_type_e)tdm_type;
             eom_output->name = eina_stringshare_add(new_name);
             eom_output->status = status;

             _e_eom_output_connected(eom_output);
          }
     }
   g_eom->timer = NULL;
   return ECORE_CALLBACK_CANCEL;
}

static Eina_Bool
_e_eom_cb_ecore_drm_output(void *data EINA_UNUSED, int type EINA_UNUSED, void *event)
{
   Ecore_Drm_Event_Output *e = NULL;
   char buff[PATH_MAX];

   if (!(e = event)) return ECORE_CALLBACK_PASS_ON;

   EOMDB("id:%d (x,y,w,h):(%d,%d,%d,%d) (w_mm,h_mm):(%d,%d) refresh:%d subpixel_order:%d transform:%d make:%s model:%s name:%s plug:%d",
            e->id, e->x, e->y, e->w, e->h, e->phys_width, e->phys_height, e->refresh, e->subpixel_order, e->transform, e->make, e->model, e->name, e->plug);

   snprintf(buff, sizeof(buff), "%s", e->name);

   /* main output */
   if (e->id == 0)
     {
        if (e->plug == 1)
          {
             g_eom->width = e->w;
             g_eom->height = e->h;
             if (g_eom->main_output_name == NULL)
               g_eom->main_output_name = strdup(buff);

             g_eom->main_output_state = 1;

             if (g_eom->check_first_boot == 0)
               {
                  if (g_eom->timer)
                    ecore_timer_del(g_eom->timer);
                  g_eom->timer = ecore_timer_add(EOM_CONNECT_CHECK_TIMEOUT, _e_eom_boot_connection_check, NULL);
               }
          }
        else
          {
             g_eom->width = -1;
             g_eom->height = -1;
             if (g_eom->main_output_name)
               free(g_eom->main_output_name);

             g_eom->main_output_state = 0;
          }
     }

   return ECORE_CALLBACK_PASS_ON;
}

static E_EomClientPtr
_e_eom_client_get_current_by_ec(E_Client *ec)
{
   Eina_List *l;
   E_EomClientPtr client;

   EINA_LIST_FOREACH(g_eom->clients, l, client)
     {
        if (client && client->current == EINA_TRUE && client->ec == ec)
          return client;
     }

   return NULL;
}

static void
_e_eom_tbm_buffer_release_ext_mod(E_EomOutputPtr eom_output, tbm_surface_h srfc, void * eom_buff)
{
   EOMDB("============>  EXT END     tbm_buff:%p E_EomBuffer:%p", srfc, eom_buff);
   _e_eom_buffer_destroy(eom_buff);
}

static Eina_Bool
_e_eom_cb_client_buffer_change(void *data, int type, void *event)
{
   E_Comp_Wl_Buffer *wl_buffer = NULL;
   E_EomClientPtr eom_client = NULL;
   E_EomOutputPtr eom_output = NULL;
   E_Event_Client *ev = event;
   E_Client *ec = NULL;
   tbm_surface_h tbm_buffer = NULL;

   EINA_SAFETY_ON_NULL_RETURN_VAL(ev, ECORE_CALLBACK_PASS_ON);
   EINA_SAFETY_ON_NULL_RETURN_VAL(ev->ec, ECORE_CALLBACK_PASS_ON);

   ec = ev->ec;
   EINA_SAFETY_ON_TRUE_RETURN_VAL(e_object_is_del(E_OBJECT(ec)),
                                  ECORE_CALLBACK_PASS_ON);

   eom_client = _e_eom_client_get_current_by_ec(ec);
   if (eom_client == NULL)
     return ECORE_CALLBACK_PASS_ON;

   eom_output = _e_eom_output_get_by_id(eom_client->output_id);
   EINA_SAFETY_ON_NULL_RETURN_VAL(eom_output, ECORE_CALLBACK_PASS_ON);

   EINA_SAFETY_ON_NULL_RETURN_VAL(ec->pixmap, ECORE_CALLBACK_PASS_ON);

   wl_buffer = e_pixmap_resource_get(ec->pixmap);
   EINA_SAFETY_ON_NULL_RETURN_VAL(wl_buffer, ECORE_CALLBACK_PASS_ON);
   EINA_SAFETY_ON_NULL_RETURN_VAL(wl_buffer->resource, ECORE_CALLBACK_PASS_ON);

   /* Since Enlightenment client has reconfigured its window to fit
    * external output resolution and Enlightenment no nothing about
    * external outputs Enlightenment sees that client's resolution
    * differs form main screen resolution. Therefore, Enlightenment
    * is trying to resize it back to main screen resolution. It uses
    * timer for that purpose. To forbid it just delte the timer */

   /* TODO: It works but maybe there is better solution exists ?
    * Also I do not know how it affects on performance */
   if (ec->map_timer)
     {
        EOMDB("delete map_timer");
        E_FREE_FUNC(ec->map_timer, ecore_timer_del);
     }

   /* TODO: Support buffers smaller then output resolution */
   if (wl_buffer->w != eom_output->width ||
       wl_buffer->h != eom_output->height )
     {
        EOMER("tbm_buffer does not fit output's resolution");
        return ECORE_CALLBACK_PASS_ON;
     }

   /* TODO: support different SHMEM buffers etc. */
   tbm_buffer = wayland_tbm_server_get_surface(e_comp->wl_comp_data->tbm.server, wl_buffer->resource);
   EINA_SAFETY_ON_NULL_RETURN_VAL(tbm_buffer, ECORE_CALLBACK_PASS_ON);

   E_EomBufferPtr eom_buff = _e_eom_buffer_create(wl_buffer);
   EINA_SAFETY_ON_NULL_RETURN_VAL(eom_buff, ECORE_CALLBACK_PASS_ON);

   EOMDB("===============>  EXT START   tbm_buff:%p", tbm_buffer);

   if(!_e_eom_output_show(eom_output, tbm_buffer, _e_eom_tbm_buffer_release_ext_mod, eom_buff))
     {
        EOMDB("===============>  EXT ENDERR  tbm_buff:%p", tbm_buffer);
        EOMDB("_e_eom_add_buff_to_show fail");
        _e_eom_buffer_destroy(eom_buff);
        return ECORE_CALLBACK_PASS_ON;
     }

   if (eom_output->state == WAIT_PRESENTATION)
     {
        EOMDB("remove delayed presentation timer");
        if (eom_output->delay)
          ecore_timer_del(eom_output->delay);
     }

   eom_output->state = PRESENTATION;

   EOMDB("===============<  EXT START");
   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
_e_eom_init()
{
   Eina_Bool ret = EINA_FALSE;
   uint32_t id = 0;

   EINA_SAFETY_ON_NULL_GOTO(e_comp_wl, err);

   g_eom = E_NEW(E_Eom, 1);
   EINA_SAFETY_ON_NULL_RETURN_VAL(g_eom, EINA_FALSE);

   g_eom->global = wl_global_create(e_comp_wl->wl.disp, &wl_eom_interface, 1, g_eom, _e_eom_cb_wl_bind);

   id = wl_display_get_serial(e_comp_wl->wl.disp);
   EOMDB("eom name: %d", id);

   EINA_SAFETY_ON_NULL_GOTO(g_eom->global, err);

   ret = _e_eom_init_internal();
   EINA_SAFETY_ON_FALSE_GOTO(ret == EINA_TRUE, err);

   E_LIST_HANDLER_APPEND(g_eom->handlers, ECORE_DRM_EVENT_ACTIVATE, _e_eom_cb_ecore_drm_activate, g_eom);
   E_LIST_HANDLER_APPEND(g_eom->handlers, ECORE_DRM_EVENT_OUTPUT, _e_eom_cb_ecore_drm_output, g_eom);
   E_LIST_HANDLER_APPEND(g_eom->handlers, E_EVENT_CLIENT_BUFFER_CHANGE, _e_eom_cb_client_buffer_change, NULL);

   g_eom->main_output_name = NULL;

   return EINA_TRUE;

err:

   _e_eom_deinit();
   return EINA_FALSE;
}

int
e_devicemgr_eom_init(void)
{
   Eina_Bool ret = EINA_FALSE;

   ret = _e_eom_init();

   if (ret == EINA_FALSE)
     return 0;

   return 1;
}

void
e_devicemgr_eom_fini(void)
{
   _e_eom_deinit();
}

Eina_Bool
e_devicemgr_eom_is_ec_external(E_Client *ec)
{
   E_EomOutputPtr eom_output;

   eom_output = _e_eom_output_by_ec_child_get(ec);
   if (!eom_output)
     return EINA_FALSE;
   return EINA_TRUE;
}

tdm_output*
e_devicemgr_eom_tdm_output_by_ec_get(E_Client *ec)
{
   E_EomOutputPtr eom_output;

   eom_output = _e_eom_output_by_ec_child_get(ec);
   if (!eom_output)
     return NULL;
   return eom_output->output;
}
