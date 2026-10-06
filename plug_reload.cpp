#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>

#include "plug.h"

static const char *plugin_name = "./libplug.so";

static plug_update_t plug_update;

void plug_reload(void)
{
	static void *plugin;
	if (plugin)
		dlclose(plugin);

	for (int attempt = 0; attempt < 10; attempt++) {
		plugin = dlopen(plugin_name, RTLD_NOW | RTLD_GLOBAL);
		if (plugin)
			break;
		usleep(100000);
	}

	if (!plugin) {
		fprintf(stderr, "dlopen: %s\n", dlerror());
		exit(1);
	}

	plug_update = (plug_update_t)dlsym(plugin, "plug_update");
	if (!plug_update) {
		fprintf(stderr, "dlsym plug_update: %s\n", dlerror());
		exit(1);
	}
	printf("reloading plug\n");
}

int plug_should_reload(time_t *last_mtime)
{
	struct stat st;

	if (stat(plugin_name, &st) != 0) {
		fprintf(stderr, "stat(%s) failed: %s\n", plugin_name, strerror(errno));
		exit(1);
	}

	if (st.st_mtime != *last_mtime) {
		*last_mtime = st.st_mtime;
		return 1;
	}

	return 0;
}
