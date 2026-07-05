#include <stdint.h>
#include <time.h>

typedef struct Voronoi_Seed {
	float x;
	float y;
	float vx;
	float vy;
	float r;
	float g;
	float b;
} Voronoi_Seed;

typedef struct Voronoi {
	Voronoi_Seed *seeds;
	size_t size;
} Voronoi;

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

	v->size = seed_count;
	v->seeds = (Voronoi_Seed *)malloc(v->size * sizeof(*v->seeds));

	for (size_t i = 0; i < v->size; i++) {
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

void update_voronoi_background(Voronoi *v, float dt, int w, int h)
{
	if (dt > 0.033f) {
		dt = 0.033f;
	}

	for (size_t i = 0; i < v->size; i++) {
		Voronoi_Seed *s = &v->seeds[i];

		s->x += s->vx * dt;
		s->y += s->vy * dt;

		if (s->x < 0.0f) {
			s->x = 0.0f;
			s->vx *= -1.0f;
		}

		if (s->x > (float)w) {
			s->x = (float)w;
			s->vx *= -1.0f;
		}

		if (s->y < 0.0f) {
			s->y = 0.0f;
			s->vy *= -1.0f;
		}

		if (s->y > (float)h) {
			s->y = (float)h;
			s->vy *= -1.0f;
		}
	}
}
