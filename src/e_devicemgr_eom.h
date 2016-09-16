#ifndef __E_DEVICEMGR_EOM_H__
#define __E_DEVICEMGR_EOM_H__

#include "e.h"

int e_devicemgr_eom_init(void);
void e_devicemgr_eom_fini(void);
Eina_Bool e_devicemgr_eom_is_ec_external(E_Client *ec);
tdm_output* e_devicemgr_eom_tdm_output_by_ec_get(E_Client *ec);

#endif
