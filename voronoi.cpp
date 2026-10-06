#include <stdint.h>
#include <time.h>

#include "voronoi.h"

static __inline__ float rand_float01(void)
{
	return (float)rand() / (float)RAND_MAX;
}

static __inline__ float rand_float_range(float min, float max)
{
	return min + (max - min) * rand_float01();
}

void init_voronoi(Voronoi *v, int w, int h, size_t seed_count)
{
	srand(time(0));

	v->max_size = seed_count;
	v->active_size = seed_count;
	v->seeds = (Voronoi_Seed *)malloc(v->max_size * sizeof(*v->seeds));

	for (size_t i = 0; i < v->max_size; i++) {
		Voronoi_Seed *s = &v->seeds[i];

		s->x = rand_float_range(0.0f, (float)w);
		s->y = rand_float_range(0.0f, (float)h);

		float angle = rand_float_range(0.0f, 2 * M_PI);
		float speed = rand_float_range(1.0f, 10.0f);

		s->vx = cosf(angle) * speed;
		s->vy = sinf(angle) * speed;

		ImGui::ColorConvertHSVtoRGB(0.58, 0.55, rand_float_range(0.40, 0.55), s->r, s->g, s->b);
	}
}
