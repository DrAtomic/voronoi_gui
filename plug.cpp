#include <stdio.h>

#include "imgui.h"
#include "implot.h"
#include "plug.h"

static void update_voronoi_background(Voronoi *v, float dt, int w, int h)
{
	if (dt > 0.033f) {
		dt = 0.033f;
	}

	for (size_t i = 0; i < v->active_size; i++) {
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

static void say_something(int x)
{
	printf("the number said is %d\n", x);
}

static void say_something(void)
{
	printf("hello!\n");
}

extern "C" void plug_update(Plug *plug)
{
	update_voronoi_background(plug->voronoi, plug->dt, plug->display_w, plug->display_h);

	ImGui::Begin("voronoi settings");
	ImGui::SliderInt("voronoi cells", (int *)&plug->voronoi->active_size, 1, plug->voronoi->max_size);
	ImGui::End();

	ImGui::Begin("hello world!");
	if (ImGui::Button("I am a button")) {
		say_something();
	}
	ImGui::End();

	ImGui::Begin("another button");
	if (ImGui::Button("I am another button")) {
		say_something(1);
	}

	ImGui::End();

	if (ImGui::Begin("Plot")) {
		int buf[20];
		for (size_t i = 0; i < 20; i++)
			buf[i] = i;

		ImPlot::BeginPlot("hello plot", ImVec2(-1,0), ImPlotFlags_Equal);
		ImColor m_color = IM_COL32(100,000,255,100);
		ImPlotSpec spec;
		spec.LineColor = m_color;
		ImPlot::PlotScatter("hello plot", buf, buf, 20, spec);
		ImPlot::EndPlot();
	}
	ImGui::End();
}
