#include "e_devicemgr_device.h"
#include "e_devicemgr_privates.h"
#include <tizen-extension-server-protocol.h>
#include <Ecore_Drm.h>

static Eina_List *handlers = NULL;

e_devicemgr_input_devmgr_data *input_devmgr_data;

#ifdef ENABLE_CYNARA
static void _e_devicemgr_util_cynara_log(const char *func_name, int err);
static Eina_Bool _e_devicemgr_util_do_privilege_check(struct wl_client *client, int socket_fd, const char *rule);

#define E_DEVMGR_CYNARA_ERROR_CHECK_GOTO(func_name, ret, label) \
  do \
    { \
       if (EINA_UNLIKELY(CYNARA_API_SUCCESS != ret)) \
          { \
             _e_devicemgr_util_cynara_log(func_name, ret); \
             goto label; \
          } \
    } \
  while (0)

static void
_e_devicemgr_util_cynara_log(const char *func_name, int err)
{
#define CYNARA_BUFSIZE 128
   char buf[CYNARA_BUFSIZE] = "\0";
   int ret;

   ret = cynara_strerror(err, buf, CYNARA_BUFSIZE);
   if (ret != CYNARA_API_SUCCESS)
     {
        DMDBG("Failed to cynara_strerror: %d (error log about %s: %d)\n", ret, func_name, err);
        return;
     }
   DMDBG("%s is failed: %s\n", func_name, buf);
}

static Eina_Bool
_e_devicemgr_util_do_privilege_check(struct wl_client *client, int socket_fd, const char *rule)
{
   int ret, pid;
   char *clientSmack=NULL, *uid=NULL, *client_session=NULL;
   Eina_Bool res = EINA_FALSE;

   /* If initialization of cynara has been failed, let's not to do further permission checks. */
   if (input_devmgr_data->p_cynara == NULL && input_devmgr_data->cynara_initialized) return EINA_TRUE;

   ret = cynara_creds_socket_get_client(socket_fd, CLIENT_METHOD_SMACK, &clientSmack);
   E_DEVMGR_CYNARA_ERROR_CHECK_GOTO("cynara_creds_socket_get_client", ret, finish);

   ret = cynara_creds_socket_get_user(socket_fd, USER_METHOD_UID, &uid);
   E_DEVMGR_CYNARA_ERROR_CHECK_GOTO("cynara_creds_socket_get_user", ret, finish);

   ret = cynara_creds_socket_get_pid(socket_fd, &pid);
   E_DEVMGR_CYNARA_ERROR_CHECK_GOTO("cynara_creds_socket_get_pid", ret, finish);

   client_session = cynara_session_from_pid(pid);

   ret = cynara_check(input_devmgr_data->p_cynara, clientSmack, client_session, uid, rule);

   if (CYNARA_API_ACCESS_ALLOWED == ret)
        res = EINA_TRUE;

finish:
   E_FREE(client_session);
   E_FREE(clientSmack);
   E_FREE(uid);

   return res;
}
#endif

static void
_e_device_mgr_device_cb_axes_select(struct wl_client *client, struct wl_resource *resource, struct wl_array *axes)
{
   return;
}

static void
_e_devicemgr_device_cb_release(struct wl_client *client, struct wl_resource *resource)
{
   wl_resource_destroy(resource);
}

static const struct tizen_input_device_interface _e_devicemgr_device_interface =
{
   _e_device_mgr_device_cb_axes_select,
   _e_devicemgr_device_cb_release,
};

static void
_e_devicemgr_del_device(const char *name, const char *identifier, const char *seatname, Ecore_Device_Class clas)
{
   E_Comp_Wl_Input_Device *dev;
   struct wl_client *wc;
   Eina_List *l, *ll, *lll;
   struct wl_resource *res, *seat_res, *dev_mgr_res;
   uint32_t serial;
   e_devicemgr_input_device_user_data *device_user_data;

   if (!e_comp) return;
   if (!e_comp_wl) return;
   if (!e_comp_wl->wl.disp) return;

   TRACE_INPUT_BEGIN(_e_devicemgr_del_device);

   EINA_LIST_FOREACH(e_comp_wl->input_device_manager.device_list, l, dev)
     {
        if ((dev->clas == clas) && (!strcmp(dev->name, name))  && (!strcmp(dev->identifier, identifier)))
          break;
     }
   if (!dev)
     {
        TRACE_INPUT_END();
        return;
     }

   if ((input_devmgr_data->detent.identifier) &&
       (!strncmp(dev->name, "tizen_detent", sizeof("tizen_detent"))))
     {
        eina_stringshare_del(input_devmgr_data->detent.identifier);
     }

   if (dev->name) eina_stringshare_del(dev->name);
   if (dev->identifier) eina_stringshare_del(dev->identifier);

   serial = wl_display_next_serial(e_comp_wl->wl.disp);

   /* TODO: find the seat corresponding to event */
   EINA_LIST_FOREACH(e_comp_wl->seat.resources, l, seat_res)
     {
        wc = wl_resource_get_client(seat_res);
        EINA_LIST_FOREACH(e_comp_wl->input_device_manager.resources, ll, dev_mgr_res)
          {
             if (wl_resource_get_client(dev_mgr_res) != wc) continue;
             EINA_LIST_FOREACH(dev->resources, lll, res)
               {
                  if (wl_resource_get_client(res) != wc) continue;
                  device_user_data = wl_resource_get_user_data(res);
                  if (!device_user_data) continue;
                  if (device_user_data->dev_mgr_res != dev_mgr_res)
                    continue;
                  if (device_user_data->seat_res != seat_res)
                    continue;

                  tizen_input_device_manager_send_device_remove(dev_mgr_res, serial, dev->identifier, res, seat_res);
               }
          }
     }

   EINA_LIST_FREE(dev->resources, res)
     {
        device_user_data = wl_resource_get_user_data(res);
        if (device_user_data)
          {
             device_user_data->dev = NULL;
             device_user_data->dev_mgr_res = NULL;
             device_user_data->seat_res = NULL;
             E_FREE(device_user_data);
          }

        wl_resource_set_user_data(res, NULL);
     }

   e_comp_wl->input_device_manager.device_list = eina_list_remove(e_comp_wl->input_device_manager.device_list, dev);

   if (e_comp_wl->input_device_manager.last_device_ptr == dev)
     e_comp_wl->input_device_manager.last_device_ptr = NULL;

   free(dev);
   TRACE_INPUT_END();
}

static void
_e_devicemgr_device_cb_device_unbind(struct wl_resource *resource)
{
   E_Comp_Wl_Input_Device *dev;
   e_devicemgr_input_device_user_data *device_user_data;

   if (!(device_user_data = wl_resource_get_user_data(resource))) return;

   dev = device_user_data->dev;

   device_user_data->dev = NULL;
   device_user_data->dev_mgr_res = NULL;
   device_user_data->seat_res = NULL;
   E_FREE(device_user_data);

   if (!dev) return;

   dev->resources = eina_list_remove(dev->resources, resource);
}

