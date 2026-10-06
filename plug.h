#ifndef PLUG_H
#define PLUG_H

#include "voronoi.h"

typedef struct Plug {
	Voronoi *voronoi;
	float dt;
	int display_w;
	int display_h;
} Plug;

typedef void (*plug_update_t)(Plug *plug);

#endif /* PLUG_H */
