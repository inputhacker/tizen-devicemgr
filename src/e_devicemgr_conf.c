#include "e_devicemgr_privates.h"

#define E_DEVICEMGR_INPUT_DFLT_BACK_KEYCODE 166

void
_e_devicemgr_conf_value_check(E_Devicemgr_Config_Data* dconfig)
{
   if (!dconfig->conf) dconfig->conf = E_NEW(E_Devicemgr_Conf_Edd, 1);
   EINA_SAFETY_ON_NULL_RETURN(dconfig->conf);

   dconfig->conf->input.back_keycode = E_DEVICEMGR_INPUT_DFLT_BACK_KEYCODE;
}

void
e_devicemgr_conf_init(E_Devicemgr_Config_Data *dconfig)
{
   dconfig->conf_edd = E_CONFIG_DD_NEW("Devicemgr_Config", E_Devicemgr_Conf_Edd);
#undef T
#undef D
#define T E_Devicemgr_Conf_Edd
#define D dconfig->conf_edd
   E_CONFIG_VAL(D, T, input.button_remap_enable, CHAR);
   E_CONFIG_VAL(D, T, input.back_keycode, INT);

#undef T
#undef D
   dconfig->conf = e_config_domain_load("module.devicemgr", dconfig->conf_edd);

   if (!dconfig->conf)
     {
        SLOG(LOG_WARNING, "DEVICEMGR", "[e_devicemgr][%s] Failed to find module.devicemgr config file.\n", __FUNCTION__);
     }
   _e_devicemgr_conf_value_check(dconfig);
}

void
e_devicemgr_conf_fini(E_Devicemgr_Config_Data *dconfig)
{
   if (dconfig->conf)
     {
        E_FREE(dconfig->conf);
     }
   E_CONFIG_DD_FREE(dconfig->conf_edd);
}
