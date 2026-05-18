#ifndef PLUG_H
#define PLUG_H

#include <stddef.h>

typedef void (*plug_init_t)(void);
typedef void (*plug_update_t)(void);
typedef void *(*plug_pre_reload_t)(void);
typedef void (*plug_post_reload_t)(void *state);

#endif /* PLUG_H */
