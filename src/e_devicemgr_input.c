#include "e.h"
#include "e_mod_main.h"
#include "e_devicemgr_privates.h"
#include "e_devicemgr_input.h"
#include "e_devicemgr_device.h"

#include <Ecore_Drm.h>
static Eina_List *handlers = NULL;
static Eina_Bool remapped = EINA_FALSE;
static Ecore_Event_Filter *ev_filter = NULL;

extern E_Devicemgr_Config_Data *dconfig;

static Eina_Bool
_cb_input_dev_add(void *data EINA_UNUSED, int type EINA_UNUSED, void *event)
{
   Ecore_Event_Device_Info *e;
   Eina_List *devices, *l, *l2, *l3;
   Ecore_Drm_Device *dev;
   Ecore_Drm_Seat *seat;
   Ecore_Drm_Evdev *edev;
   Eina_Bool ret = EINA_FALSE;
   const char *name;

   int from_keycode = KEY_EXIT;
   int to_keycode = KEY_BACK;

   if (remapped) return ECORE_CALLBACK_PASS_ON;
   if (!(e = event)) return ECORE_CALLBACK_PASS_ON;
   if (!(e->clas == ECORE_DEVICE_CLASS_KEYBOARD)) return ECORE_CALLBACK_PASS_ON;
   if (!(e->name) || strcmp(e->name, "au0828 IR (Hauppauge HVR950Q)"))
     return ECORE_CALLBACK_PASS_ON;

   devices = eina_list_clone(ecore_drm_devices_get());
   EINA_LIST_FOREACH(devices, l, dev)
     {
        EINA_LIST_FOREACH(dev->seats, l2, seat)
          {
             EINA_LIST_FOREACH(ecore_drm_seat_evdev_list_get(seat), l3, edev)
               {
                  name = ecore_drm_evdev_name_get(edev);

                  if ((name) && (!strcmp(name, e->name)))
                    {
                       ret = ecore_drm_evdev_key_remap_enable(edev, EINA_TRUE);

                       if (!ret)
                         {
                            DBG("[DEVMGR] Failed to enable ecore_drm_evdev_key_remap !");
                            eina_list_free(devices);
                            return ECORE_CALLBACK_PASS_ON;
                         }

                       ret = ecore_drm_evdev_key_remap_set(edev, &from_keycode, &to_keycode, 1);

                       if (!ret)
                         {
                            DBG("[DEVMGR] Failed to enable ecore_drm_evdev_key_remap_set !");
                            eina_list_free(devices);
                            return ECORE_CALLBACK_PASS_ON;
                         }

                       remapped = EINA_TRUE;
                       eina_list_free(devices);
                       return ECORE_CALLBACK_PASS_ON;
                    }
               }
          }
     }
   eina_list_free(devices);

   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
_cb_input_dev_del(void *data EINA_UNUSED, int type EINA_UNUSED, void *event)
{
   Ecore_Event_Device_Info *e;

   if(!(e = event)) return ECORE_CALLBACK_PASS_ON;

   if (e->name && !strcmp(e->name, "au0828 IR (Hauppauge HVR950Q)"))
     remapped = EINA_FALSE;

   return ECORE_CALLBACK_PASS_ON;
}

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
   (void) data;

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
   else if (ECORE_EVENT_MOUSE_WHEEL == type)
     {
        return e_devicemgr_detent_check(type, event);
     }

   return ECORE_CALLBACK_PASS_ON;
}


int
e_devicemgr_input_init(void)
{
   E_LIST_HANDLER_APPEND(handlers, ECORE_EVENT_DEVICE_ADD, _cb_input_dev_add, NULL);
   E_LIST_HANDLER_APPEND(handlers, ECORE_EVENT_DEVICE_DEL, _cb_input_dev_del, NULL);

   /* add event filter for blocking events */
   ev_filter = ecore_event_filter_add(NULL, _e_devicemgr_event_filter, NULL, NULL);

   DMDBG("input.button_remap_enable: %d\n", dconfig->conf->input.button_remap_enable);

   return 1;
}

void
e_devicemgr_input_fini(void)
{
   E_FREE_LIST(handlers, ecore_event_handler_del);

   /* remove existing event filter */
   ecore_event_filter_del(ev_filter);
}
