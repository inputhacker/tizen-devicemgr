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

typedef struct _E_Eom E_Eom, *E_EomPtr;
typedef struct _E_Eom_Output E_EomOutput, *E_EomOutputPtr;
typedef struct _E_Eom_Client_Buffer E_EomClientBuffer, *E_EomClientBufferPtr;
typedef struct _E_Eom_Client E_EomClient, *E_EomClientPtr;
typedef struct _E_Eom_Comp_Object_Intercept_Hook_Data E_EomCompObjectInterceptHookData;

typedef enum {
   NONE,
   MIRROR,
   PRESENTATION,
} E_EomOutputState;

#define NUM_EOM_MAIN_BUF 2
#define NUM_EOM_ATTR 3
#define EOM_CONNECT_CHECK_TIMEOUT 7.0

/* EOM Output Attributes
   +-------------------+------------+-----------+-------------+
   |                         |   normal   | exclusive_share  |  exclusive  |
   +-------------------+------------+-----------+-------------+
   | normal              |  possible   |    possible          |  possible    |
   +-------------------+------------+-----------+-------------+
   | exclusive_share | impossible |    possible         |  possible    |
   +-------------------+------------+-----------+-------------+
   | exclusive           | impossible |   impossible      | impossible  |
   +-------------------+------------+-----------+-------------+

   possible   = 1
   impossible = 0
*/
static int eom_output_attributes[NUM_EOM_ATTR][NUM_EOM_ATTR] =
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

   /* mirror mode data */
   tbm_surface_h dst_buffers[NUM_EOM_MAIN_BUF];
   tbm_surface_h src_buffer;
   int current_buffer;
   int pp_buffer;

   tbm_surface_h dummy_buffer;

#ifdef DUMP_PRESENTATION
   Eina_Bool dump_do;
   int dump_count;
#endif
#ifdef FRAMES
   /* for testing purposes */
   struct timeval prev;
   struct timeval curr;

   int num_frames;
#endif
};


struct _E_Eom_Client
{
   struct wl_resource *resource;
   Eina_Bool current;

   /* EOM output the client related to */
   int output_id;
   /* E_Client the client related to */
   E_Client *ec;

   /*TODO: As I understand there are cannot be more than one client buffer on
    *server side, but for future extendabilty store it in the list */
   /*Client's buffers */
   Eina_List *buffers_show;
   Eina_List *buffers_del;
   Eina_Bool first_buffer;
};

struct _E_Eom_Client_Buffer
{
   E_Comp_Wl_Buffer *wl_buffer;
   tbm_surface_h tbm_buffer;

   unsigned long stamp;
};

struct _E_Eom_Comp_Object_Intercept_Hook_Data
{
   E_Client *ec;
   E_Comp_Object_Intercept_Hook *hook;
};

static E_EomPtr g_eom = NULL;
Eina_Bool eom_server_debug_on = EINA_FALSE;

#define ALEN(array) (sizeof(array) / sizeof(array)[0])

/*#define EOM_USE_DLOG_VPRINT*/
#ifdef EOM_USE_DLOG_VPRINT
void eom_log_print(const char *fmt, ...)
{
   va_list arg;

   va_start(arg, fmt);
   dlog_vprint(DLOG_ERROR, "E20", fmt, arg);
   va_end(arg);
}

