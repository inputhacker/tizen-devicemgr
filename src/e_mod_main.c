#include "e.h"
#include "eina_log.h"
#include "e_mod_main.h"
#include "e_devicemgr_input.h"
#ifdef HAVE_WAYLAND_ONLY
#include "e_devicemgr_embedded_compositor.h"
#include "e_devicemgr_device.h"
#endif
#include "e_devicemgr_privates.h"

int _log_dom = -1;

/* this is needed to advertise a label for the module IN the code (not just
 * the .desktop file) but more specifically the api version it was compiled
 * for so E can skip modules that are compiled for an incorrect API version
 * safely) */
EAPI E_Module_Api e_modapi =
{
   E_MODULE_API_VERSION,
   "DeviceMgr Module of Window Manager"
};

E_Devicemgr_Config_Data *dconfig;

EAPI void *
e_modapi_init(E_Module *m)
{
   if (!eina_init())
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed @ eina_init()..!\n", __FUNCTION__);
        return NULL;
     }

   _log_dom = eina_log_domain_register("e-devicemgr", EINA_COLOR_BLUE);
   if (_log_dom < 0)
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed @ eina_log_domain_register()..!\n", __FUNCTION__);
        return NULL;
     }

   dconfig = E_NEW(E_Devicemgr_Config_Data, 1);
   if (!dconfig)
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed @ allocate memory for config data\n", __FUNCTION__);
        return NULL;
     }
   dconfig->module = m;
   e_devicemgr_conf_init(dconfig);
   if (!dconfig->conf)
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed @ get config data\n", __FUNCTION__);
        return NULL;
     }

   if (!e_devicemgr_input_init())
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed @ e_devicemgr_input_init()..!\n", __FUNCTION__);
        return NULL;
     }

#ifdef HAVE_WAYLAND_ONLY
   if (!e_devicemgr_embedded_compositor_init())
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed @ e_devicemgr_embedded_compositor_init()..!\n", __FUNCTION__);
        return NULL;
     }

   if (!e_devicemgr_device_init())
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed @ e_devicemgr_device_init()..!\n", __FUNCTION__);
        return NULL;
     }
#endif

   return dconfig;
}

EAPI int
e_modapi_shutdown(E_Module *m)
{
   E_Devicemgr_Config_Data *dconf = m->data;
#ifdef HAVE_WAYLAND_ONLY
   e_devicemgr_embedded_compositor_fini();
   e_devicemgr_device_fini();
#endif
   e_devicemgr_input_fini();
   e_devicemgr_conf_fini(dconf);
   E_FREE(dconf);

   eina_log_domain_unregister(_log_dom);
   eina_shutdown();

   return 1;
}

EAPI int
e_modapi_save(E_Module *m)
{
   /* Do Something */
   E_Devicemgr_Config_Data *dconf = m->data;
   e_config_domain_save("module.devicemgr",
                        dconf->conf_edd,
                        dconf->conf);
   return 1;
}