static void
_e_devicemgr_add_device(const char *name, const char *identifier, const char *seatname, Ecore_Device_Class clas)
{
   E_Comp_Wl_Input_Device *dev;
   struct wl_client *wc;
   Eina_List *l, *ll, *l1, *l2, *l3;
   struct wl_resource *res, *seat_res, *dev_mgr_res;
   uint32_t serial;
   struct wl_array axes;
   Ecore_Drm_Device *drm_device_data = NULL;
   Ecore_Drm_Seat *seat = NULL;
   Ecore_Drm_Evdev *edev = NULL;
   int wheel_click_angle;
   Eina_List *dev_list;
   e_devicemgr_input_device_user_data *device_user_data;

   if (!e_comp) return;
   if (!e_comp_wl) return;
   if (!e_comp_wl->wl.disp) return;

   TRACE_INPUT_BEGIN(_e_devicemgr_add_device);

   EINA_LIST_FOREACH(e_comp_wl->input_device_manager.device_list, l, dev)
     {
        if ((dev->clas == clas) && (!strcmp(dev->identifier, identifier)))
          {
             TRACE_INPUT_END();
             return;
          }
     }

   if (!(dev = E_NEW(E_Comp_Wl_Input_Device, 1))) return;
   dev->name = eina_stringshare_add(name);
   dev->identifier = eina_stringshare_add(identifier);
   dev->clas = clas;

   wl_array_init(&axes);

   /* TODO: find the seat corresponding to event */
   serial = wl_display_next_serial(e_comp_wl->wl.disp);

   EINA_LIST_FOREACH(e_comp_wl->seat.resources, l, seat_res)
     {
        wc = wl_resource_get_client(seat_res);

        EINA_LIST_FOREACH(e_comp_wl->input_device_manager.resources, ll, dev_mgr_res)
          {
             if (wl_resource_get_client(dev_mgr_res) != wc) continue;
             res = wl_resource_create(wc, &tizen_input_device_interface, 1, 0);
             if (!res)
                 {
                  DMERR("Could not create tizen_input_device resource");
                  TRACE_INPUT_END();
                  return;
                }

             device_user_data = E_NEW(e_devicemgr_input_device_user_data, 1);
             if (!device_user_data)
               {
                  DMERR("Failed to allocate memory for input device user data\n");
                  TRACE_INPUT_END();
                  return;
               }
             device_user_data->dev = dev;
             device_user_data->dev_mgr_res = dev_mgr_res;
             device_user_data->seat_res = seat_res;

             dev->resources = eina_list_append(dev->resources, res);
             wl_resource_set_implementation(res, &_e_devicemgr_device_interface, device_user_data,
                                            _e_devicemgr_device_cb_device_unbind);
             tizen_input_device_manager_send_device_add(dev_mgr_res, serial, dev->identifier, res, seat_res);
             tizen_input_device_send_device_info(res, dev->name, dev->clas, TIZEN_INPUT_DEVICE_SUBCLAS_NONE, &axes);
          }
     }

   e_comp_wl->input_device_manager.device_list = eina_list_append(e_comp_wl->input_device_manager.device_list, dev);

   if ((!input_devmgr_data->inputgen.kbd.uinp_identifier) && (dev->clas == ECORE_DEVICE_CLASS_KEYBOARD) &&
       (dev->name && !strncmp(dev->name, "Input Generator", sizeof("Input Generator"))))
     {
        input_devmgr_data->inputgen.kbd.uinp_identifier = (char *)eina_stringshare_add(identifier);
     }
   if ((!input_devmgr_data->inputgen.ptr.uinp_identifier) && (dev->clas == ECORE_DEVICE_CLASS_MOUSE) &&
       (dev->name && !strncmp(dev->name, "Input Generator", sizeof("Input Generator"))))
     {
        input_devmgr_data->inputgen.ptr.uinp_identifier = (char *)eina_stringshare_add(identifier);
     }
   if ((!input_devmgr_data->inputgen.touch.uinp_identifier) && (dev->clas == ECORE_DEVICE_CLASS_TOUCH) &&
       (dev->name && !strncmp(dev->name, "Input Generator", sizeof("Input Generator"))))
     {
        input_devmgr_data->inputgen.touch.uinp_identifier = (char *)eina_stringshare_add(identifier);
     }

   if ((!input_devmgr_data->detent.identifier) &&
       (dev->name && !strncmp(dev->name, "tizen_detent", sizeof("tizen_detent"))))
     {
        input_devmgr_data->detent.identifier = (char *)eina_stringshare_add(identifier);
        dev_list = (Eina_List *)ecore_drm_devices_get();
        EINA_LIST_FOREACH(dev_list, l1, drm_device_data)
          {
             EINA_LIST_FOREACH(drm_device_data->seats, l2, seat)
               {
                  EINA_LIST_FOREACH(ecore_drm_seat_evdev_list_get(seat), l3, edev)
                    {
                       if (!strncmp(ecore_drm_evdev_name_get(edev), "tizen_detent", sizeof("tizen_detent")))
                         {
                            wheel_click_angle = ecore_drm_evdev_wheel_click_angle_get(edev);
                            input_devmgr_data->detent.wheel_click_angle = wheel_click_angle;
                         }
                    }
               }
          }
     }

   if (dev->clas == ECORE_DEVICE_CLASS_MOUSE)
     e_comp_wl->input_device_manager.last_device_ptr = dev;

   TRACE_INPUT_END();
}

