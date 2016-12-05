#ifndef __E_DEVICEMGR_VIDEO_H__
#define __E_DEVICEMGR_VIDEO_H__

#define E_COMP_WL
#include "e.h"
#include "e_comp_wl.h"
#include <wayland-server.h>
#include <Ecore_Drm.h>
#include "e_devicemgr_buffer.h"

typedef struct _E_Video E_Video;

int e_devicemgr_video_init(void);
void e_devicemgr_video_fini(void);

Eina_List* e_devicemgr_video_list_get(void);
E_Devmgr_Buf* e_devicemgr_video_fb_get(E_Video *video);
void e_devicemgr_video_pos_get(E_Video *video, int *x, int *y);
Ecore_Drm_Output* e_devicemgr_video_drm_output_get(E_Video *video);
tdm_layer* e_devicemgr_video_layer_get(tdm_output *output);

#endif