#define EOM_ERR(msg, ARG...) eom_log_print("[eom module][%s:%d] ERR: " msg "\n", __FUNCTION__, __LINE__, ##ARG)
#define EOM_WAR(msg, ARG...) eom_log_print("[eom module][%s:%d] WRN: " msg "\n", __FUNCTION__, __LINE__, ##ARG)
#define EOM_INF(msg, ARG...) eom_log_print("[eom module][%s:%d] INF: " msg "\n", __FUNCTION__, __LINE__, ##ARG)
#define EOM_DBG(msg, ARG...) eom_log_print("[eom module][%s:%d] DBG: " msg "\n", __FUNCTION__, __LINE__, ##ARG)
#else
#define EOM_ERR(msg, ARG...) ERR("[eom module][%s:%d] ERR: " msg "\n", __FUNCTION__, __LINE__, ##ARG)
#define EOM_WRN(msg, ARG...) WRN("[eom module][%s:%d] WRN: " msg "\n", __FUNCTION__, __LINE__, ##ARG)
#define EOM_INF(msg, ARG...) \
   { \
      if (eom_server_debug_on) \
        INF("[eom module][%s:%d] INF: " msg "\n", __FUNCTION__, __LINE__, ##ARG); \
   }
#define EOM_DBG(msg, ARG...) \
   { \
      if (eom_server_debug_on) \
        DBG("[eom module][%s:%d] DBG: " msg "\n", __FUNCTION__, __LINE__, ##ARG); \
   }
#endif
#define RETURNIFTRUE(statement, msg, ARG...) \
   if (statement) \
     { \
        EOM_ERR(msg, ##ARG); \
        return; \
     }
#define RETURNVALIFTRUE(statement, ret, msg, ARG...) \
   if (statement) \
     { \
        EOM_ERR(msg, ##ARG); \
        return ret; \
     }

#define GOTOIFTRUE(statement, lable, msg, ARG...) \
   if (statement) \
     { \
        EOM_ERR(msg, ##ARG); \
        goto lable; \
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

   RETURNVALIFTRUE(eom_output == NULL, NULL, "eom_output is NULL");
   RETURNVALIFTRUE(eom_output->output == NULL, NULL, "eom_output->output is NULL");

   err = tdm_output_get_layer_count(eom_output->output, &count);
   if (err != TDM_ERROR_NONE)
     {
        EOM_DBG ("tdm_output_get_layer_count fail(%d)", err);
        return NULL;
     }

   for (i = 0; i < count; i++)
     {
        layer = (tdm_layer *)tdm_output_get_layer(eom_output->output, i, &err);
        RETURNVALIFTRUE(err != TDM_ERROR_NONE, NULL, "tdm_output_get_layer fail(%d)", err);

        err = tdm_layer_get_capabilities(layer, &capa);
        RETURNVALIFTRUE(err != TDM_ERROR_NONE, NULL, "tdm_layer_get_capabilities fail(%d)", err);

        if (capa & TDM_LAYER_CAPABILITY_PRIMARY)
          {
             EOM_DBG("TDM_LAYER_CAPABILITY_PRIMARY layer found : %d", i);
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
   RETURNVALIFTRUE(err != TDM_ERROR_NONE, NULL, "tdm_layer_set_info fail(%d)", err);

   return layer;
}

static tbm_surface_h
_e_eom_util_create_buffer(int width, int height, int format, int flags)
{
   tbm_surface_info_s buffer_info;
   tbm_surface_h buffer = NULL;

   buffer = tbm_surface_internal_create_with_flags(width, height, format, flags);
   RETURNVALIFTRUE(buffer == NULL, NULL, "Create buffer");

   memset(&buffer_info, 0x0, sizeof(tbm_surface_info_s));
   if (tbm_surface_map(buffer, TBM_SURF_OPTION_READ | TBM_SURF_OPTION_WRITE,
                       &buffer_info) != TBM_SURFACE_ERROR_NONE)
     {
        EOM_ERR("map buffer fail");
        tbm_surface_destroy(buffer);
        return NULL;
     }

   memset(buffer_info.planes[0].ptr, 0x0, buffer_info.planes[0].size);
   tbm_surface_unmap(buffer);

   return buffer;
}

static Eina_Bool
_e_eom_output_create_buffers(E_EomOutputPtr eom_output, int width, int height)
{
   int i = 0;

   /* TODO: Add support for other formats */
   for (i = 0; i < NUM_EOM_MAIN_BUF; i++)
     {
        eom_output->dst_buffers[i] = _e_eom_util_create_buffer(width, height, TBM_FORMAT_ARGB8888, TBM_BO_SCANOUT);
        GOTOIFTRUE(eom_output->dst_buffers[i] == NULL, err, "Create dst buffer fail");
     }

   eom_output->dummy_buffer = _e_eom_util_create_buffer(width, height, TBM_FORMAT_ARGB8888, TBM_BO_SCANOUT);
   GOTOIFTRUE(eom_output->dummy_buffer == NULL, err, "Create dummy buffer fail");

   return EINA_TRUE;

err:
   for (i = 0; i < NUM_EOM_MAIN_BUF; i++)
     if (eom_output->dst_buffers[i])
       tbm_surface_destroy(eom_output->dst_buffers[i]);

   return EINA_FALSE;
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
        EOM_ERR("Get primary output.(%s)", name);
        EINA_LIST_FOREACH(ecore_drm_devices_get(), l, dev)
          {
             primary_output = ecore_drm_output_primary_get(dev);
             if (primary_output != NULL)
               break;
          }

        if (primary_output == NULL)
          {
             EOM_ERR("Get primary output.(%s)", name);
             return NULL;
          }
     }

   tdm_output_obj = tdm_display_get_output(g_eom->dpy, 0, &err);
   if (tdm_output_obj == NULL || err != TDM_ERROR_NONE)
     {
        EOM_ERR("tdm_display_get_output 0 fail");
        return NULL;
     }
   err = tdm_output_get_layer_count(tdm_output_obj, &count);
   if (err != TDM_ERROR_NONE)
     {
        EOM_ERR("tdm_output_get_layer_count fail");
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
                  EOM_ERR("tdm_layer_get_displaying_buffer fail");
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
_e_eom_cb_pp(tbm_surface_h surface, void *user_data)
{
   tdm_error tdm_err = TDM_ERROR_NONE;
   E_EomOutputPtr eom_output = NULL;

   eom_output = (E_EomOutputPtr)user_data;
   RETURNIFTRUE(user_data == NULL, "pp event: user data is NULL");

   tdm_buffer_remove_release_handler(eom_output->dst_buffers[eom_output->pp_buffer],
                                     _e_eom_cb_pp, eom_output);

   /* TODO: lock these flags??? */
   if (g_eom->main_output_state == 0)
     return;

   /* If a client has committed its buffer stop mirror mode */
   if (eom_output->state != MIRROR)
     return;

   tbm_surface_h src_buffer;
   src_buffer = _e_eom_util_get_output_surface(g_eom->main_output_name);
   RETURNIFTRUE(src_buffer == NULL, "pp event: get root tdm surface");

   /*TODO: rewrite the mirror mode buffer's switching */
   eom_output->pp_buffer ^= 1;

   tdm_err = tdm_buffer_add_release_handler(eom_output->dst_buffers[eom_output->pp_buffer],
                                            _e_eom_cb_pp, eom_output);
   RETURNIFTRUE(tdm_err != TDM_ERROR_NONE, "pp event: set pp hadler:%d", tdm_err );

   tdm_err = tdm_pp_attach(eom_output->pp, src_buffer, eom_output->dst_buffers[eom_output->pp_buffer]);
   RETURNIFTRUE(tdm_err != TDM_ERROR_NONE, "pp attach:%d", tdm_err);

   tdm_err = tdm_pp_commit(eom_output->pp);
   RETURNIFTRUE(tdm_err != TDM_ERROR_NONE, "pp event: commit:%d", tdm_err );
}

static Eina_Bool
_e_eom_pp_init(E_EomOutputPtr eom_output, tbm_surface_h src_buffer)
{
   tdm_error err = TDM_ERROR_NONE;
   tdm_info_pp pp_info;
   tdm_pp *pp = NULL;
   int x, y, w, h;

   if (eom_output->pp == NULL)
     {
        pp = tdm_display_create_pp(g_eom->dpy, &err);
        RETURNVALIFTRUE(err != TDM_ERROR_NONE, EINA_FALSE, "Create pp:%d", err);

        eom_output->pp = pp;
     }
   else
     {
        pp = eom_output->pp;
     }

   /* TODO : consider rotation */
   _e_eom_util_calculate_fullsize(g_eom->width, g_eom->height,
                                  eom_output->width, eom_output->height,
                                  &x, &y, &w, &h);
   EOM_DBG("PP calculation: x:%d, y:%d, w:%d, h:%d", x, y, w, h);

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
   RETURNVALIFTRUE(err != TDM_ERROR_NONE, EINA_FALSE, "Set pp info:%d", err);

   eom_output->pp_buffer = !eom_output->current_buffer;
   err = tdm_buffer_add_release_handler(eom_output->dst_buffers[eom_output->pp_buffer],
                                        _e_eom_cb_pp, eom_output);
   RETURNVALIFTRUE(err != TDM_ERROR_NONE, EINA_FALSE, "Set pp handler:%d", err);

   err = tdm_pp_attach(pp, src_buffer, eom_output->dst_buffers[eom_output->pp_buffer]);
   RETURNVALIFTRUE(err != TDM_ERROR_NONE, EINA_FALSE, "pp attach:%d", err);

   err = tdm_pp_commit(eom_output->pp);
   RETURNVALIFTRUE(err != TDM_ERROR_NONE, EINA_FALSE, "pp commit:%d", err);

   return EINA_TRUE;
}

static Eina_Bool
_e_eom_output_start_pp(E_EomOutputPtr eom_output)
{
   /* should be changed in HWC enable environment */
   tbm_surface_info_s src_buffer_info;
   tbm_surface_h src_buffer = NULL;
   Eina_Bool ret = EINA_FALSE;

   src_buffer = _e_eom_util_get_output_surface(g_eom->main_output_name);
   RETURNVALIFTRUE(src_buffer == NULL, EINA_FALSE, "Get root tdm surfcae");

   tbm_surface_get_info(src_buffer, &src_buffer_info);

   EOM_DBG("main output info: %dx%d   bpp:%d   size:%d",
           src_buffer_info.width, src_buffer_info.height,
           src_buffer_info.bpp, src_buffer_info.size);

   /* TODO: if internal and external outputs are equal */
   ret = _e_eom_pp_is_needed(g_eom->width, g_eom->height,
                             eom_output->width, eom_output->height);
   RETURNVALIFTRUE(ret == EINA_FALSE, EINA_TRUE, "pp is not required");

   ret = _e_eom_pp_init(eom_output, src_buffer);
   RETURNVALIFTRUE(ret == EINA_FALSE, EINA_FALSE, "Init pp");

   return EINA_TRUE;
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

static E_EomClientBufferPtr
_e_eom_client_get_buffer(E_EomClientPtr client)
{
   E_EomClientBufferPtr show_buffer = NULL;
   E_EomClientBufferPtr prev_buffer = NULL;
   E_EomClientBufferPtr del_buffer = NULL;
   Eina_List *l;

   /* TODO: is it possible that a client has only one buffer to draw too? */
   RETURNVALIFTRUE(eina_list_count(client->buffers_show) == 0, NULL,
                   "eom client (%p) do not have buffers to be shown", client);

   /* If there is only one buffer we have slow client, just return the buffer */
   if (eina_list_count(client->buffers_show) == 1)
     {
        show_buffer = eina_list_nth(client->buffers_show, 0);
        RETURNVALIFTRUE(show_buffer == NULL, NULL,
                        "eom client (%p) buffer is NULL", client);

        EOM_DBG("one wl:%p tbm:%p", show_buffer->wl_buffer, show_buffer->tbm_buffer);

        return show_buffer;
     }

   if (client->first_buffer == EINA_TRUE)
     {
        show_buffer= eina_list_nth(client->buffers_show, 0);
        RETURNVALIFTRUE(show_buffer == NULL, NULL, "eom client (%p) first buffer is NULL", client);

        EOM_DBG("first wl:%p tbm:%p", show_buffer->wl_buffer, show_buffer->tbm_buffer);

        /* I am not sure if it is necessary */
        EOM_DBG("ref first");
        tbm_surface_internal_ref(show_buffer->tbm_buffer);

        client->first_buffer = EINA_FALSE;
        return show_buffer;
     }

   EINA_LIST_FOREACH(client->buffers_del, l, del_buffer)
     {
        if (del_buffer)
          {
             client->buffers_del = eina_list_remove(client->buffers_del, del_buffer);

             if (del_buffer->wl_buffer)
               {
                  del_buffer->wl_buffer->busy--;
                  EOM_DBG("wl_buffer:%p busy:%d", del_buffer->wl_buffer, del_buffer->wl_buffer->busy);
                  if (del_buffer->wl_buffer->busy == 0)
                    {
                       if (del_buffer->wl_buffer->type != E_COMP_WL_BUFFER_TYPE_TBM)
                         {
                            if (!wl_resource_get_client(del_buffer->wl_buffer->resource))
                              {
                                 EOM_DBG("wl_buffer->resource is NULL");
                                 return NULL;
                              }

                            wl_buffer_send_release(del_buffer->wl_buffer->resource);
                         }
                    }
                }

             EOM_DBG("del wl:%p tbm:%p", del_buffer->wl_buffer, del_buffer->tbm_buffer);

             if (del_buffer->tbm_buffer)
               {
                  EOM_DBG("del unref");
                  tbm_surface_internal_unref(del_buffer->tbm_buffer);
               }

             /* TODO: should wl_buffer and tbm_surface be deleted here? */
             E_FREE(del_buffer);
          }
     }

   /* TODO: case of 2 or n buffers */
   prev_buffer = eina_list_nth(client->buffers_show, 0);
   RETURNVALIFTRUE(prev_buffer == NULL, NULL, "eom client (%p) old_buffer is NULL", client);

   EOM_DBG("old wl:%p tbm:%p", prev_buffer->wl_buffer, prev_buffer->tbm_buffer);

   client->buffers_show = eina_list_remove(client->buffers_show, prev_buffer);
   client->buffers_del = eina_list_append(client->buffers_del, prev_buffer);

   /* Take next buffer to be shown on external output */
   show_buffer = eina_list_nth(client->buffers_show, 0);
   RETURNVALIFTRUE(show_buffer == NULL, NULL, "eom client (%p) next buffer is NULL", client);

   EOM_DBG("show wl:%p tbm:%p", show_buffer->wl_buffer, show_buffer->tbm_buffer);

   if (show_buffer->tbm_buffer)
     {
        EOM_DBG("show ref");
        tbm_surface_internal_ref(show_buffer->tbm_buffer);
     }

   return show_buffer;
}

static void
_e_eom_cb_commit(tdm_output *output EINA_UNUSED, unsigned int sequence EINA_UNUSED,
                unsigned int tv_sec EINA_UNUSED, unsigned int tv_usec EINA_UNUSED, void *user_data)
{
   E_EomClientBufferPtr client_buffer = NULL;
   E_EomOutputPtr eom_output = NULL;
   E_EomClientPtr eom_client = NULL;
   tdm_error err = TDM_ERROR_NONE;
   tbm_surface_h external_buffer = NULL;

   eom_output = (E_EomOutputPtr)user_data;
   RETURNIFTRUE(user_data == NULL, "commit event: user data is NULL");

   if (g_eom->main_output_state == 0)
     return;

   /* TODO: Maybe better to separating that callback on to mirror and extended callbacks */
   if (eom_output->state == MIRROR)
     {
        /*TODO: rewrite the mirror mode buffer's switching */
        eom_output->current_buffer ^= 1;
        err = tdm_layer_set_buffer(eom_output->layer,
                                   eom_output->dst_buffers[eom_output->current_buffer]);
        RETURNIFTRUE(err != TDM_ERROR_NONE, "commit event: mirror: set buffer 0 err:%d", err);

        err = tdm_output_commit(eom_output->output, 0, _e_eom_cb_commit, eom_output);
        RETURNIFTRUE(err != TDM_ERROR_NONE, "commit event: mirror: commit err:%d", err);
     }
   else if (eom_output->state == PRESENTATION)
     {
        eom_client = _e_eom_client_get_current_by_id(eom_output->id);

        EOM_DBG("COMMIT +++++++++++++++>");

        client_buffer = _e_eom_client_get_buffer(eom_client);
        if (client_buffer == NULL)
          {
             external_buffer = eom_output->dummy_buffer;
             EOM_DBG("substitute dummy buffer");
          }
        else
          external_buffer = client_buffer->tbm_buffer;

#ifdef DRAW_DUMMY
        EOM_DBG("COMMIT draw and set dummy");

        _e_eom_util_draw(eom_output->dummy_buffer);
        external_buffer = eom_output->dummy_buffer;
#endif

#ifdef DUMP_PRESENTATION
        if (eom_output->dump_do)
          {
             tbm_surface_internal_dump_buffer(external_buffer, "eom_buffer");
             eom_output->dump_count++;

             if (eom_output->dump_count > DUMP_NUM)
               {
                  tbm_surface_internal_dump_end();
                  eom_output->dump_do = EINA_FALSE;
               }
          }
#endif

        err = tdm_layer_set_buffer(eom_output->layer, external_buffer);
        RETURNIFTRUE(err != TDM_ERROR_NONE, "commit event: presentation: set buffer");

        err = tdm_output_commit(eom_output->output, 0, _e_eom_cb_commit, eom_output);
        RETURNIFTRUE(err != TDM_ERROR_NONE, "commit event: presentation: commit err:%d", err);

        EOM_DBG("COMMIT <+++++++++++++++");
     }

#ifdef FRAMES
   _e_eom_util_check_frames(eom_output);
#endif
}

static void
_e_eom_output_start_mirror(E_EomOutputPtr eom_output)
{
   tdm_layer *hal_layer;
   tdm_info_layer layer_info;
   tdm_error tdm_err = TDM_ERROR_NONE;
   int ret = 0;

   if (eom_output->state == MIRROR)
     return;

   hal_layer = _e_eom_output_get_layer(eom_output);
   GOTOIFTRUE(hal_layer == NULL, err, "Get hal layer fail");

   ret = _e_eom_output_create_buffers(eom_output, eom_output->width, eom_output->height);
   GOTOIFTRUE(ret == EINA_FALSE, err, "Create buffers fail");

   tdm_err = tdm_layer_get_info(hal_layer, &layer_info);
   GOTOIFTRUE(tdm_err != TDM_ERROR_NONE, err, "Get layer info fail(%d)", tdm_err);

   EOM_DBG("layer info: %dx%d, pos (x:%d, y:%d, w:%d, h:%d,  dpos (x:%d, y:%d, w:%d, h:%d))",
           layer_info.src_config.size.h,  layer_info.src_config.size.v,
           layer_info.src_config.pos.x, layer_info.src_config.pos.y,
           layer_info.src_config.pos.w, layer_info.src_config.pos.h,
           layer_info.dst_pos.x, layer_info.dst_pos.y,
           layer_info.dst_pos.w, layer_info.dst_pos.h);

   eom_output->layer = hal_layer;
   eom_output->current_buffer = 0;

   tdm_err = tdm_layer_set_buffer(hal_layer, eom_output->dst_buffers[eom_output->current_buffer]);
   GOTOIFTRUE(tdm_err != TDM_ERROR_NONE, err, "Set buffer on layer:%d", tdm_err);

   tdm_err = tdm_output_set_dpms(eom_output->output, TDM_OUTPUT_DPMS_ON);
   GOTOIFTRUE(tdm_err != TDM_ERROR_NONE, err, "tdm_output_set_dpms on");

   /* get main surface */
   ret = _e_eom_output_start_pp(eom_output);
   GOTOIFTRUE(ret == EINA_FALSE, err, "Get root surfcae");

   tdm_err = tdm_output_commit(eom_output->output, 0, _e_eom_cb_commit, eom_output);
   GOTOIFTRUE(tdm_err != TDM_ERROR_NONE, err, "Commit crtc:%d", tdm_err);

   _e_eom_output_state_set_mode(eom_output, EOM_OUTPUT_MODE_MIRROR);
   eom_output->state = MIRROR;

   return;

err:
/*
 * TODO: add deinitialization
 */
   return;
}

static void
_e_eom_client_free_buffers(E_EomClientPtr client)
{
   E_EomClientBufferPtr buffer = NULL;
   Eina_List *l;

   EINA_LIST_FOREACH(client->buffers_show, l, buffer)
     {
        if (buffer)
          {
             client->buffers_show = eina_list_remove(client->buffers_show, buffer);

             /* TODO: not sure if it is necessary */
             if (buffer->tbm_buffer && client->first_buffer == EINA_FALSE)
               tbm_surface_internal_unref(buffer->tbm_buffer);

             /* TODO: not sure if it is necessary */
             if (buffer->wl_buffer)
               buffer->wl_buffer->busy--;

             E_FREE(buffer);
          }
     }

   EINA_LIST_FOREACH(client->buffers_del, l, buffer)
     {
        if (buffer)
          {
             client->buffers_del= eina_list_remove(client->buffers_del, buffer);

             /* TODO: not sure if it is necessary */
             if (buffer->tbm_buffer)
               tbm_surface_internal_unref(buffer->tbm_buffer);

             /* TODO: not sure if it is necessary */
             if (buffer->wl_buffer)
               buffer->wl_buffer->busy--;

             E_FREE(buffer);
          }
     }
}

static void
_e_eom_output_deinit(E_EomOutputPtr eom_output, int pp)
{
   tdm_error err = TDM_ERROR_NONE;
   E_EomClientPtr iterator = NULL;
   Eina_List *l;
   int i = 0;

   if (eom_output->layer)
     {
        err = tdm_layer_unset_buffer(eom_output->layer);
        if (err != TDM_ERROR_NONE)
          EOM_DBG("fail unset buffer:%d", err);

        err = tdm_output_commit(eom_output->output, 0, NULL, eom_output);
        if (err != TDM_ERROR_NONE)
          EOM_DBG ("fail commit:%d", err);
    }

   err = tdm_output_set_dpms(eom_output->output, TDM_OUTPUT_DPMS_OFF);
   if (err != TDM_ERROR_NONE)
     EOM_ERR("set DPMS off:%d", err);

   if (pp == 1)
     {
        for (i = 0; i < NUM_EOM_MAIN_BUF; i++)
          {
             tdm_buffer_remove_release_handler(eom_output->dst_buffers[i],
                                              _e_eom_cb_pp, eom_output);

             if (eom_output->dst_buffers[i])
               tbm_surface_destroy(eom_output->dst_buffers[i]);
          }
        if (eom_output->pp)
          {
             tdm_pp_destroy(eom_output->pp);
             eom_output->pp = NULL;
          }
     }

   if (g_eom->main_output_state == 0)
     EINA_LIST_FOREACH(g_eom->clients, l, iterator)
       {
          if (iterator && iterator->output_id == eom_output->id)
            _e_eom_client_free_buffers(iterator);
       }
}

static void
_e_eom_output_stop_mirror(E_EomOutputPtr eom_output)
{
   if (eom_output->state == NONE)
     return;

   _e_eom_output_state_set_status(eom_output, TDM_OUTPUT_CONN_STATUS_DISCONNECTED);
   _e_eom_output_state_set_mode(eom_output, EOM_OUTPUT_MODE_NONE);

   _e_eom_output_deinit(eom_output, 1);

   eom_output->state = NONE;
}

static void
_e_eom_output_stop_presentation(E_EomOutputPtr eom_output)
{
   E_EomClientPtr iterator = NULL;
   Eina_List *l;

   if (eom_output->state == NONE)
     return;

   _e_eom_output_state_set_status(eom_output, TDM_OUTPUT_CONN_STATUS_DISCONNECTED);
   _e_eom_output_state_set_mode(eom_output, EOM_OUTPUT_MODE_NONE);

   _e_eom_output_deinit(eom_output, 0);

   EINA_LIST_FOREACH(g_eom->clients, l, iterator)
     if (iterator && iterator->output_id == eom_output->id)
       _e_eom_client_free_buffers(iterator);

   eom_output->state = NONE;
}

static const tdm_output_mode *
_e_eom_output_get_best_mode(tdm_output *output)
{
   tdm_error ret = TDM_ERROR_NONE;
   const tdm_output_mode *modes;
   const tdm_output_mode *mode = NULL;
   /* unsigned int vrefresh = 0; */
   unsigned int best_value = 0;
   unsigned int value;
   int i, count = 0;

   ret = tdm_output_get_available_modes(output, &modes, &count);
   if (ret != TDM_ERROR_NONE)
     {
        EOM_ERR("tdm_output_get_available_modes fail(%d)", ret);
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

   EOM_DBG("bestmode : %s, (%dx%d) r(%d), f(%d), t(%d)",
           mode->name, mode->hdisplay, mode->vdisplay, mode->vrefresh, mode->flags, mode->type);

   return mode;
}

static int
_e_eom_output_get_position(void)
{
   tdm_output *output_main = NULL;
   const tdm_output_mode *mode;
   tdm_error ret = TDM_ERROR_NONE;
   int x = 0;

   output_main = tdm_display_get_output(g_eom->dpy, 0, &ret);
   RETURNVALIFTRUE(ret != TDM_ERROR_NONE, 0, "tdm_display_get_output main fail(ret:%d)", ret);
   RETURNVALIFTRUE(output_main == NULL, 0, "tdm_display_get_output main fail(no output:%d)", ret);

   ret = tdm_output_get_mode(output_main, &mode);
   RETURNVALIFTRUE(ret != TDM_ERROR_NONE, 0, "tdm_output_get_mode main fail(ret:%d)", ret);

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

static int
_e_eom_output_connected(E_EomOutputPtr eom_output)
{
   tdm_output *output;
   const tdm_output_mode *mode;
   tdm_error ret = TDM_ERROR_NONE;
   const char *maker = NULL, *model = NULL, *name = NULL;
   unsigned int mmWidth, mmHeight, subpixel;
   int x = 0;
   E_EomClientPtr iterator = NULL;
   Eina_List *l;

   output = eom_output->output;

   ret = tdm_output_get_model_info(output, &maker, &model, &name);
   RETURNVALIFTRUE(ret != TDM_ERROR_NONE, -1, "tdm_output_get_model_info fail(%d)", ret);

   ret = tdm_output_get_physical_size(output, &mmWidth, &mmHeight);
   RETURNVALIFTRUE(ret != TDM_ERROR_NONE, -1, "tdm_output_get_physical_size fail(%d)", ret);

   ret = tdm_output_get_subpixel(output, &subpixel);
   RETURNVALIFTRUE(ret != TDM_ERROR_NONE, -1, "tdm_output_get_subpixel fail(%d)", ret);

   mode = _e_eom_output_get_best_mode(output);
   RETURNVALIFTRUE(mode == NULL, -1, "_e_eom_get_best_resolution fail");

   ret = tdm_output_set_mode(output, mode);
   RETURNVALIFTRUE(ret != TDM_ERROR_NONE, -1, "tdm_output_set_mode fail(%d)", ret);

   x = _e_eom_output_get_position();
   EOM_DBG("mode: %dx%d, phy(%dx%d), pos(%d,0), refresh:%d, subpixel:%d",
           mode->hdisplay, mode->vdisplay, mmWidth, mmHeight, x, mode->vrefresh, subpixel);

   if (!e_comp_wl_output_init(eom_output->name, maker, eom_output->name, x, 0,
                              mode->hdisplay, mode->vdisplay,
                              mmWidth, mmHeight, mode->vrefresh, subpixel, 0))
     {
        EOM_ERR("Could not setup new output: %s", eom_output->name);
        return -1;
     }
   EOM_DBG("Setup new output: %s", eom_output->name);

   /* update eom_output connect */
   eom_output->width = mode->hdisplay;
   eom_output->height = mode->vdisplay;
   eom_output->phys_width = mmWidth;
   eom_output->phys_height = mmHeight;
#ifdef DUMP_PRESENTATION
   eom_output->dump_do = EINA_TRUE;
   eom_output->dump_count = 1;

   EOM_DBG("dump init");
   tbm_surface_internal_dump_start("/eom_buffers", eom_output->width, eom_output->height, DUMP_NUM);
#endif
   /* TODO: check output mode(presentation set) and HDMI type */
   _e_eom_output_start_mirror(eom_output);

   /* If there were previously connected clients to the output - notify them */
   EINA_LIST_FOREACH(g_eom->clients, l, iterator)
     {
        if (iterator)
          {
             EOM_DBG("Send MIRROR ON notification to clients");

             if (iterator->current)
               wl_eom_send_output_info(iterator->resource, eom_output->id,
                                       eom_output->type, eom_output->mode,
                                       eom_output->width, eom_output->height,
                                       eom_output->phys_width, eom_output->phys_height,
                                       WL_EOM_STATUS_CONNECTION,
                                       0,
                                       _e_eom_output_state_get_attribute(eom_output),
                                       EOM_OUTPUT_ATTRIBUTE_STATE_ACTIVE,
                                       EOM_ERROR_NONE);
             else
               wl_eom_send_output_info(iterator->resource, eom_output->id,
                                       eom_output->type, eom_output->mode,
                                       eom_output->width, eom_output->height,
                                       eom_output->phys_width, eom_output->phys_height,
                                       WL_EOM_STATUS_CONNECTION,
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

   if (eom_output->state == MIRROR)
     _e_eom_output_stop_mirror(eom_output);
   else if (eom_output->state == PRESENTATION)
     _e_eom_output_stop_presentation(eom_output);

   /* update eom_output disconnect */
   eom_output->width = 0;
   eom_output->height = 0;
   eom_output->phys_width = 0;
   eom_output->phys_height = 0;

   /* If there were previously connected clients to the output - notify them */
   EINA_LIST_FOREACH(g_eom->clients, l, iterator)
     {
        if (iterator)
          {
             EOM_DBG("Send MIRROR OFF notification to clients");
             if (iterator->current)
               wl_eom_send_output_info(iterator->resource, eom_output->id,
                                       eom_output->type, eom_output->mode,
                                       eom_output->width, eom_output->height,
                                       eom_output->phys_width, eom_output->phys_height,
                                       WL_EOM_STATUS_DISCONNECTION,
                                       0,
                                       _e_eom_output_state_get_attribute(eom_output),
                                       EOM_OUTPUT_ATTRIBUTE_STATE_INACTIVE,
                                       EOM_ERROR_NONE);
             else
               wl_eom_send_output_info(iterator->resource, eom_output->id,
                                       eom_output->type, eom_output->mode,
                                       eom_output->width, eom_output->height,
                                       eom_output->phys_width, eom_output->phys_height,
                                       WL_EOM_STATUS_DISCONNECTION,
                                       1, 0, 0, 0);
          }
     }

   e_comp_wl_output_remove(eom_output->name);
   EOM_DBG("Destory output: %s", eom_output->name);
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

   ret = tdm_output_get_output_type(output, &tdm_type);
   RETURNIFTRUE(ret != TDM_ERROR_NONE, "tdm_output_get_output_type fail(%d)", ret);

   ret = tdm_output_get_conn_status(output, &status_check);
   RETURNIFTRUE(ret != TDM_ERROR_NONE, "tdm_output_get_conn_status fail(%d)", ret);

   status = value.u32;

   EOM_DBG("id (%d), type(%d, %d), status(%d, %d)", eom_output->id, type, tdm_type, status_check, status);

   eom_output->type = (eom_output_type_e)tdm_type;
   eom_output->status = status;

   if (status == TDM_OUTPUT_CONN_STATUS_CONNECTED)
     {
        if (tdm_type < ALEN(eom_conn_types))
          tmp_name = eom_conn_types[tdm_type];
        else
          tmp_name = "unknown";

        /*TODO: What if there will more then one output of same type.
               *e.g. "HDMI and HDMI" "LVDS and LVDS"*/
        snprintf(new_name, sizeof(new_name), "%s-%d", tmp_name, 0);

        eom_output->name = eina_stringshare_add(new_name);

        _e_eom_output_connected(eom_output);
     }
   else if (status == TDM_OUTPUT_CONN_STATUS_DISCONNECTED)
     {
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
   RETURNVALIFTRUE(ret != TDM_ERROR_NONE, EINA_FALSE,
                   "tdm_display_get_output_count fail");
   RETURNVALIFTRUE(count <= 1, EINA_FALSE,
                   "output count is 1. device doesn't support external outputs.");

   g_eom->output_count = count - 1;
   EOM_DBG("external output count : %d", g_eom->output_count);

   /* skip main output id:0 */
   /* start from 1 */
   for (i = 1; i < count; i++)
     {
        output = tdm_display_get_output(dpy, i, &ret);
        GOTOIFTRUE(ret != TDM_ERROR_NONE, err, "tdm_display_get_output fail(ret:%d)", ret);
        GOTOIFTRUE(output == NULL, err, "tdm_display_get_output fail(no output:%d)", ret);

        ret = tdm_output_get_output_type(output, &type);
        GOTOIFTRUE(ret != TDM_ERROR_NONE, err, "tdm_output_get_output_type fail(%d)", ret);

        new_output = E_NEW(E_EomOutput, 1);
        GOTOIFTRUE(new_output == NULL, err, "calloc fail");

        ret = tdm_output_get_conn_status(output, &status);
        if (ret != TDM_ERROR_NONE)
          {
             EOM_ERR("tdm_output_get_conn_status fail(%d)", ret);
             free(new_output);
             goto err;
          }
        new_output->id = i;
        new_output->type = type;
        new_output->status = status;
        new_output->mode = EOM_OUTPUT_MODE_NONE;
        new_output->output = output;

        ret = tdm_output_add_change_handler(output, _e_eom_cb_tdm_output_status_change, NULL);
        if (ret != TDM_ERROR_NONE)
          {
              EOM_ERR("tdm_output_add_change_handler fail(%d)", ret);
              free(new_output);
              goto err;
          }

        if (status == TDM_OUTPUT_CONN_STATUS_DISCONNECTED)
          {
             EOM_DBG("create(%d)output, type:%d, status:%d",
                     new_output->id, new_output->type, new_output->status);
             g_eom->outputs = eina_list_append(g_eom->outputs, new_output);
             continue;
          }
        new_output->status = TDM_OUTPUT_CONN_STATUS_CONNECTED;

        ret = tdm_output_get_mode(output, &mode);
        if (ret != TDM_ERROR_NONE)
          {
             EOM_ERR("tdm_output_get_mode fail(%d)", ret);
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
             EOM_ERR("tdm_output_get_conn_status fail(%d)", ret);
             free(new_output);
             goto err;
          }
        new_output->phys_width = mmWidth;
        new_output->phys_height = mmHeight;

        EOM_DBG("create(%d)output, type:%d, status:%d, w:%d, h:%d, mm_w:%d, mm_h:%d",
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
#if 0
   tdm_error ret = TDM_ERROR_NONE;

   g_eom->dpy = tdm_display_init(&ret);
   GOTOIFTRUE(ret != TDM_ERROR_NONE, err, "tdm_display_init fail");

   ret = tdm_display_get_fd(g_eom->dpy, &g_eom->fd);
   GOTOIFTRUE(ret != TDM_ERROR_NONE, err, "tdm_display_get_fd fail");

   g_eom->bufmgr = tbm_bufmgr_init(g_eom->fd);
   GOTOIFTRUE(g_eom->bufmgr == NULL, err, "tbm_bufmgr_init fail");
#else
   g_eom->dpy = e_devmgr_dpy->tdm;
   GOTOIFTRUE(g_eom->dpy == NULL, err, "tdm display get fail");

   g_eom->bufmgr = e_devmgr_dpy->bufmgr;
   GOTOIFTRUE(g_eom->bufmgr == NULL, err, "tbm bufmgr get fail");
#endif

   if (_e_eom_output_init(g_eom->dpy) != EINA_TRUE)
     {
        EOM_ERR("_e_eom_output_init fail");
        goto err;
     }

   return EINA_TRUE;

err:
#if 0
   if (g_eom->bufmgr)
     tbm_bufmgr_deinit(g_eom->bufmgr);

   if (g_eom->dpy)
     tdm_display_deinit(g_eom->dpy);
#else
   if (g_eom->bufmgr)
     g_eom->bufmgr = NULL;

   if (g_eom->dpy)
     g_eom->dpy = NULL;
#endif
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

static void
_e_eom_util_get_debug_env()
{
   char *env = getenv("EOM_SERVER_DEBUG");

   /* TODO: DLOG */
   if (env)
     eom_server_debug_on = (atoi(env)) > 0 ? EINA_TRUE : EINA_FALSE;
   else
     eom_server_debug_on = EINA_FALSE;

   EOM_INF("EOM_SERVER_DEBUG = %s", eom_server_debug_on > 0 ? "ON" : "OFF");
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

static void
_e_eom_cb_wl_resource_destroy(struct wl_resource *resource)
{
   E_EomClientPtr client = NULL, iterator = NULL;
   E_EomOutputPtr output = NULL;
   Eina_List *l = NULL;
   Eina_Bool ret;

   EOM_DBG("client unbind");

   client = _e_eom_client_get_by_resource(resource);
   RETURNIFTRUE(client == NULL, "eom client is NULL");

   g_eom->clients = eina_list_remove(g_eom->clients, client);

   /* If it is not current client do nothing */
   if (client->current == EINA_FALSE)
     goto end2;

   output = _e_eom_output_get_by_id(client->output_id);
   GOTOIFTRUE(output == NULL, end2, "output is NULL");

   _e_eom_output_state_set_mode(output, EOM_OUTPUT_MODE_MIRROR);
   ret = _e_eom_output_state_set_attribute(output, EOM_OUTPUT_ATTRIBUTE_NONE);
   (void)ret;

   _e_eom_client_free_buffers(client);

   /* TODO: process case when output is not connected */
   if (output->state == NONE)
     goto end;

   /* If a client has been disconnected and mirror mode has not
    * been restored, start mirror mode
    */
   if (output->state != MIRROR)
     {
        output->state = MIRROR;

        ret = _e_eom_output_start_pp(output);
        GOTOIFTRUE(ret == EINA_FALSE, end,
                   "Restore mirror mode after a client disconnection");
     }

end:
   /* Notify eom clients which are binded to a concrete output that the
    * state and mode of the output has been changed */
   EINA_LIST_FOREACH(g_eom->clients, l, iterator)
     {
        if (iterator && iterator->output_id == output->id)
          {
             wl_eom_send_output_attribute(iterator->resource,
                                          output->id,
                                          _e_eom_output_state_get_attribute(output),
                                          _e_eom_output_state_get_attribute_state(output),
                                          EOM_OUTPUT_MODE_NONE);

             wl_eom_send_output_mode(iterator->resource,
                                     output->id,
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
   RETURNIFTRUE(eom_client == NULL, "eom_client is NULL");

   /* Bind the client with a concrete output */
   eom_client->output_id = output_id;

   eom_output = _e_eom_output_get_by_id(output_id);
   GOTOIFTRUE(eom_output == NULL, no_output, "eom_output is NULL");

   EOM_DBG("Set attribute:%d", attribute);

   if (eom_client->current == EINA_TRUE && eom_output->id == eom_client->output_id)
     {
        /* Current client can set any flag it wants */
        _e_eom_output_state_set_force_attribute(eom_output, attribute);
     }
   else
     {
        /* A client is trying to set new attribute */
        ret = _e_eom_output_state_set_attribute(eom_output, attribute);
        if (ret == EINA_FALSE)
          {
             EOM_DBG("set attribute FAILED");

             eom_error = EOM_ERROR_INVALID_PARAMETER;
             goto end;
          }
     }

   EOM_DBG("set attribute OK");

   /* If client has set EOM_OUTPUT_ATTRIBUTE_NONE switching to mirror mode */
   if (attribute == EOM_OUTPUT_ATTRIBUTE_NONE && eom_output->state != MIRROR)
     {
        eom_output->state = MIRROR;
        eom_client->current = EINA_FALSE;

        _e_eom_output_state_set_mode(eom_output, EOM_OUTPUT_ATTRIBUTE_NONE);
        ret = _e_eom_output_state_set_attribute(eom_output, EOM_OUTPUT_ATTRIBUTE_NONE);
        (void)ret;

        _e_eom_client_free_buffers(eom_client);

        if (eom_output->status == 0)
          {
             EOM_DBG("output:%d is disconnected", output_id);
             goto end;
          }

        ret = _e_eom_output_start_pp(eom_output);
        GOTOIFTRUE(ret == EINA_FALSE, end,
                   "Restore mirror mode after disconnection of the client");

        /* If mirror mode has been ran notify all clients about that */
        EOM_DBG("client set NONE attribute send new info to previous current client");
        EINA_LIST_FOREACH(g_eom->clients, l, iterator)
          {
             if (iterator->output_id == output_id)
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
   /* If client was not able to set attribute then send LOST event
    * to it and return */
   /* TODO: I think it is bad to send LOST event in that case
    * Client must process eom_errors */
   if (eom_error == EOM_ERROR_INVALID_PARAMETER)
     {
        EOM_DBG("client failed to set attribute");

        wl_eom_send_output_attribute(eom_client->resource, eom_output->id,
                                     _e_eom_output_state_get_attribute(eom_output),
                                     EOM_OUTPUT_ATTRIBUTE_STATE_LOST,
                                     eom_error);
        return;
     }

   /* Send changes to the caller-client */
   wl_eom_send_output_attribute(eom_client->resource,
                                eom_output->id,
                                _e_eom_output_state_get_attribute(eom_output),
                                _e_eom_output_state_get_attribute_state(eom_output),
                                eom_error);

   /* Send changes to previous current client */
   if (eom_client->current == EINA_FALSE &&
       (current_eom_client = _e_eom_client_get_current_by_id(eom_output->id)))
     {
        current_eom_client->current = EINA_FALSE;

        EOM_DBG("client failed to set attribute");

        wl_eom_send_output_attribute(current_eom_client->resource,
                                     eom_output->id,
                                     _e_eom_output_state_get_attribute(eom_output),
                                     EOM_OUTPUT_ATTRIBUTE_STATE_LOST,
                                     EOM_ERROR_NONE);
     }

   /* Set the client as current client of the eom_output */
   eom_client->current= EINA_TRUE;

   return;

   /* Get here if EOM does not have output referred by output_id */
no_output:
   wl_eom_send_output_attribute(eom_client->resource,
                                output_id,
                                EOM_OUTPUT_ATTRIBUTE_NONE,
                                EOM_OUTPUT_ATTRIBUTE_STATE_NONE,
                                EOM_ERROR_NO_SUCH_DEVICE);

   wl_eom_send_output_mode(eom_client->resource,
                           output_id,
                           EOM_OUTPUT_MODE_NONE);

   wl_eom_send_output_type(eom_client->resource,
                           output_id,
                           EOM_OUTPUT_ATTRIBUTE_STATE_NONE,
                           TDM_OUTPUT_CONN_STATUS_DISCONNECTED);
   return;
}

static Eina_Bool
_e_eom_cb_comp_object_redirected(void *data, E_Client *ec)
{
   E_EomCompObjectInterceptHookData *hook_data;

   EOM_DBG("_e_eom_cb_comp_object_redirected");
   RETURNVALIFTRUE(data == NULL, EINA_TRUE, "data is NULL");

   hook_data = (E_EomCompObjectInterceptHookData* )data;

   RETURNVALIFTRUE(hook_data->ec == NULL, EINA_TRUE, "hook_data->ec is NULL");
   RETURNVALIFTRUE(hook_data->hook == NULL, EINA_TRUE, "hook_data->hook is NULL");
   RETURNVALIFTRUE(hook_data->ec != ec, EINA_TRUE, "hook_data->ec != ec");

   /* Hide the window from Enlightenment main screen */
   e_comp_object_redirected_set(ec->frame, EINA_FALSE);

   e_comp_object_intercept_hook_del(hook_data->hook);

   g_eom->comp_object_intercept_hooks = eina_list_remove(g_eom->comp_object_intercept_hooks,
                                                         hook_data);

   free(hook_data);

   return EINA_TRUE;
}

static Eina_Bool
_e_eom_util_add_comp_object_redirected_hook(E_Client *ec)
{
   E_EomCompObjectInterceptHookData *hook_data = NULL;
   E_Comp_Object_Intercept_Hook *hook = NULL;

   hook_data = E_NEW(E_EomCompObjectInterceptHookData, 1);
   GOTOIFTRUE(hook_data == NULL, err, "hook_data = NULL");

   hook_data->ec = ec;

   hook = e_comp_object_intercept_hook_add(E_COMP_OBJECT_INTERCEPT_HOOK_SHOW_HELPER,
                                           _e_eom_cb_comp_object_redirected,
                                           hook_data);
   GOTOIFTRUE(hook == NULL, err, "hook = NULL");

   hook_data->hook = hook;

   g_eom->comp_object_intercept_hooks = eina_list_append(g_eom->comp_object_intercept_hooks,
                                                         hook_data);

   EOM_DBG("_e_eom_redirected_hook have been added");
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
   RETURNIFTRUE(cdata == NULL, "cdata is NULL");
   RETURNIFTRUE(cdata->shell.configure_send == NULL, "cdata->shell.configure_send  is NULL");

   eom_client = _e_eom_client_get_by_resource(resource);
   RETURNIFTRUE(eom_client == NULL, "eom_client is NULL");

   eom_output = _e_eom_output_get_by_id(output_id);
   RETURNIFTRUE(eom_output == NULL, "eom_output is NULL");

   ret = _e_eom_util_add_comp_object_redirected_hook(ec);
   RETURNIFTRUE(ret != EINA_TRUE, "Set redirect comp hook failed");

   EOM_DBG("e_comp_object_redirected_set (ec->frame:%p)\n", ec->frame);

   /* Send reconfigure event to a client which will resize its window to
    * external output resolution in respond */
   cdata->shell.configure_send(ec->comp_data->shell.surface, 0, eom_output->width, eom_output->height);

   /* ec is used in buffer_change callback for distinguishing external ec and its buffers */
   eom_client->ec = ec;

   if (eom_client->current == EINA_TRUE)
     wl_eom_send_output_set_window(resource, eom_output->id, WL_EOM_ERROR_NONE);
   else
     wl_eom_send_output_set_window(resource, eom_output->id, WL_EOM_ERROR_OUTPUT_OCCUPIED);
}

/* TODO: It uses xdg_surface. Add support of shell_surface */
/* TODO: I think there must be implemented an event for client which signals if set window was successful */
static void
_e_eom_cb_wl_request_set_xdg_window(struct wl_client *client, struct wl_resource *resource, uint32_t output_id, struct wl_resource *surface)
{
   E_Client *ec = NULL;

   if (resource == NULL || output_id <= 0 || surface == NULL)
     return;

   EOM_DBG("set xdg output id:%d resource:%p surface:%p",
           output_id, resource, surface);

   if (!(ec = wl_resource_get_user_data(surface)))
     {
        wl_resource_post_error(surface,WL_DISPLAY_ERROR_INVALID_OBJECT,
                               "No Client For Shell Surface");
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

   EOM_DBG("set shell output id:%d resource:%p surface:%p",
           output_id, resource, surface);

   if (!(ec = wl_resource_get_user_data(surface)))
     {
        wl_resource_post_error(surface,WL_DISPLAY_ERROR_INVALID_OBJECT,
                               "No Client For Shell Surface");
        return;
     }

   _e_eom_window_set_internal(resource, output_id, ec);
}

static void
_e_eom_cb_wl_request_get_output_info(struct wl_client *client, struct wl_resource *resource, uint32_t output_id)
{
   EOM_DBG("output:%d", output_id);

   if (g_eom->outputs)
     {
        Eina_List *l;
        E_EomOutputPtr output = NULL;

        EINA_LIST_FOREACH(g_eom->outputs, l, output)
          {
             if (output->id == output_id)
               {
                  EOM_DBG("send - id : %d, type : %d, mode : %d, w : %d, h : %d, w_mm : %d, h_mm : %d, conn : %d",
                          output->id, output->type, output->mode, output->width, output->height,
                          output->phys_width, output->phys_height, output->status);

                  wl_eom_send_output_info(resource, output->id, output->type, output->mode, output->width, output->height,
                                          output->phys_width, output->phys_height, output->status, 1, 0, 0, 0);
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

   RETURNIFTRUE(data == NULL, "data is NULL");

   eom = data;

   resource = wl_resource_create(client, &wl_eom_interface, MIN(version, 1), id);
   if (resource == NULL)
     {
        EOM_ERR("resource is null. (version :%d, id:%d)", version, id);
        wl_client_post_no_memory(client);
        return;
     }

   wl_resource_set_implementation(resource, &_e_eom_wl_implementation, eom, _e_eom_cb_wl_resource_destroy);

   EOM_DBG("send - output count : %d", g_eom->output_count);

   wl_eom_send_output_count(resource, g_eom->output_count);

   if (g_eom->outputs)
     {
        EINA_LIST_FOREACH(g_eom->outputs, l, output)
          {
             EOM_DBG("send - id : %d, type : %d, mode : %d, w : %d, h : %d, w_mm : %d, h_mm : %d, conn : %d",
                     output->id, output->type, output->mode, output->width, output->height,
                     output->phys_width, output->phys_height, output->status);
             wl_eom_send_output_info(resource, output->id, output->type, output->mode, output->width, output->height,
                                     output->phys_width, output->phys_height, output->status,
                                     1, 0, 0, 0);
          }
     }

   new_client = E_NEW(E_EomClient, 1);
   RETURNIFTRUE(new_client == NULL, "Allocate new client")

   new_client->resource = resource;
   new_client->current = EINA_FALSE;
   new_client->output_id = -1;
   new_client->ec = NULL;
   new_client->buffers_show = NULL;
   new_client->buffers_del = NULL;
   new_client->first_buffer = EINA_TRUE;

   g_eom->clients = eina_list_append(g_eom->clients, new_client);
}

static Eina_Bool
_e_eom_cb_ecore_drm_activate(void *data, int type EINA_UNUSED, void *event)
{
 /*
   Ecore_Drm_Event_Activate *e = NULL;
   E_EomPtr eom = NULL;

   EOM_DBG("_e_eom_cb_ecore_drm_activate called");

   if ((!event) || (!data)) goto end;
   e = event;
   eom = data;

   EOM_DBG("e->active:%d", e->active);

   if (e->active)
     {
        ;
     }
   else
     {
        ;
     }

end:

*/
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
                  EOM_ERR("output is null fail");
                  continue;
               }

             ret = tdm_output_get_conn_status(output, &status);
             if (ret != TDM_ERROR_NONE)
               {
                  EOM_ERR("tdm_output_get_conn_status fail(%d)", ret);
                  continue;
               }

             if (status == TDM_OUTPUT_CONN_STATUS_DISCONNECTED)
               continue;

             ret = tdm_output_get_output_type(output, &tdm_type);
             if (ret != TDM_ERROR_NONE)
               {
                  EOM_ERR("tdm_output_get_output_type fail(%d)", ret);
                  continue;
               }

             if (tdm_type < ALEN(eom_conn_types))
               tmp_name = eom_conn_types[tdm_type];
             else
               tmp_name = "unknown";
             /*TODO: What if there will more then one output of same type.
                        *e.g. "HDMI and HDMI" "LVDS and LVDS"*/
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

   EOM_DBG("id:%d (x,y,w,h):(%d,%d,%d,%d) (w_mm,h_mm):(%d,%d) refresh:%d subpixel_order:%d transform:%d make:%s model:%s name:%s plug:%d",
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
        if (client &&
            client->current == EINA_TRUE &&
            client->ec == ec)
          return client;
     }

   return NULL;
}

static E_EomClientBufferPtr
_e_eom_util_create_client_buffer(E_EomClientPtr client, E_Comp_Wl_Buffer *wl_buffer, tbm_surface_h tbm_buffer)
{
   E_EomClientBufferPtr buffer = NULL;

   buffer = E_NEW(E_EomClientBuffer, 1);
   RETURNVALIFTRUE(buffer == NULL, NULL, "Allocate new client buffer")

   /* TODO: Internal and External frame rate are same */

   /* Forbid E sending 'wl_buffer_send_release' event to external clients */
   wl_buffer->busy++;

   buffer->wl_buffer = wl_buffer;
   buffer->tbm_buffer = tbm_buffer;

   EOM_DBG("new client buffer wl:%p tbm:%p",
           buffer->wl_buffer, buffer->tbm_buffer);

#if 0
   buffer->stamp = _e_eom_util_get_stamp();
#endif

   return buffer;
}

static Eina_Bool
_e_eom_cb_client_buffer_change(void *data, int type, void *event)
{
   E_Comp_Wl_Buffer *external_wl_buffer = NULL;
   E_EomClientBufferPtr client_buffer = NULL;
   E_EomClientPtr eom_client = NULL;
   E_EomOutputPtr eom_output = NULL;
   E_Event_Client *ev = event;
   E_Client *ec = NULL;
   tbm_surface_h external_tbm_buffer = NULL;
/*
   tbm_surface_info_s surface_info;
   int ret;
*/

   EINA_SAFETY_ON_NULL_RETURN_VAL(ev, ECORE_CALLBACK_PASS_ON);
   EINA_SAFETY_ON_NULL_RETURN_VAL(ev->ec, ECORE_CALLBACK_PASS_ON);

   ec = ev->ec;
   RETURNVALIFTRUE(e_object_is_del(E_OBJECT(ec)), ECORE_CALLBACK_PASS_ON, "ec objects is del");

   eom_client = _e_eom_client_get_current_by_ec(ec);
   if (eom_client== NULL)
     return ECORE_CALLBACK_PASS_ON;
/*
   RETURNVALIFTRUE(eom_client == NULL,
                   ECORE_CALLBACK_PASS_ON,
                   "Current client is NULL");
*/

   eom_output = _e_eom_output_get_by_id(eom_client->output_id);
   RETURNVALIFTRUE(eom_output == NULL, ECORE_CALLBACK_PASS_ON, "eom_output is NULL");

   if (ec->pixmap == NULL)
     return ECORE_CALLBACK_PASS_ON;

   external_wl_buffer = e_pixmap_resource_get(ec->pixmap);
   RETURNVALIFTRUE(external_wl_buffer == NULL, ECORE_CALLBACK_PASS_ON, "wl buffer is NULL");
   RETURNVALIFTRUE(external_wl_buffer->resource == NULL, ECORE_CALLBACK_PASS_ON, "resource is NULL");

/*
   EOM_DBG("wl_buff:%p type:%d %dx%d ",
           external_wl_buffer,
           external_wl_buffer->type,
           external_wl_buffer->w,
           external_wl_buffer->h,
           );
*/

   /* Since Enlightenment client has reconfigured its window to fit
    * external output resolution and Enlightenment no nothing about
    * external outputs Enlightenment sees that client's resolution
    * differs form main screen resolution. Therefore, Enlightenment
    * is trying to resize it back to main screen resolution. It uses
    * timer for that purpose. To forbid it just delte the timer */

   /* TODO: it works but maybe there is better solution exists ?
    * Also I do not know how it affects on performance */
   if (ec->map_timer)
     {
        EOM_DBG("delete map_timer");
        E_FREE_FUNC(ec->map_timer, ecore_timer_del);
     }

   /* TODO: support buffers smaller then output resolution */
   if (external_wl_buffer->w != eom_output->width ||
       external_wl_buffer->h != eom_output->height )
     {
        EOM_ERR("tbm_buffer does not fit output's resolution");
        return ECORE_CALLBACK_PASS_ON;
     }

   /* TODO: support different external_wl_buffer->type */

   external_tbm_buffer = wayland_tbm_server_get_surface(
                            e_comp->wl_comp_data->tbm.server,
                            external_wl_buffer->resource);
   RETURNVALIFTRUE(external_tbm_buffer == NULL, ECORE_CALLBACK_PASS_ON, "Client tbm buffer is NULL");

   /* EOM_DBG("tbm_buffer %p", external_tbm_buffer); */

#if 0
   _e_eom_util_draw(external_tbm_buffer);
#endif

   EOM_DBG("BUFF_CHANGE >>>>>>>>>>");
   client_buffer = _e_eom_util_create_client_buffer(eom_client, external_wl_buffer, external_tbm_buffer);
   RETURNVALIFTRUE(client_buffer == NULL, ECORE_CALLBACK_PASS_ON, "Alloc client buffer");

   eom_client->buffers_show = eina_list_append(eom_client->buffers_show, client_buffer);
   EOM_DBG("BUFF_CHANGE <<<<<<<<<<");

   eom_output->state = PRESENTATION;

   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
_e_eom_init()
{
   Eina_Bool ret = EINA_FALSE;
   uint32_t id = 0;

   _e_eom_util_get_debug_env();

   EINA_SAFETY_ON_NULL_GOTO(e_comp_wl, err);

   g_eom = E_NEW(E_Eom, 1);
   EINA_SAFETY_ON_NULL_RETURN_VAL(g_eom, EINA_FALSE);

   g_eom->global = wl_global_create(e_comp_wl->wl.disp, &wl_eom_interface, 1,
                           g_eom, _e_eom_cb_wl_bind);

   id = wl_display_get_serial(e_comp_wl->wl.disp);
   EOM_DBG("eom name: %d", id);

   EINA_SAFETY_ON_NULL_GOTO(g_eom->global, err);

   ret = _e_eom_init_internal();
   GOTOIFTRUE(ret == EINA_FALSE, err, "init_internal() failed");

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
