#include "imgui.h"
#include "implot.h"

#include "backend_abstraction.cpp"

int main(void)
{
	Voronoi voronoi = {};
	backend_init(&voronoi);

	plug_reload();

	time_t last_mtime = 0;
	plug_should_reload(&last_mtime);

	bool done = false;
	while (!done) {
		check_for_exit(&done);

		if (plug_should_reload(&last_mtime)) {
			plug_reload();
		}

		int display_w, display_h;
		glfwGetFramebufferSize(window, &display_w, &display_h);
		float dt = ImGui::GetIO().DeltaTime;

		glViewport(0, 0, display_w, display_h);
		draw_voronoi_gl(&voronoi, display_w, display_h);

		new_frame();
		{
			Plug plug = {.voronoi = &voronoi, .dt = dt, .display_w = display_w, .display_h = display_h};
			plug_update(&plug);
		}
		render();
	}
	backend_exit();
	return 0;
}
