#ifndef __E_DEVICEMGR_DEVICE_H__
#define __E_DEVICEMGR_DEVICE_H__

#define E_COMP_WL
#include "e.h"
#include "e_comp_wl.h"
#include <wayland-server.h>

#include <linux/uinput.h>
#include <xkbcommon/xkbcommon.h>

#ifdef TRACE_INPUT_BEGIN
#undef TRACE_INPUT_BEGIN
#endif
#ifdef TRACE_INPUT_END
#undef TRACE_INPUT_END
#endif

#ifdef ENABLE_TTRACE
#include <ttrace.h>

#define TRACE_INPUT_BEGIN(NAME) traceBegin(TTRACE_TAG_INPUT, "INPUT:DEVMGR:"#NAME)
#define TRACE_INPUT_END() traceEnd(TTRACE_TAG_INPUT)
#else
#define TRACE_INPUT_BEGIN(NAME)
#define TRACE_INPUT_END()
#endif

#define DMERR(msg, ARG...) ERR("[%s:%d] "msg, __FUNCTION__, __LINE__, ##ARG)
#define DMWRN(msg, ARG...) WRN("[%s:%d] "msg, __FUNCTION__, __LINE__, ##ARG)
#define DMINF(msg, ARG...) INF("[%s:%d] "msg, __FUNCTION__, __LINE__, ##ARG)
#define DMDBG(msg, ARG...) DBG("[%s:%d] "msg, __FUNCTION__, __LINE__, ##ARG)

#ifdef ENABLE_CYNARA
#include <cynara-session.h>
#include <cynara-client.h>
#include <cynara-creds-socket.h>
#endif

#define INPUT_GENERATOR_DEVICE "Input Generator"

typedef struct _e_devicemgr_input_devmgr_data e_devicemgr_input_devmgr_data;
typedef struct _e_devicemgr_input_device_user_data e_devicemgr_input_device_user_data;
typedef struct _e_devicemgr_inputgen_client_data e_devicemgr_inputgen_client_data;
typedef struct _e_devicemgr_inputgen_client_global_data e_devicemgr_inputgen_client_global_data;
typedef struct _e_devicemgr_inputgen_device_data e_devicemgr_inputgen_device_data;
typedef struct _e_devicemgr_inputgen_resource_data e_devicemgr_inputgen_resource_data;

struct _e_devicemgr_input_device_user_data
{
   E_Comp_Wl_Input_Device *dev;
   struct wl_resource *dev_mgr_res;
   struct wl_resource *seat_res;
};

struct _e_devicemgr_inputgen_client_data
{
   struct wl_client *client;
   int ref;
};

struct _e_devicemgr_inputgen_client_global_data
{
   struct wl_client *client;
   unsigned int clas;
};

struct _e_devicemgr_inputgen_device_data
{
   int uinp_fd;
   char *identifier;
   char name[UINPUT_MAX_NAME_SIZE];
   Eina_List *clients;
};

struct _e_devicemgr_inputgen_resource_data
{
   struct wl_resource *resource;
   char name[UINPUT_MAX_NAME_SIZE];
};

struct _e_devicemgr_input_devmgr_data
{
   unsigned int block_devtype;
   struct wl_client *block_client;
   Ecore_Timer *duration_timer;
   Eina_List *pressed_keys;
#ifdef ENABLE_CYNARA
   cynara *p_cynara;
   Eina_Bool cynara_initialized;
#endif

   unsigned int pressed_button;
   unsigned int pressed_finger;

   struct
   {
      Eina_List *kbd_list;
      Eina_List *ptr_list;
      Eina_List *touch_list;

      Eina_List *resource_list;
   }inputgen;

   struct
   {
      char *identifier;
      int wheel_click_angle;
   } detent;

   Eina_List *watched_clients;
};

typedef enum _E_Devicemgr_Device_Type
{
   E_DEVICEMGR_DEVICE_TYPE_NONE,
   E_DEVICEMGR_DEVICE_TYPE_KEY,
   E_DEVICEMGR_DEVICE_TYPE_MOUSE,
   E_DEVICEMGR_DEVICE_TYPE_TOUCH,
} E_Devicemgr_Device_Type;

int e_devicemgr_device_init(void);
void e_devicemgr_device_fini(void);

void e_devicemgr_destroy_virtual_device(int uinp_fd);
int e_devicemgr_create_virtual_device(E_Devicemgr_Device_Type type, const char *name);

Eina_Bool e_devicemgr_block_check_keyboard(int type, void *event);
Eina_Bool e_devicemgr_block_check_pointer(int type, void *event);
Eina_Bool e_devicemgr_is_detent_device(const char *name);
Eina_Bool e_devicemgr_check_detent_device_add(int type, void *event);
Eina_Bool e_devicemgr_detent_check(int type EINA_UNUSED, void *event);

#endif
