#ifndef __E_DEVICEMGR_TDM_H__
#define __E_DEVICEMGR_TDM_H__

#include <tdm.h>
#include <tbm_bufmgr.h>
#include <tbm_surface.h>
#include <tbm_surface_internal.h>

#define C(b,m)              (((b) >> (m)) & 0xFF)
#define B(c,s)              ((((unsigned int)(c)) & 0xff) << (s))
#define FOURCC(a,b,c,d)     (B(d,24) | B(c,16) | B(b,8) | B(a,0))
#define FOURCC_STR(id)      C(id,0), C(id,8), C(id,16), C(id,24)
#define FOURCC_ID(str)      FOURCC(((char*)str)[0],((char*)str)[1],((char*)str)[2],((char*)str)[3])

#define IS_RGB(f)           ((f) == TBM_FORMAT_XRGB8888 || (f) == TBM_FORMAT_ARGB8888)
#define ROUNDUP(s,c)        (((s) + (c-1)) & ~(c-1))
#define ALIGN_TO_16B(x)     ((((x) + (1 <<  4) - 1) >>  4) <<  4)
#define ALIGN_TO_32B(x)     ((((x) + (1 <<  5) - 1) >>  5) <<  5)
#define ALIGN_TO_128B(x)    ((((x) + (1 <<  7) - 1) >>  7) <<  7)
#define ALIGN_TO_2KB(x)     ((((x) + (1 << 11) - 1) >> 11) << 11)
#define ALIGN_TO_8KB(x)     ((((x) + (1 << 13) - 1) >> 13) << 13)
#define ALIGN_TO_64KB(x)    ((((x) + (1 << 16) - 1) >> 16) << 16)

typedef struct _E_DevMgr_Display
{
   int drm_fd;
   tbm_bufmgr bufmgr;
   tdm_display *tdm;

   Eina_Bool pp_available;
   Eina_Bool capture_available;
} E_DevMgr_Display;

extern E_DevMgr_Display *e_devmgr_dpy;

int e_devicemgr_tdm_init(void);
void e_devicemgr_tdm_fini(void);
void e_devicemgr_tdm_update(void);

tdm_output *e_devicemgr_tdm_output_get(Ecore_Drm_Output *output);
tdm_layer *e_devicemgr_tdm_video_layer_get(tdm_output *output);
tdm_layer *e_devicemgr_tdm_avaiable_video_layer_get(tdm_output *output);
void e_devicemgr_tdm_set_layer_usable(tdm_layer *layer, Eina_Bool usable);
Eina_Bool e_devicemgr_tdm_get_layer_usable(tdm_layer *layer);

#endif
