#ifndef _E_DEVICEMGR_PRIVATE_H_
#define _E_DEVICEMGR_PRIVATE_H_

#include "e.h"
#define LOG_TAG	"DEVICEMGR"
#include "dlog.h"

extern int _log_dom;
#undef ERR
#undef DBG
#undef INF
#undef WRN
#undef CRI
#define CRI(...) EINA_LOG_DOM_CRIT(_log_dom, __VA_ARGS__)
#define ERR(...) EINA_LOG_DOM_ERR(_log_dom, __VA_ARGS__)
#define WRN(...) EINA_LOG_DOM_WARN(_log_dom, __VA_ARGS__)
#define INF(...) EINA_LOG_DOM_INFO(_log_dom, __VA_ARGS__)
#define DBG(...) EINA_LOG_DOM_DBG(_log_dom, __VA_ARGS__)

#undef SLOG
#define SLOG(priority, tag, format, arg...) printf(format, ##arg)

#ifndef CLEAR
#define CLEAR(x) memset(&(x), 0, sizeof (x))
#endif

#undef NEVER_GET_HERE
#define NEVER_GET_HERE()     CRI("** need to improve more **")

#if 0
#undef DBG
#define DBG(fmt, ARG...)      printf("@@@ [%s:%d] "fmt"\n", __FUNCTION__, __LINE__, ##ARG)
#endif

#ifdef MIN
#undef MIN
#endif
#define MIN(a,b) (((a) < (b)) ? (a) : (b))

#ifdef MAX
#undef MAX
#endif
#define MAX(a,b) (((a) < (b)) ? (b) : (a))

/* Structure for config data */
typedef struct _E_Devicemgr_Conf_Edd E_Devicemgr_Conf_Edd;
typedef struct _E_Devicemgr_Config_Data E_Devicemgr_Config_Data;

extern E_Devicemgr_Config_Data *dconfig;

struct _E_Devicemgr_Conf_Edd
{
   struct
   {
      Eina_Bool button_remap_enable;
      int back_keycode;
   } input;
   Eina_Bool eom_enable;
};

struct _E_Devicemgr_Config_Data
{
   E_Module *module;
   E_Config_DD *conf_edd;
   E_Devicemgr_Conf_Edd *conf;
};

/* Functions for config data */
void e_devicemgr_conf_init(E_Devicemgr_Config_Data *dconfig);
void e_devicemgr_conf_fini(E_Devicemgr_Config_Data *dconfig);

#endif//_E_DEVICEMGR_PRIVATE_H_
