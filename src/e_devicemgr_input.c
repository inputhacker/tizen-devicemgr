#include "e.h"
#include "e_mod_main.h"
#include "e_devicemgr_privates.h"
#include "e_devicemgr_input.h"
#include "e_devicemgr_device.h"

static Ecore_Event_Filter *ev_filter = NULL;
static int virtual_key_device_fd = -1;
static int virtual_mouse_device_fd = -1;

extern E_Devicemgr_Config_Data *dconfig;

static void
_e_devicemgr_input_keyevent_free(void *data EINA_UNUSED, void *ev)
{
   Ecore_Event_Key *e = ev;

   eina_stringshare_del(e->keyname);
   eina_stringshare_del(e->key);
   eina_stringshare_del(e->compose);

   E_FREE(e);
}

static Eina_Bool
_e_devicemgr_input_pointer_mouse_remap(int type, void *event)
{
   Ecore_Event_Key *ev_key;
   Ecore_Event_Mouse_Button *ev;

   if (type == ECORE_EVENT_MOUSE_MOVE) return ECORE_CALLBACK_PASS_ON;
   EINA_SAFETY_ON_NULL_RETURN_VAL(e_comp_wl->xkb.keymap, ECORE_CALLBACK_PASS_ON);

   ev = (Ecore_Event_Mouse_Button *)event;
   EINA_SAFETY_ON_NULL_RETURN_VAL(ev, ECORE_CALLBACK_PASS_ON);

   if (ev->buttons != 3) return ECORE_CALLBACK_PASS_ON;

   ev_key = E_NEW(Ecore_Event_Key, 1);
   EINA_SAFETY_ON_NULL_RETURN_VAL(ev_key, ECORE_CALLBACK_PASS_ON);

   ev_key->key = (char *)eina_stringshare_add("XF86Back");
   ev_key->keyname = (char *)eina_stringshare_add(ev_key->key);
   ev_key->compose = (char *)eina_stringshare_add(ev_key->key);
   ev_key->timestamp = (int)(ecore_time_get()*1000);
   ev_key->same_screen = 1;
   ev_key->keycode = dconfig->conf->input.back_keycode;

   if (type == ECORE_EVENT_MOUSE_BUTTON_DOWN)
     ecore_event_add(ECORE_EVENT_KEY_DOWN, ev_key, _e_devicemgr_input_keyevent_free, NULL);
   else if (type == ECORE_EVENT_MOUSE_BUTTON_UP)
     ecore_event_add(ECORE_EVENT_KEY_UP, ev_key, _e_devicemgr_input_keyevent_free, NULL);

   return ECORE_CALLBACK_DONE;
}

static Eina_Bool
_e_devicemgr_input_pointer_process(int type, void *event)
{
   Eina_Bool res = ECORE_CALLBACK_PASS_ON;

   res = e_devicemgr_block_check_pointer(type, event);
   if (res == ECORE_CALLBACK_DONE) return res;

   if (dconfig->conf->input.button_remap_enable)
     res = _e_devicemgr_input_pointer_mouse_remap(type, event);

   return res;
}

static Eina_Bool
_e_devicemgr_input_keyboard_process(int type, void *event)
{
   Eina_Bool res = ECORE_CALLBACK_PASS_ON;

   res = e_devicemgr_block_check_keyboard(type, event);

   return res;
}

static Eina_Bool
_e_devicemgr_event_filter(void *data, void *loop_data EINA_UNUSED, int type, void *event)
{
   Eina_Bool res = ECORE_CALLBACK_PASS_ON;

   (void) data;

   if (ECORE_EVENT_MOUSE_MOVE == type || ECORE_EVENT_MOUSE_WHEEL == type)
     res = e_devicemgr_detent_check(type, event);

   if (res != ECORE_CALLBACK_PASS_ON)
     return ECORE_CALLBACK_DONE;

   if (ECORE_EVENT_KEY_DOWN == type || ECORE_EVENT_KEY_UP == type)
     {
        return _e_devicemgr_input_keyboard_process(type, event);
     }
   else if (ECORE_EVENT_MOUSE_BUTTON_DOWN == type ||
           ECORE_EVENT_MOUSE_BUTTON_UP == type ||
           ECORE_EVENT_MOUSE_MOVE == type)
     {
        return _e_devicemgr_input_pointer_process(type, event);
     }
   else if (ECORE_EVENT_DEVICE_ADD == type)
     {
        return e_devicemgr_check_detent_device_add(type, event);
     }
   else if (ECORE_DRM_EVENT_INPUT_DEVICE_ADD == type)
     {
        return e_devicemgr_check_detent_device_add(type, event);
     }

   return ECORE_CALLBACK_PASS_ON;
}

int
e_devicemgr_input_init(void)
{
   /* add event filter for blocking events */
   ev_filter = ecore_event_filter_add(NULL, _e_devicemgr_event_filter, NULL, NULL);

   DMDBG("input.button_remap_enable: %d\n", dconfig->conf->input.button_remap_enable);

   if (dconfig->conf->input.virtual_key_device_enable)
     {
        virtual_key_device_fd = e_devicemgr_create_virtual_device(E_DEVICEMGR_DEVICE_TYPE_KEY, "Virtual Key Device");

        if (virtual_key_device_fd >= 0)
          DMDBG("input.virtual_key_device_enable: 1, device fd : %d\n", virtual_key_device_fd);
        else
          DMDBG("input.virtual_key_device_enable: 1, but failed to create device !\n");
     }

   if (dconfig->conf->input.virtual_mouse_device_enable)
     {
        virtual_mouse_device_fd = e_devicemgr_create_virtual_device(E_DEVICEMGR_DEVICE_TYPE_MOUSE, "Virtual Mouse Device");

        if (virtual_mouse_device_fd >= 0)
          DMDBG("input.virtual_mouse_device_enable: 1, device fd : %d\n", virtual_mouse_device_fd);
        else
          DMDBG("input.virtual_mouse_device_enable: 1, but failed to create device !\n");
     }

   return 1;
}

void
e_devicemgr_input_fini(void)
{
   /* remove existing event filter */
   ecore_event_filter_del(ev_filter);

   if (virtual_key_device_fd)
     {
        e_devicemgr_destroy_virtual_device(virtual_key_device_fd);
        virtual_key_device_fd = -1;
     }

   if (virtual_mouse_device_fd)
     {
        e_devicemgr_destroy_virtual_device(virtual_mouse_device_fd);
        virtual_mouse_device_fd = -1;
     }
}
