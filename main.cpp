#include "imgui.h"
#include "implot.h"

#include "backend_abstraction.cpp"

int main(void)
{
	backend_init();

	plug_reload();

	plug_init();

	time_t last_mtime = 0;
	plug_should_reload(&last_mtime);

	bool done = false;
	while (!done) {
		check_for_exit(&done);

		if (plug_should_reload(&last_mtime)) {
			void *state = plug_pre_reload();
			plug_reload();
			plug_post_reload(state);
		}

		new_frame();
		{
			plug_update();
		}
		render();
	}
	backend_exit();
	return 0;
}
