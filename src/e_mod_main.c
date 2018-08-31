#include "e.h"
#include "dlog.h"
#include "eina_log.h"
#include "e_mod_main.h"
#include "e_devicemgr_embedded_compositor.h"

/* this is needed to advertise a label for the module IN the code (not just
 * the .desktop file) but more specifically the api version it was compiled
 * for so E can skip modules that are compiled for an incorrect API version
 * safely) */
EAPI E_Module_Api e_modapi =
{
   E_MODULE_API_VERSION,
   "DeviceMgr Module of Window Manager"
};

EAPI void *
e_modapi_init(E_Module *m)
{
   if (!eina_init())
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed @ eina_init()..!\n", __FUNCTION__);
        return NULL;
     }

   if (!e_devicemgr_embedded_compositor_init())
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed @ e_devicemgr_embedded_compositor_init()..!\n", __FUNCTION__);
        return NULL;
     }

   return m;
}

EAPI int
e_modapi_shutdown(E_Module *m)
{
   e_devicemgr_embedded_compositor_fini();
   eina_shutdown();

   return 1;
}

EAPI int
e_modapi_save(E_Module *m)
{
   /* Do Something */
   return 1;
}