static Eina_Bool
_cb_device_add(void *data EINA_UNUSED, int type EINA_UNUSED, void *event)
{
   Ecore_Event_Device_Info *e;

   if (!(e = event)) return ECORE_CALLBACK_PASS_ON;

   _e_devicemgr_add_device(e->name, e->identifier, e->seatname, e->clas);

   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
_cb_device_del(void *data EINA_UNUSED, int type EINA_UNUSED, void *event)
{
   Ecore_Event_Device_Info *e;

   if(!(e = event)) return ECORE_CALLBACK_PASS_ON;

   _e_devicemgr_del_device(e->name, e->identifier, e->seatname, e->clas);

   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
_e_devicemgr_block_check_button(int type, void *event)
{
   Ecore_Event_Mouse_Button *ev;
   Ecore_Device *dev;

   ev = event;
   EINA_SAFETY_ON_NULL_RETURN_VAL(ev, ECORE_CALLBACK_PASS_ON);
   dev = ev->dev;
   EINA_SAFETY_ON_NULL_RETURN_VAL(dev, ECORE_CALLBACK_PASS_ON);

   if (ecore_device_class_get(dev) == ECORE_DEVICE_CLASS_MOUSE)
     {
        if (input_devmgr_data->block_devtype & TIZEN_INPUT_DEVICE_MANAGER_CLAS_MOUSE)
          {
             if (type == ECORE_EVENT_MOUSE_BUTTON_UP)
               {
                  if (input_devmgr_data->pressed_button & (1 << ev->buttons))
                    {
                       input_devmgr_data->pressed_button &= ~(1 << ev->buttons);
                       return ECORE_CALLBACK_PASS_ON;
                    }
               }
             return ECORE_CALLBACK_DONE;
          }

        if (type == ECORE_EVENT_MOUSE_BUTTON_DOWN)
          {
             input_devmgr_data->pressed_button |= (1 << ev->buttons);
          }
        else if (type == ECORE_EVENT_MOUSE_BUTTON_UP)
          {
             input_devmgr_data->pressed_button &= ~(1 << ev->buttons);
          }
     }
   else if (ecore_device_class_get(dev) == ECORE_DEVICE_CLASS_TOUCH)
     {
        if (input_devmgr_data->block_devtype & TIZEN_INPUT_DEVICE_MANAGER_CLAS_TOUCHSCREEN)
          {
             if (type == ECORE_EVENT_MOUSE_BUTTON_UP)
               {
                  if (input_devmgr_data->pressed_finger & (1 << ev->multi.device))
                    {
                       input_devmgr_data->pressed_finger &= ~(1 << ev->multi.device);
                       return ECORE_CALLBACK_PASS_ON;
                    }
               }
             return ECORE_CALLBACK_DONE;
          }

        if (type == ECORE_EVENT_MOUSE_BUTTON_DOWN)
          {
             input_devmgr_data->pressed_finger |= (1 << ev->multi.device);
          }
        else if (type == ECORE_EVENT_MOUSE_BUTTON_UP)
          {
             input_devmgr_data->pressed_finger &= ~(1 << ev->multi.device);
          }
     }

   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
_e_devicemgr_block_check_move(int type, void *event)
{
   Ecore_Event_Mouse_Move *ev;
   Ecore_Device *dev;

   ev = event;
   EINA_SAFETY_ON_NULL_RETURN_VAL(ev, ECORE_CALLBACK_PASS_ON);
   dev = ev->dev;
   EINA_SAFETY_ON_NULL_RETURN_VAL(dev, ECORE_CALLBACK_PASS_ON);

   if (ecore_device_class_get(dev) == ECORE_DEVICE_CLASS_MOUSE)
     {
        if (input_devmgr_data->block_devtype & TIZEN_INPUT_DEVICE_MANAGER_CLAS_MOUSE)
          {
             return ECORE_CALLBACK_DONE;
          }
     }
   else if (ecore_device_class_get(dev) == ECORE_DEVICE_CLASS_TOUCH)
     {
        if (input_devmgr_data->block_devtype & TIZEN_INPUT_DEVICE_MANAGER_CLAS_TOUCHSCREEN)
          {
             return ECORE_CALLBACK_DONE;
          }
     }

   return ECORE_CALLBACK_PASS_ON;
}

Eina_Bool
e_devicemgr_block_check_pointer(int type, void *event)
{
   Eina_Bool res = ECORE_CALLBACK_PASS_ON;

   if (type == ECORE_EVENT_MOUSE_BUTTON_DOWN ||
       type == ECORE_EVENT_MOUSE_BUTTON_UP)
     {
        res = _e_devicemgr_block_check_button(type, event);
     }
   else if (type == ECORE_EVENT_MOUSE_MOVE)
     {
        res = _e_devicemgr_block_check_move(type, event);
     }

   return res;
}

Eina_Bool
e_devicemgr_block_check_keyboard(int type, void *event)
{
   Ecore_Event_Key *ev;
   Eina_List *l, *l_next;
   int *keycode, *data;

   ev = event;
   EINA_SAFETY_ON_NULL_RETURN_VAL(ev, ECORE_CALLBACK_PASS_ON);

   if (input_devmgr_data->block_devtype & TIZEN_INPUT_DEVICE_MANAGER_CLAS_KEYBOARD)
     {
        if (type == ECORE_EVENT_KEY_UP)
          {
             EINA_LIST_FOREACH_SAFE(input_devmgr_data->pressed_keys, l, l_next, data)
               {
                  if (ev->keycode == *data)
                    {
                       DMERR("%d is already press key. Propagate this key event.\n", *data);
                       input_devmgr_data->pressed_keys = eina_list_remove_list(input_devmgr_data->pressed_keys, l);
                       E_FREE(data);
                       return ECORE_CALLBACK_PASS_ON;
                    }
               }
          }
        return ECORE_CALLBACK_DONE;
     }

   if (type == ECORE_EVENT_KEY_DOWN)
     {
        keycode = E_NEW(int, 1);
        EINA_SAFETY_ON_NULL_RETURN_VAL(keycode, ECORE_CALLBACK_PASS_ON);

        *keycode = ev->keycode;

        EINA_LIST_FOREACH(input_devmgr_data->pressed_keys, l, data)
          {
             if (*data == *keycode)
               {
                  E_FREE(keycode);
                  return ECORE_CALLBACK_PASS_ON;
               }
          }
        input_devmgr_data->pressed_keys = eina_list_append(input_devmgr_data->pressed_keys, keycode);
     }
   else
     {
        EINA_LIST_FOREACH_SAFE(input_devmgr_data->pressed_keys, l, l_next, data)
          {
             if (ev->keycode == *data)
               {
                  input_devmgr_data->pressed_keys = eina_list_remove_list(input_devmgr_data->pressed_keys, l);
                  E_FREE(data);
               }
          }
     }

   return ECORE_CALLBACK_PASS_ON;
}

static void
_e_devicemgr_send_detent_event(int detent)
{
   E_Comp_Wl_Input_Device *input_dev;
   struct wl_resource *dev_res;
   struct wl_client *wc;
   Eina_List *l, *ll;
   wl_fixed_t f_value;
   E_Client *ec;

   ec = e_client_focused_get();

   if (!ec) return;
   if (e_object_is_del(E_OBJECT(ec))) return;
   if (ec->ignored) return;
   if (!ec->comp_data || !ec->comp_data->surface) return;

   f_value = wl_fixed_from_double(detent*1.0);
   wc = wl_resource_get_client(ec->comp_data->surface);

   EINA_LIST_FOREACH(e_comp_wl->input_device_manager.device_list, l, input_dev)
     {
        if (!strncmp(input_dev->name, "tizen_detent", sizeof("tizen_detent")))
          {
             EINA_LIST_FOREACH(input_dev->resources, ll, dev_res)
               {
                  if (wl_resource_get_client(dev_res) != wc) continue;
                  tizen_input_device_send_axis(dev_res, TIZEN_INPUT_DEVICE_AXIS_TYPE_DETENT, f_value);
               }
          }
     }
}

Eina_Bool
e_devicemgr_detent_check(int type EINA_UNUSED, void *event)
{
   Ecore_Event_Mouse_Wheel *ev;
   int detent;
   const char *name;

   ev = event;
   EINA_SAFETY_ON_NULL_RETURN_VAL(ev, ECORE_CALLBACK_PASS_ON);
   if (!ev->dev) return ECORE_CALLBACK_PASS_ON;

   name = ecore_device_identifier_get(ev->dev);
   if (!name) return ECORE_CALLBACK_PASS_ON;

   if ((input_devmgr_data->detent.identifier) &&
       (!strncmp(name, input_devmgr_data->detent.identifier,
                 eina_stringshare_strlen(input_devmgr_data->detent.identifier))))
     {
        detent = (int)(ev->z / (input_devmgr_data->detent.wheel_click_angle ? input_devmgr_data->detent.wheel_click_angle : 1));
        if (detent == 2 || detent == -2)
          {
             detent = (detent / 2)*(-1);
             _e_devicemgr_send_detent_event(detent);
          }

        return ECORE_CALLBACK_DONE;
     }

   return ECORE_CALLBACK_PASS_ON;
}

static void
_e_input_devmgr_request_client_remove(struct wl_client *client)
{
   if (client != input_devmgr_data->block_client) return;

   input_devmgr_data->block_devtype = 0x0;
   if (input_devmgr_data->duration_timer)
     {
        ecore_timer_del(input_devmgr_data->duration_timer);
        input_devmgr_data->duration_timer = NULL;
     }
   input_devmgr_data->block_client = NULL;
}

static Eina_Bool
_e_input_devmgr_cb_block_timer(void *data)
{
   struct wl_resource *resource = (struct wl_resource *)data;
   struct wl_client *client = wl_resource_get_client(resource);

   if ((input_devmgr_data->block_client) && (input_devmgr_data->block_client != client))
     {
        return ECORE_CALLBACK_CANCEL;
     }

   _e_input_devmgr_request_client_remove(client);
   tizen_input_device_manager_send_block_expired(resource);

   return ECORE_CALLBACK_CANCEL;
}

static void
_e_input_devmgr_client_cb_destroy(struct wl_listener *l, void *data)
{
   struct wl_client *client = (struct wl_client *)data;

   if (!input_devmgr_data->block_client) return;

   wl_list_remove(&l->link);
   E_FREE(l);

   _e_input_devmgr_request_client_remove(client);
}

static void
_e_input_devmgr_request_client_add(struct wl_client *client, struct wl_resource *resource, uint32_t clas, uint32_t duration)
{
   struct wl_listener *destroy_listener = NULL;
   double milli_duration = duration / 1000;

   /* Last request of block can renew timer time */
   if (input_devmgr_data->duration_timer)
     ecore_timer_del(input_devmgr_data->duration_timer);
   input_devmgr_data->duration_timer = ecore_timer_add(milli_duration, _e_input_devmgr_cb_block_timer, resource);

   input_devmgr_data->block_devtype |= clas;

   if (input_devmgr_data->block_client) return;
   input_devmgr_data->block_client = client;

   destroy_listener = E_NEW(struct wl_listener, 1);
   destroy_listener->notify = _e_input_devmgr_client_cb_destroy;
   wl_client_add_destroy_listener(client, destroy_listener);
}

static void
_e_input_devmgr_cb_block_events(struct wl_client *client, struct wl_resource *resource,
                             uint32_t serial, uint32_t clas, uint32_t duration)
{
   uint32_t all_class = TIZEN_INPUT_DEVICE_MANAGER_CLAS_MOUSE |
                        TIZEN_INPUT_DEVICE_MANAGER_CLAS_KEYBOARD |
                        TIZEN_INPUT_DEVICE_MANAGER_CLAS_TOUCHSCREEN;

#ifdef ENABLE_CYNARA
   if (EINA_FALSE == _e_devicemgr_util_do_privilege_check(client, wl_client_get_fd(client), "http://tizen.org/privilege/internal/inputdevice.block"))
     {
        DMERR("_e_input_devmgr_cb_block_events:priv check failed");
        return;
     }
#endif

   if ((input_devmgr_data->block_client) && (input_devmgr_data->block_client != client))
     {
        tizen_input_device_manager_send_error(resource, TIZEN_INPUT_DEVICE_MANAGER_ERROR_BLOCKED_ALREADY);
        return;
     }
   if (!(clas & all_class))
     {
        tizen_input_device_manager_send_error(resource, TIZEN_INPUT_DEVICE_MANAGER_ERROR_INVALID_CLASS);
        return;
     }

   _e_input_devmgr_request_client_add(client, resource, clas, duration);
   tizen_input_device_manager_send_error(resource, TIZEN_INPUT_DEVICE_MANAGER_ERROR_NONE);

   /* TODO: Release pressed button or key */
}

static void
_e_input_devmgr_cb_unblock_events(struct wl_client *client, struct wl_resource *resource,
                             uint32_t serial)
{
#ifdef ENABLE_CYNARA
   if (EINA_FALSE == _e_devicemgr_util_do_privilege_check(client, wl_client_get_fd(client), "http://tizen.org/privilege/internal/inputdevice.block"))
     {
        DMERR("_e_input_devmgr_cb_unblock_events:priv check failed");
        return;
     }
#endif

  if ((input_devmgr_data->block_client) && (input_devmgr_data->block_client != client))
    {
       tizen_input_device_manager_send_error(resource, TIZEN_INPUT_DEVICE_MANAGER_ERROR_BLOCKED_ALREADY);
       return;
    }

   _e_input_devmgr_request_client_remove(client);
   tizen_input_device_manager_send_error(resource, TIZEN_INPUT_DEVICE_MANAGER_ERROR_NONE);
}

typedef struct _keycode_map{
    xkb_keysym_t keysym;
    xkb_keycode_t *keycodes;
    int nkeycodes;
}keycode_map;

static void
find_keycode(struct xkb_keymap *keymap, xkb_keycode_t key, void *data)
{
   keycode_map *found_keycodes = (keycode_map *)data;
   xkb_keysym_t keysym = found_keycodes->keysym;
   int nsyms = 0;
   const xkb_keysym_t *syms_out = NULL;

   nsyms = xkb_keymap_key_get_syms_by_level(keymap, key, 0, 0, &syms_out);
   if (nsyms && syms_out)
     {
        if (*syms_out == keysym)
          {
             found_keycodes->nkeycodes++;
             found_keycodes->keycodes = realloc(found_keycodes->keycodes, sizeof(int)*found_keycodes->nkeycodes);
             found_keycodes->keycodes[found_keycodes->nkeycodes-1] = key;
          }
     }
}

int
_e_input_devmgr_keycode_from_keysym(struct xkb_keymap *keymap, xkb_keysym_t keysym, xkb_keycode_t **keycodes)
{
    keycode_map found_keycodes = {0,};
    found_keycodes.keysym = keysym;
    xkb_keymap_key_for_each(keymap, find_keycode, &found_keycodes);

    *keycodes = found_keycodes.keycodes;
    return found_keycodes.nkeycodes;
}

static int
_e_input_devmgr_keycode_from_string(const char *keyname)
{
   xkb_keysym_t keysym = 0x0;
   int nkeycodes=0;
   xkb_keycode_t *keycodes = NULL;
   int keycode = 0;

   if (!strncmp(keyname, "Keycode-", sizeof("Keycode-")-1))
     {
        keycode = atoi(keyname+8);
     }
   else
     {
        keysym = xkb_keysym_from_name(keyname, XKB_KEYSYM_NO_FLAGS);
        nkeycodes = _e_input_devmgr_keycode_from_keysym(e_comp_wl->xkb.keymap, keysym, &keycodes);
        if (nkeycodes > 0)
          {
             keycode = keycodes[0];
          }

        free(keycodes);
        keycodes = NULL;
     }

   return keycode;
}

static void
_e_input_devmgr_inputgen_generator_remove_keyboard(void)
{
   if (input_devmgr_data->inputgen.kbd.uinp_fd < 0)
     {
        DMWRN("There are no devices created for input generation.\n");
        return;
     }

   close(input_devmgr_data->inputgen.kbd.uinp_fd);
   input_devmgr_data->inputgen.kbd.uinp_fd = -1;
   eina_stringshare_del(input_devmgr_data->inputgen.kbd.uinp_identifier);
   input_devmgr_data->inputgen.kbd.uinp_identifier = NULL;
}

static void
_e_input_devmgr_inputgen_generator_remove_mouse(void)
{
   if (input_devmgr_data->inputgen.ptr.uinp_fd < 0)
     {
        DMWRN("There are no devices created for input generation.\n");
        return;
     }

   close(input_devmgr_data->inputgen.ptr.uinp_fd);
   input_devmgr_data->inputgen.ptr.uinp_fd = -1;
   eina_stringshare_del(input_devmgr_data->inputgen.ptr.uinp_identifier);
   input_devmgr_data->inputgen.ptr.uinp_identifier = NULL;
}

static void
_e_input_devmgr_inputgen_generator_remove_touch(void)
{
   if (input_devmgr_data->inputgen.touch.uinp_fd < 0)
     {
        DMWRN("There are no devices created for input generation.\n");
        return;
     }

   close(input_devmgr_data->inputgen.touch.uinp_fd);
   input_devmgr_data->inputgen.touch.uinp_fd = -1;
   eina_stringshare_del(input_devmgr_data->inputgen.touch.uinp_identifier);
   input_devmgr_data->inputgen.touch.uinp_identifier = NULL;
}

static void
_e_input_devmgr_inputgen_client_cb_destroy(struct wl_listener *l, void *data)
{
   struct wl_client *client = (struct wl_client *)data;
   e_devicemgr_inputgen_client_data *cdata;
   Eina_List *list, *l_next;

   wl_list_remove(&l->link);
   E_FREE(l);

   EINA_LIST_FOREACH_SAFE(input_devmgr_data->inputgen.kbd.clients, list, l_next, cdata)
     {
        if (cdata->client == client)
          {
             input_devmgr_data->inputgen.kbd.clients =
                eina_list_remove_list(input_devmgr_data->inputgen.kbd.clients, list);
             E_FREE(cdata);
          }
     }
   EINA_LIST_FOREACH_SAFE(input_devmgr_data->inputgen.ptr.clients, list, l_next, cdata)
     {
        if (cdata->client == client)
          {
             input_devmgr_data->inputgen.ptr.clients =
                eina_list_remove_list(input_devmgr_data->inputgen.ptr.clients, list);
             E_FREE(cdata);
          }
     }
   EINA_LIST_FOREACH_SAFE(input_devmgr_data->inputgen.touch.clients, list, l_next, cdata)
     {
        if (cdata->client == client)
          {
             input_devmgr_data->inputgen.touch.clients =
                eina_list_remove_list(input_devmgr_data->inputgen.touch.clients, list);
             E_FREE(cdata);
          }
     }

   if (eina_list_count(input_devmgr_data->inputgen.kbd.clients) == 0)
     _e_input_devmgr_inputgen_generator_remove_keyboard();
   if (eina_list_count(input_devmgr_data->inputgen.ptr.clients) == 0)
     _e_input_devmgr_inputgen_generator_remove_mouse();
   if (eina_list_count(input_devmgr_data->inputgen.touch.clients) == 0)
     _e_input_devmgr_inputgen_generator_remove_touch();
}

static void
_e_input_devmgr_inputgen_client_add(struct wl_client *client, unsigned int clas)
{
   struct wl_listener *destroy_listener;
   e_devicemgr_inputgen_client_global_data *data;
   Eina_List *l;

   EINA_LIST_FOREACH(input_devmgr_data->watched_clients, l, data)
     {
        if (data->client == client)
          {
             data->clas |= clas;
             return;
          }
     }
   data = NULL;

   destroy_listener = E_NEW(struct wl_listener, 1);
   EINA_SAFETY_ON_NULL_GOTO(destroy_listener, failed);
   destroy_listener->notify = _e_input_devmgr_inputgen_client_cb_destroy;
   wl_client_add_destroy_listener(client, destroy_listener);

   data = E_NEW(e_devicemgr_inputgen_client_global_data, 1);
   EINA_SAFETY_ON_NULL_GOTO(data, failed);

   data->client = client;
   data->clas = clas;

   input_devmgr_data->watched_clients =
      eina_list_append(input_devmgr_data->watched_clients, data);

   return;

failed:
   if (destroy_listener)
     {
        wl_list_remove(&destroy_listener->link);
        E_FREE(destroy_listener);
     }
}

static Eina_List **
_e_input_devmgr_inputgen_util_list_get(unsigned int clas)
{
   if (clas == TIZEN_INPUT_DEVICE_MANAGER_CLAS_KEYBOARD)
     return &input_devmgr_data->inputgen.kbd.clients;
   else if (clas == TIZEN_INPUT_DEVICE_MANAGER_CLAS_MOUSE)
     return &input_devmgr_data->inputgen.ptr.clients;
   else if (clas == TIZEN_INPUT_DEVICE_MANAGER_CLAS_TOUCHSCREEN)
     return &input_devmgr_data->inputgen.touch.clients;
   else
     return NULL;
}

static void
_e_input_devmgr_inputgen_client_remove(struct wl_client *client, unsigned int clas)
{
   e_devicemgr_inputgen_client_global_data *data;
   e_devicemgr_inputgen_client_data *cdata;
   Eina_List *l, *l_next, **list;

   list = _e_input_devmgr_inputgen_util_list_get(clas);
   EINA_SAFETY_ON_NULL_RETURN(list);

   EINA_LIST_FOREACH_SAFE(*list, l, l_next, cdata)
     {
        if (cdata->client == client)
          {
             cdata->ref--;
             if (cdata->ref <= 0)
               {
                  *list = eina_list_remove_list(*list, l);
               }
          }
     }

   EINA_LIST_FOREACH(input_devmgr_data->watched_clients, l, data)
     {
        if (data->client == client)
          {
             data->clas &= ~clas;
          }
     }

   if (eina_list_count(*list) == 0)
     {
        if (clas == TIZEN_INPUT_DEVICE_MANAGER_CLAS_KEYBOARD)
          _e_input_devmgr_inputgen_generator_remove_keyboard();
        else if (clas == TIZEN_INPUT_DEVICE_MANAGER_CLAS_MOUSE)
          _e_input_devmgr_inputgen_generator_remove_mouse();
        else if (clas == TIZEN_INPUT_DEVICE_MANAGER_CLAS_TOUCHSCREEN)
          _e_input_devmgr_inputgen_generator_remove_touch();
     }
}

static int
_e_input_devmgr_create_keyboard_device(struct wl_client *client)
{
   int uinp_fd = -1, ret = -1;
   struct uinput_user_dev *uinp = &input_devmgr_data->inputgen.uinp;
   Eina_List *l;
   e_devicemgr_inputgen_client_data *data;

   EINA_LIST_FOREACH(input_devmgr_data->inputgen.kbd.clients, l, data)
     {
        if (data->client == client)
          {
             data->ref++;
             return TIZEN_INPUT_DEVICE_MANAGER_ERROR_NONE;
          }
     }

   data = NULL;
   data = E_NEW(e_devicemgr_inputgen_client_data, 1);
   EINA_SAFETY_ON_NULL_RETURN_VAL(data, TIZEN_INPUT_DEVICE_MANAGER_ERROR_NO_SYSTEM_RESOURCES);

   data->ref = 1;
   data->client = client;

   input_devmgr_data->inputgen.kbd.clients =
      eina_list_append(input_devmgr_data->inputgen.kbd.clients, data);

   if (input_devmgr_data->inputgen.kbd.uinp_fd > 0)
     return TIZEN_INPUT_DEVICE_MANAGER_ERROR_NONE;

   uinp_fd = open("/dev/uinput", O_WRONLY | O_NDELAY);
   if ( uinp_fd < 0)
     {
        DMWRN("Failed to open /dev/uinput: (%d)\n", uinp_fd);
        return EINA_FALSE;
     }

   ioctl(uinp_fd, UI_SET_EVBIT, EV_KEY);
   ioctl(uinp_fd, UI_SET_EVBIT, EV_SYN);
   ioctl(uinp_fd, UI_SET_EVBIT, EV_MSC);

   ioctl(uinp_fd, UI_SET_MSCBIT, MSC_SCAN);
   ioctl(uinp_fd, UI_SET_KEYBIT, KEY_ESC);

   ret = write(uinp_fd, uinp, sizeof(struct uinput_user_dev));
   if (ret < 0)
     {
        DMWRN("mouse Failed to write UINPUT device\n");
        close(uinp_fd);
        return TIZEN_INPUT_DEVICE_MANAGER_ERROR_NO_SYSTEM_RESOURCES;
     }
   if (ioctl(uinp_fd, UI_DEV_CREATE))
     {
        DMWRN("mouse Unable to create UINPUT device\n");
        close(uinp_fd);
        return TIZEN_INPUT_DEVICE_MANAGER_ERROR_NO_SYSTEM_RESOURCES;
     }

   input_devmgr_data->inputgen.kbd.uinp_fd = uinp_fd;

   return TIZEN_INPUT_DEVICE_MANAGER_ERROR_NONE;
}

static Eina_Bool
_e_input_devmgr_create_mouse_device(struct wl_client *client)
{
   int uinp_fd = -1, ret = -1;
   struct uinput_user_dev *uinp = &input_devmgr_data->inputgen.uinp;
   Eina_List *l;
   e_devicemgr_inputgen_client_data *data;

   EINA_LIST_FOREACH(input_devmgr_data->inputgen.ptr.clients, l, data)
     {
        if (data->client == client)
          {
             data->ref++;
             return TIZEN_INPUT_DEVICE_MANAGER_ERROR_NONE;
          }
     }

   data = NULL;
   data = E_NEW(e_devicemgr_inputgen_client_data, 1);
   EINA_SAFETY_ON_NULL_RETURN_VAL(data, TIZEN_INPUT_DEVICE_MANAGER_ERROR_NO_SYSTEM_RESOURCES);

   data->ref = 1;
   data->client = client;

   input_devmgr_data->inputgen.ptr.clients =
      eina_list_append(input_devmgr_data->inputgen.ptr.clients, data);

   if (input_devmgr_data->inputgen.ptr.uinp_fd > 0)
     return TIZEN_INPUT_DEVICE_MANAGER_ERROR_NONE;

   uinp_fd = open("/dev/uinput", O_WRONLY | O_NDELAY);
   if ( uinp_fd < 0)
     {
        DMWRN("Failed to open /dev/uinput: (%d)\n", uinp_fd);
        return TIZEN_INPUT_DEVICE_MANAGER_ERROR_NO_SYSTEM_RESOURCES;
     }

   ioctl(uinp_fd, UI_SET_EVBIT, EV_KEY);
   ioctl(uinp_fd, UI_SET_EVBIT, EV_SYN);
   ioctl(uinp_fd, UI_SET_EVBIT, EV_MSC);
   ioctl(uinp_fd, UI_SET_EVBIT, EV_REL);

   ioctl(uinp_fd, UI_SET_MSCBIT, MSC_SCAN);

   ioctl(uinp_fd, UI_SET_KEYBIT, BTN_LEFT);
   ioctl(uinp_fd, UI_SET_RELBIT, BTN_RIGHT);
   ioctl(uinp_fd, UI_SET_RELBIT, BTN_MIDDLE);
   ioctl(uinp_fd, UI_SET_RELBIT, REL_X);
   ioctl(uinp_fd, UI_SET_RELBIT, REL_Y);
   ioctl(uinp_fd, UI_SET_RELBIT, REL_WHEEL);

   ret = write(uinp_fd, uinp, sizeof(struct uinput_user_dev));
   if (ret < 0)
     {
        DMWRN("mouse Failed to write UINPUT device\n");
        close(uinp_fd);
        return TIZEN_INPUT_DEVICE_MANAGER_ERROR_NO_SYSTEM_RESOURCES;
     }
   if (ioctl(uinp_fd, UI_DEV_CREATE))
     {
        DMWRN("mouse Unable to create UINPUT device\n");
        close(uinp_fd);
        return TIZEN_INPUT_DEVICE_MANAGER_ERROR_NO_SYSTEM_RESOURCES;
     }

   input_devmgr_data->inputgen.ptr.uinp_fd = uinp_fd;

   return TIZEN_INPUT_DEVICE_MANAGER_ERROR_NONE;
}

static Eina_Bool
_e_input_devmgr_create_touch_device(struct wl_client *client)
{
   int uinp_fd = -1, ret = -1;
   struct uinput_user_dev *uinp = &input_devmgr_data->inputgen.uinp;
   Eina_List *l;
   e_devicemgr_inputgen_client_data *data;

   EINA_LIST_FOREACH(input_devmgr_data->inputgen.touch.clients, l, data)
     {
        if (data->client == client)
          {
             data->ref++;
             return TIZEN_INPUT_DEVICE_MANAGER_ERROR_NONE;
          }
     }

   data = NULL;
   data = E_NEW(e_devicemgr_inputgen_client_data, 1);
   EINA_SAFETY_ON_NULL_RETURN_VAL(data, TIZEN_INPUT_DEVICE_MANAGER_ERROR_NO_SYSTEM_RESOURCES);

   data->ref = 1;
   data->client = client;

   input_devmgr_data->inputgen.touch.clients =
      eina_list_append(input_devmgr_data->inputgen.touch.clients, data);

   if (input_devmgr_data->inputgen.touch.uinp_fd > 0)
     return TIZEN_INPUT_DEVICE_MANAGER_ERROR_NONE;

   uinp_fd = open("/dev/uinput", O_WRONLY | O_NDELAY);
   if ( uinp_fd < 0)
     {
        DMWRN("Failed to open /dev/uinput: (%d)\n", uinp_fd);
        return TIZEN_INPUT_DEVICE_MANAGER_ERROR_NO_SYSTEM_RESOURCES;
     }

   ioctl(uinp_fd, UI_SET_EVBIT, EV_KEY);
   ioctl(uinp_fd, UI_SET_EVBIT, EV_SYN);
   ioctl(uinp_fd, UI_SET_EVBIT, EV_MSC);
   ioctl(uinp_fd, UI_SET_EVBIT, EV_ABS);

   ioctl(uinp_fd, UI_SET_KEYBIT, BTN_TOUCH);
   ioctl(uinp_fd, UI_SET_ABSBIT, ABS_X);
   ioctl(uinp_fd, UI_SET_ABSBIT, ABS_Y);
   ioctl(uinp_fd, UI_SET_ABSBIT, ABS_MT_SLOT);
   ioctl(uinp_fd, UI_SET_ABSBIT, ABS_MT_TOUCH_MAJOR);
   ioctl(uinp_fd, UI_SET_ABSBIT, ABS_MT_TOUCH_MINOR);
   ioctl(uinp_fd, UI_SET_ABSBIT, ABS_MT_WIDTH_MAJOR);
   ioctl(uinp_fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
   ioctl(uinp_fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
   ioctl(uinp_fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);

   ioctl(uinp_fd, UI_SET_MSCBIT, MSC_SCAN);

   ret = write(uinp_fd, uinp, sizeof(struct uinput_user_dev));
   if (ret < 0)
     {
        DMWRN("mouse Failed to write UINPUT device\n");
        close(uinp_fd);
        return TIZEN_INPUT_DEVICE_MANAGER_ERROR_NO_SYSTEM_RESOURCES;
     }
   else
     {
        ret = TIZEN_INPUT_DEVICE_MANAGER_ERROR_NONE;
     }
   if (ioctl(uinp_fd, UI_DEV_CREATE))
     {
        DMWRN("mouse Unable to create UINPUT device\n");
        close(uinp_fd);
        return TIZEN_INPUT_DEVICE_MANAGER_ERROR_NO_SYSTEM_RESOURCES;
     }

   input_devmgr_data->inputgen.touch.uinp_fd = uinp_fd;

   return TIZEN_INPUT_DEVICE_MANAGER_ERROR_NONE;
}


static void
_e_input_devmgr_cb_init_generator(struct wl_client *client, struct wl_resource *resource, uint32_t clas)
{
   struct uinput_user_dev *uinp = &input_devmgr_data->inputgen.uinp;
   int ret = TIZEN_INPUT_DEVICE_MANAGER_ERROR_NONE;

#ifdef ENABLE_CYNARA
   if (EINA_FALSE == _e_devicemgr_util_do_privilege_check(client, wl_client_get_fd(client), "http://tizen.org/privilege/inputgenerator"))
     {
        DMERR("_e_input_devmgr_cb_init_generator:priv check failed");
        ret = TIZEN_INPUT_DEVICE_MANAGER_ERROR_NO_PERMISSION;
        goto finish;
     }
#endif

   if (!uinp->id.version)
     {
        strncpy(uinp->name, "Input Generator", UINPUT_MAX_NAME_SIZE);
        uinp->id.version = 4;
        uinp->id.bustype = BUS_VIRTUAL;
     }

   if (clas & TIZEN_INPUT_DEVICE_MANAGER_CLAS_KEYBOARD)
     ret = _e_input_devmgr_create_keyboard_device(client);
   if (clas & TIZEN_INPUT_DEVICE_MANAGER_CLAS_MOUSE)
     ret = _e_input_devmgr_create_mouse_device(client);
   if (clas & TIZEN_INPUT_DEVICE_MANAGER_CLAS_TOUCHSCREEN)
     ret = _e_input_devmgr_create_touch_device(client);

   if (ret == TIZEN_INPUT_DEVICE_MANAGER_ERROR_NONE)
     _e_input_devmgr_inputgen_client_add(client, clas);

finish:
   tizen_input_device_manager_send_error(resource, ret);
}

static void
_e_input_devmgr_cb_deinit_generator(struct wl_client *client, struct wl_resource *resource, uint32_t clas)
{
   int ret = -1;
   Eina_List *l, *l_next;
   e_devicemgr_inputgen_client_global_data *data;
   struct wl_listener *listener;

#ifdef ENABLE_CYNARA
   if (EINA_FALSE == _e_devicemgr_util_do_privilege_check(client, wl_client_get_fd(client), "http://tizen.org/privilege/inputgenerator"))
     {
        DMERR("_e_input_devmgr_cb_deinit_generator:priv check failed");
        ret = TIZEN_INPUT_DEVICE_MANAGER_ERROR_NO_PERMISSION;
        goto finish;
     }
#endif

   if (clas & TIZEN_INPUT_DEVICE_MANAGER_CLAS_KEYBOARD)
     _e_input_devmgr_inputgen_client_remove(client, TIZEN_INPUT_DEVICE_MANAGER_CLAS_KEYBOARD);
   if (clas & TIZEN_INPUT_DEVICE_MANAGER_CLAS_MOUSE)
     _e_input_devmgr_inputgen_client_remove(client, TIZEN_INPUT_DEVICE_MANAGER_CLAS_MOUSE);
   if (clas & TIZEN_INPUT_DEVICE_MANAGER_CLAS_TOUCHSCREEN)
     _e_input_devmgr_inputgen_client_remove(client, TIZEN_INPUT_DEVICE_MANAGER_CLAS_TOUCHSCREEN);

   EINA_LIST_FOREACH_SAFE(input_devmgr_data->watched_clients, l, l_next, data)
     {
        if (data->client == client && !data->clas)
          {
             listener = wl_client_get_destroy_listener(client,
                        _e_input_devmgr_inputgen_client_cb_destroy);
             wl_list_remove(&listener->link);
             E_FREE(listener);

             input_devmgr_data->watched_clients =
                eina_list_remove_list(input_devmgr_data->watched_clients, l);
             E_FREE(data);

             break;
          }
     }

   ret = TIZEN_INPUT_DEVICE_MANAGER_ERROR_NONE;

finish:
   tizen_input_device_manager_send_error(resource, ret);
}

static void
_e_input_devmgr_keyevent_free(void *data EINA_UNUSED, void *ev)
{
   Ecore_Event_Key *e = ev;

   eina_stringshare_del(e->keyname);
   eina_stringshare_del(e->key);
   eina_stringshare_del(e->compose);

   free(e);
}

static int
_e_input_devmgr_generate_key_event(const char *key, Eina_Bool pressed)
{
   Ecore_Event_Key *e;
   unsigned int keycode;

   EINA_SAFETY_ON_NULL_RETURN_VAL(key, TIZEN_INPUT_DEVICE_MANAGER_ERROR_INVALID_PARAMETER);

   e = calloc(1, sizeof(Ecore_Event_Key));
   if (!e) return TIZEN_INPUT_DEVICE_MANAGER_ERROR_NO_SYSTEM_RESOURCES;

   keycode = _e_input_devmgr_keycode_from_string(key);
   if (keycode <= 0) goto finish;

   e->keyname = eina_stringshare_add(key);
   e->key = eina_stringshare_add(key);
   e->compose = eina_stringshare_add(key);
   e->string = e->compose;

   e->window = e_comp->ee_win;
   e->event_window = e_comp->ee_win;
   e->root_window = e_comp->ee_win;
   e->timestamp = (int)(ecore_time_get() * 1000);
   e->same_screen = 1;
   e->keycode = keycode;
   e->data = NULL;

   e->modifiers = 0;
   e->dev = ecore_drm_evdev_get_ecore_device(input_devmgr_data->inputgen.kbd.uinp_identifier, ECORE_DEVICE_CLASS_KEYBOARD);

   DMDBG("Generate key event: key: %s, keycode: %d, iden: %s\n", e->key, e->keycode, input_devmgr_data->inputgen.kbd.uinp_identifier);

   if (pressed)
     ecore_event_add(ECORE_EVENT_KEY_DOWN, e, _e_input_devmgr_keyevent_free, NULL);
   else
     ecore_event_add(ECORE_EVENT_KEY_UP, e, _e_input_devmgr_keyevent_free, NULL);

   return TIZEN_INPUT_DEVICE_MANAGER_ERROR_NONE;

finish:
    if(e)
        free(e);
    return TIZEN_INPUT_DEVICE_MANAGER_ERROR_INVALID_PARAMETER;
}

static void
_e_input_devmgr_cb_generate_key(struct wl_client *client, struct wl_resource *resource,
                                const char *keyname, uint32_t pressed)
{
   int ret = -1;

#ifdef ENABLE_CYNARA
   if (EINA_FALSE == _e_devicemgr_util_do_privilege_check(client, wl_client_get_fd(client), "http://tizen.org/privilege/inputgenerator"))
     {
        DMERR("_e_input_devmgr_cb_generate_key:priv check failed");
        ret = TIZEN_INPUT_DEVICE_MANAGER_ERROR_NO_PERMISSION;
        goto finish;
     }
#endif

   if (input_devmgr_data->inputgen.kbd.uinp_fd < 0)
     {
        DMWRN("generate is not init\n");
        ret = TIZEN_INPUT_DEVICE_MANAGER_ERROR_INVALID_PARAMETER;
        goto finish;
     }
   if (!e_comp_wl->xkb.keymap)
     {
        DMWRN("keymap is not ready\n");
        ret = TIZEN_INPUT_DEVICE_MANAGER_ERROR_INVALID_PARAMETER;
        goto finish;
     }

   ret = _e_input_devmgr_generate_key_event(keyname, (Eina_Bool)!!pressed);

finish:
   tizen_input_device_manager_send_error(resource, ret);
}

static void
_e_input_devmgr_mouse_button_event_free(void *data EINA_UNUSED, void *ev)
{
   Ecore_Event_Mouse_Button *e = ev;

   free(e);
}

static int
_e_input_devmgr_generate_pointer_event(Eina_Bool state, int x, int y, int buttons)
{
   Ecore_Event_Mouse_Button *e;

   e = calloc(1, sizeof(Ecore_Event_Mouse_Button));
   if (!e) return TIZEN_INPUT_DEVICE_MANAGER_ERROR_NO_SYSTEM_RESOURCES;

   e->window = e_comp->win;
   e->event_window = e->window;
   e->root_window = e_comp->root;
   e->timestamp = (int)(ecore_time_get() * 1000);
   e->same_screen = 1;

   e->x = x;
   e->y = y;
   e->root.x = e->x;
   e->root.y = e->y;

   e->multi.device = 0;
   e->multi.radius = 1;
   e->multi.radius_x = 1;
   e->multi.radius_y = 1;
   e->multi.pressure = 1.0;
   e->multi.angle = 0.0;

   e->multi.x = e->x;
   e->multi.y = e->y;
   e->multi.root.x = e->x;
   e->multi.root.y = e->y;
   e->dev = ecore_drm_evdev_get_ecore_device(input_devmgr_data->inputgen.ptr.uinp_identifier, ECORE_DEVICE_CLASS_MOUSE);
   e->buttons = buttons;

   DMDBG("Generate mouse button event: button: %d (state: %d)\n", buttons, state);

   if (state)
     ecore_event_add(ECORE_EVENT_MOUSE_BUTTON_DOWN, e, _e_input_devmgr_mouse_button_event_free, NULL);
   else
     ecore_event_add(ECORE_EVENT_MOUSE_BUTTON_UP, e, _e_input_devmgr_mouse_button_event_free, NULL);

   return TIZEN_INPUT_DEVICE_MANAGER_ERROR_NONE;
}

static void
_e_input_devmgr_mouse_move_event_free(void *data EINA_UNUSED, void *ev)
{
   Ecore_Event_Mouse_Move *e = ev;

   free(e);
}

static int
_e_input_devmgr_generate_pointer_move_event(int x, int y)
{
   Ecore_Event_Mouse_Move *e;

   DMERR("Try\n");

   e = calloc(1, sizeof(Ecore_Event_Mouse_Button));
   if (!e) return TIZEN_INPUT_DEVICE_MANAGER_ERROR_NO_SYSTEM_RESOURCES;

   e->window = e_comp->win;
   e->event_window = e->window;
   e->root_window = e_comp->root;
   e->timestamp = (int)(ecore_time_get() * 1000);
   e->same_screen = 1;

   e->x = x;
   e->y = y;
   e->root.x = e->x;
   e->root.y = e->y;

   e->multi.device = 0;
   e->multi.radius = 1;
   e->multi.radius_x = 1;
   e->multi.radius_y = 1;
   e->multi.pressure = 1.0;
   e->multi.angle = 0.0;

   e->multi.x = e->x;
   e->multi.y = e->y;
   e->multi.root.x = e->x;
   e->multi.root.y = e->y;
   e->dev = ecore_drm_evdev_get_ecore_device(input_devmgr_data->inputgen.ptr.uinp_identifier, ECORE_DEVICE_CLASS_MOUSE);

   DMDBG("Generate mouse move event: (%d, %d)\n", e->x, e->y);

   ecore_event_add(ECORE_EVENT_MOUSE_MOVE, e, _e_input_devmgr_mouse_move_event_free, NULL);

   return TIZEN_INPUT_DEVICE_MANAGER_ERROR_NONE;
}

static void
_e_input_devmgr_cb_generate_pointer(struct wl_client *client, struct wl_resource *resource,
                                    uint32_t type, uint32_t x, uint32_t y, uint32_t button)
{
   int ret = TIZEN_INPUT_DEVICE_MANAGER_ERROR_NONE;
   Eina_Bool state;
#ifdef ENABLE_CYNARA
   if (EINA_FALSE == _e_devicemgr_util_do_privilege_check(client, wl_client_get_fd(client), "http://tizen.org/privilege/inputgenerator"))
     {
        DMERR("_e_input_devmgr_cb_generate_pointer:priv check failed");
        ret = TIZEN_INPUT_DEVICE_MANAGER_ERROR_NO_PERMISSION;
        goto finish;
     }
#endif
   if (input_devmgr_data->inputgen.ptr.uinp_fd < 0)
     {
        DMWRN("generate is not init\n");
        ret = TIZEN_INPUT_DEVICE_MANAGER_ERROR_INVALID_PARAMETER;
        goto finish;
     }

   if (type == TIZEN_INPUT_DEVICE_MANAGER_POINTER_EVENT_TYPE_UPDATE)
      ret = _e_input_devmgr_generate_pointer_move_event(x, y);
   else
     {
        state = (type == TIZEN_INPUT_DEVICE_MANAGER_POINTER_EVENT_TYPE_BEGIN) ?
                EINA_TRUE : EINA_FALSE;
        ret = _e_input_devmgr_generate_pointer_event(state, x, y, button);
     }

   DMDBG("generate pointer is requested from %p client. type: %d, coord(%d, %d), button: %d\n", client, type, x, y, button);
finish:
   tizen_input_device_manager_send_error(resource, ret);
}

static int
_e_input_devmgr_generate_touch_event(uint32_t type, uint32_t x, uint32_t y, uint32_t finger)
{
   Ecore_Event_Mouse_Button *e;

   e = calloc(1, sizeof(Ecore_Event_Mouse_Button));
   if (!e) return TIZEN_INPUT_DEVICE_MANAGER_ERROR_NO_SYSTEM_RESOURCES;

   e->window = e_comp->win;
   e->event_window = e->window;
   e->root_window = e_comp->root;
   e->timestamp = (int)(ecore_time_get() * 1000);
   e->same_screen = 1;

   e->x = x;
   e->y = y;
   e->root.x = e->x;
   e->root.y = e->y;

   e->multi.device = finger;
   e->multi.radius = 1;
   e->multi.radius_x = 1;
   e->multi.radius_y = 1;
   e->multi.pressure = 1.0;
   e->multi.angle = 0.0;

   e->multi.x = e->x;
   e->multi.y = e->y;
   e->multi.root.x = e->x;
   e->multi.root.y = e->y;
   e->dev = ecore_drm_evdev_get_ecore_device(input_devmgr_data->inputgen.touch.uinp_identifier, ECORE_DEVICE_CLASS_TOUCH);
   e->buttons = 1;

   DMDBG("Generate touch event: device: %d (%d, %d)\n", e->multi.device, e->x, e->y);

   if (type == TIZEN_INPUT_DEVICE_MANAGER_POINTER_EVENT_TYPE_BEGIN)
     ecore_event_add(ECORE_EVENT_MOUSE_BUTTON_DOWN, e, _e_input_devmgr_mouse_button_event_free, NULL);
   else
     ecore_event_add(ECORE_EVENT_MOUSE_BUTTON_UP, e, _e_input_devmgr_mouse_button_event_free, NULL);

   return TIZEN_INPUT_DEVICE_MANAGER_ERROR_NONE;
}

static int
_e_input_devmgr_generate_touch_update_event(uint32_t x, uint32_t y, uint32_t finger)
{
   Ecore_Event_Mouse_Move *e;

   e = calloc(1, sizeof(Ecore_Event_Mouse_Button));
   if (!e) return TIZEN_INPUT_DEVICE_MANAGER_ERROR_NO_SYSTEM_RESOURCES;

   e->window = e_comp->win;
   e->event_window = e->window;
   e->root_window = e_comp->root;
   e->timestamp = (int)(ecore_time_get() * 1000);
   e->same_screen = 1;

   e->x = x;
   e->y = y;
   e->root.x = e->x;
   e->root.y = e->y;

   e->multi.device = finger;
   e->multi.radius = 1;
   e->multi.radius_x = 1;
   e->multi.radius_y = 1;
   e->multi.pressure = 1.0;
   e->multi.angle = 0.0;

   e->multi.x = e->x;
   e->multi.y = e->y;
   e->multi.root.x = e->x;
   e->multi.root.y = e->y;
   e->dev = ecore_drm_evdev_get_ecore_device(input_devmgr_data->inputgen.touch.uinp_identifier, ECORE_DEVICE_CLASS_TOUCH);

   DMDBG("Generate touch move event: device: %d (%d, %d)\n", e->multi.device, e->x, e->y);

   ecore_event_add(ECORE_EVENT_MOUSE_MOVE, e, _e_input_devmgr_mouse_move_event_free, NULL);

   return TIZEN_INPUT_DEVICE_MANAGER_ERROR_NONE;
}

static void
_e_input_devmgr_cb_generate_touch(struct wl_client *client, struct wl_resource *resource,
                                   uint32_t type, uint32_t x, uint32_t y, uint32_t finger)
{
   int ret = TIZEN_INPUT_DEVICE_MANAGER_ERROR_NONE;

#ifdef ENABLE_CYNARA
   if (EINA_FALSE == _e_devicemgr_util_do_privilege_check(client, wl_client_get_fd(client), "http://tizen.org/privilege/inputgenerator"))
     {
        DMERR("_e_input_devmgr_cb_generate_touch:priv check failed");
        ret = TIZEN_INPUT_DEVICE_MANAGER_ERROR_NO_PERMISSION;
        goto finish;
     }
#endif

   if (input_devmgr_data->inputgen.touch.uinp_fd < 0)
     {
        DMWRN("generate is not init\n");
        ret = TIZEN_INPUT_DEVICE_MANAGER_ERROR_INVALID_PARAMETER;
        goto finish;
     }

   switch(type)
     {
        case TIZEN_INPUT_DEVICE_MANAGER_POINTER_EVENT_TYPE_BEGIN:
           ret = _e_input_devmgr_generate_touch_update_event(x, y, finger);
           ret = _e_input_devmgr_generate_touch_event(type, x, y, finger);
           break;
        case TIZEN_INPUT_DEVICE_MANAGER_POINTER_EVENT_TYPE_END:
           ret = _e_input_devmgr_generate_touch_event(type, x, y, finger);
           break;
        case TIZEN_INPUT_DEVICE_MANAGER_POINTER_EVENT_TYPE_UPDATE:
           ret = _e_input_devmgr_generate_touch_update_event(x, y, finger);
           break;
     }
finish:
   tizen_input_device_manager_send_error(resource, ret);
}

/* being edited */
static  int
_e_devicemgr_pointer_warp(int x, int y)
{
   ecore_evas_pointer_warp(e_comp->ee, x, y);
   DMDBG("The pointer warped to (%d, %d) !\n", x, y);

   return TIZEN_INPUT_DEVICE_MANAGER_ERROR_NONE;
}

static void
_e_input_devmgr_cb_pointer_warp(struct wl_client *client, struct wl_resource *resource, struct wl_resource *surface, wl_fixed_t x, wl_fixed_t y)
{
   E_Client *ec = NULL;
   int ret;
   int new_x, new_y;

   if (!(ec = wl_resource_get_user_data(surface)) || !ec->visible)
     {
        DMDBG("The given surface is invalid or invisible !\n");
        tizen_input_device_manager_send_error(resource, TIZEN_INPUT_DEVICE_MANAGER_ERROR_INVALID_SURFACE);
        return;
     }

   if (ec != e_comp_wl->ptr.ec)
     {
        DMDBG("Pointer is not on the given surface  !\n");
        tizen_input_device_manager_send_error(resource, TIZEN_INPUT_DEVICE_MANAGER_ERROR_INVALID_SURFACE);
        return;
     }

   if (e_pointer_is_hidden(e_comp->pointer))
     {
        DMDBG("Pointer is hidden");
        tizen_input_device_manager_send_error(resource, TIZEN_INPUT_DEVICE_MANAGER_ERROR_NO_POINTER_AVAILABLE);
        return;
     }

   new_x = wl_fixed_to_int(x);
   new_y = wl_fixed_to_int(y);
   if (e_client_transform_core_enable_get(ec))
     e_client_transform_core_input_inv_transform(ec, wl_fixed_to_int(x), wl_fixed_to_int(y), &new_x, &new_y);

   ret = _e_devicemgr_pointer_warp(ec->client.x + new_x, ec->client.y + new_y);
   tizen_input_device_manager_send_error(resource, ret);
}

static const struct tizen_input_device_manager_interface _e_input_devmgr_implementation = {
   _e_input_devmgr_cb_block_events,
   _e_input_devmgr_cb_unblock_events,
   _e_input_devmgr_cb_init_generator,
   _e_input_devmgr_cb_deinit_generator,
   _e_input_devmgr_cb_generate_key,
   _e_input_devmgr_cb_generate_pointer,
   _e_input_devmgr_cb_generate_touch,
   _e_input_devmgr_cb_pointer_warp,
};

static void
_e_devicemgr_device_mgr_cb_unbind(struct wl_resource *resource)
{
   if(!e_comp_wl) return;

   e_comp_wl->input_device_manager.resources = eina_list_remove(e_comp_wl->input_device_manager.resources, resource);
}

static void
_e_devicemgr_device_mgr_cb_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
   struct wl_resource *res, *seat_res, *device_res;
   Eina_List *l;
   uint32_t serial;
   E_Comp_Wl_Input_Device *dev;
   struct wl_array axes;
   e_devicemgr_input_device_user_data *device_user_data;

   if (!e_comp_wl) return;
   if (!e_comp_wl->wl.disp) return;

   if (!(res = wl_resource_create(client, &tizen_input_device_manager_interface, MIN(version, 1), id)))
     {
        DMERR("Could not create tizen_devices_interface resource: %m");
        wl_client_post_no_memory(client);
        return;
     }

   e_comp_wl->input_device_manager.resources = eina_list_append(e_comp_wl->input_device_manager.resources, res);

   wl_resource_set_implementation(res, &_e_input_devmgr_implementation, NULL,
                                 _e_devicemgr_device_mgr_cb_unbind);

   EINA_LIST_FOREACH(e_comp_wl->seat.resources, l, seat_res)
     {
        if (wl_resource_get_client(seat_res) != client) continue;

        wl_array_init(&axes);
        serial = wl_display_next_serial(e_comp_wl->wl.disp);

        EINA_LIST_FOREACH(e_comp_wl->input_device_manager.device_list, l, dev)
          {
             device_res = wl_resource_create(client, &tizen_input_device_interface, 1, 0);
             if (!device_res)
               {
                  DMERR("Could not create tizen_input_device resource: %m");
                  return;
               }
             device_user_data = E_NEW(e_devicemgr_input_device_user_data, 1);
             if (!device_user_data)
               {
                  DMERR("Failed to allocate memory for input device user data\n");
                  return;
               }
             device_user_data->dev = dev;
             device_user_data->dev_mgr_res = res;
             device_user_data->seat_res = seat_res;

             dev->resources = eina_list_append(dev->resources, device_res);

             wl_resource_set_implementation(device_res, &_e_devicemgr_device_interface, device_user_data,
                                            _e_devicemgr_device_cb_device_unbind);

             tizen_input_device_manager_send_device_add(res, serial, dev->identifier, device_res, seat_res);
             tizen_input_device_send_device_info(device_res, dev->name, dev->clas, TIZEN_INPUT_DEVICE_SUBCLAS_NONE, &axes);
          }
     }
}

int
e_devicemgr_device_init(void)
{
   int ret;

   if (!e_comp) return 0;
   if (!e_comp_wl) return 0;
   if (!e_comp_wl->wl.disp) return 0;

   TRACE_INPUT_BEGIN(e_devicemgr_device_init);

   /* try to add tizen_input_device_manager to wayland globals */
   e_comp_wl->input_device_manager.global = wl_global_create(e_comp_wl->wl.disp, &tizen_input_device_manager_interface, 1,
                         NULL, _e_devicemgr_device_mgr_cb_bind);
   if (!e_comp_wl->input_device_manager.global)
     {
        DMERR("Could not add tizen_input_device_manager to wayland globals");
        TRACE_INPUT_END();
        return 0;
     }
   e_comp_wl->input_device_manager.resources = NULL;
   e_comp_wl->input_device_manager.device_list = NULL;

   E_LIST_HANDLER_APPEND(handlers, ECORE_EVENT_DEVICE_ADD, _cb_device_add, NULL);
   E_LIST_HANDLER_APPEND(handlers, ECORE_EVENT_DEVICE_DEL, _cb_device_del, NULL);

   input_devmgr_data = E_NEW(e_devicemgr_input_devmgr_data, 1);
   EINA_SAFETY_ON_NULL_RETURN_VAL(input_devmgr_data, 0);

   input_devmgr_data->block_devtype = 0x0;

   /* initialization of cynara for checking privilege */
#ifdef ENABLE_CYNARA
   ret = cynara_initialize(&input_devmgr_data->p_cynara, NULL);
   if (EINA_UNLIKELY(CYNARA_API_SUCCESS != ret))
     {
        _e_devicemgr_util_cynara_log("cynara_initialize", ret);
        input_devmgr_data->p_cynara = NULL;
     }
   input_devmgr_data->cynara_initialized = EINA_TRUE;
#endif

   input_devmgr_data->inputgen.kbd.uinp_fd = -1;
   input_devmgr_data->inputgen.ptr.uinp_fd = -1;
   input_devmgr_data->inputgen.touch.uinp_fd = -1;

   TRACE_INPUT_END();
   return 1;
}

void
e_devicemgr_device_fini(void)
{
   E_Comp_Wl_Input_Device *dev;
   struct wl_resource *res;
   e_devicemgr_input_device_user_data *device_user_data;
   struct wl_listener *destroy_listener;
   Eina_List *l, *l_next;
   e_devicemgr_inputgen_client_global_data *global_data;

   /* destroy the global seat resource */
   if (e_comp_wl->input_device_manager.global)
     wl_global_destroy(e_comp_wl->input_device_manager.global);
   e_comp_wl->input_device_manager.global = NULL;

   EINA_LIST_FREE(e_comp_wl->input_device_manager.device_list, dev)
     {
        if (dev->name) eina_stringshare_del(dev->name);
        if (dev->identifier) eina_stringshare_del(dev->identifier);
        EINA_LIST_FREE(dev->resources, res)
          {
             device_user_data = wl_resource_get_user_data(res);
             if (device_user_data)
               {
                  device_user_data->dev = NULL;
                  device_user_data->dev_mgr_res = NULL;
                  device_user_data->seat_res = NULL;
                  E_FREE(device_user_data);
               }

             wl_resource_set_user_data(res, NULL);
          }

        free(dev);
     }

   E_FREE_LIST(handlers, ecore_event_handler_del);

   /* deinitialization of cynara if it has been initialized */
#ifdef ENABLE_CYNARA
   if (input_devmgr_data->p_cynara) cynara_finish(input_devmgr_data->p_cynara);
   input_devmgr_data->cynara_initialized = EINA_FALSE;
#endif

   if (input_devmgr_data->block_client)
     {
        destroy_listener = wl_client_get_destroy_listener(input_devmgr_data->block_client,
                                                          _e_input_devmgr_client_cb_destroy);
        if (destroy_listener)
          {
             wl_list_remove(&destroy_listener->link);
             E_FREE(destroy_listener);
          }
        input_devmgr_data->block_client = NULL;
     }

   E_FREE_LIST(input_devmgr_data->inputgen.kbd.clients, free);
   E_FREE_LIST(input_devmgr_data->inputgen.ptr.clients, free);
   E_FREE_LIST(input_devmgr_data->inputgen.touch.clients, free);

   EINA_LIST_FOREACH_SAFE(input_devmgr_data->watched_clients, l, l_next, global_data)
     {
        destroy_listener = wl_client_get_destroy_listener(global_data->client,
                                                          _e_input_devmgr_inputgen_client_cb_destroy);
        if (destroy_listener)
          {
             wl_list_remove(&destroy_listener->link);
             E_FREE(destroy_listener);
          }
        input_devmgr_data->watched_clients =
           eina_list_remove_list(input_devmgr_data->watched_clients, l);
     }

   E_FREE_LIST(input_devmgr_data->pressed_keys, free);

   eina_stringshare_del(input_devmgr_data->detent.identifier);
   eina_stringshare_del(input_devmgr_data->inputgen.kbd.uinp_identifier);
   eina_stringshare_del(input_devmgr_data->inputgen.ptr.uinp_identifier);
   eina_stringshare_del(input_devmgr_data->inputgen.touch.uinp_identifier);
}
