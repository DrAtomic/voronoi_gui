#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>

#include "plug.h"

static const char *lib_plug_name = "./lib_plug.so";
static void *libplug;

static plug_init_t plug_init;
static plug_update_t plug_update;
static plug_pre_reload_t plug_pre_reload;
static plug_post_reload_t plug_post_reload;

void plug_reload(void)
{
	if (libplug)
		dlclose(libplug);

	for (int attempt = 0; attempt < 10; attempt++) {
		libplug = dlopen(lib_plug_name, RTLD_NOW | RTLD_GLOBAL);
		if (libplug)
			break;
		usleep(100000);
	}

	if (!libplug) {
		fprintf(stderr, "dlopen: %s\n", dlerror());
		exit(1);
	}

	plug_init = (plug_init_t)dlsym(libplug, "plug_init");
	if (!plug_init) {
		fprintf(stderr, "dlsym plug_init: %s\n", dlerror());
		exit(1);
	}

	plug_update = (plug_update_t)dlsym(libplug, "plug_update");
	if (!plug_update) {
		fprintf(stderr, "dlsym plug_update: %s\n", dlerror());
		exit(1);
	}

	plug_pre_reload = (plug_pre_reload_t)dlsym(libplug, "plug_pre_reload");
	if (!plug_pre_reload) {
		fprintf(stderr, "dlsym plug_pre_reload: %s\n", dlerror());
		exit(1);
	}

	plug_post_reload = (plug_post_reload_t)dlsym(libplug, "plug_post_reload");
	if (!plug_post_reload) {
		fprintf(stderr, "dlsym plug_post_reload: %s\n", dlerror());
		exit(1);
	}

	printf("reloading plug\n");
}

int plug_should_reload(time_t *last_mtime)
{
	struct stat st;

	if (stat(lib_plug_name, &st) != 0) {
		fprintf(stderr, "stat(%s) failed: %s\n", lib_plug_name, strerror(errno));
		exit(1);
	}

	if (st.st_mtime != *last_mtime) {
		*last_mtime = st.st_mtime;
		return 1;
	}

	return 0;
}
