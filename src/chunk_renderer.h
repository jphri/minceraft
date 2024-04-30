#ifndef CHUNK_RENDERER_H
#define CHUNK_RENDERER_H

#include "util.h"
#include "world.h"

void chunk_render_init();
void chunk_render_terminate();

void chunk_render_set_camera(vec3 position, vec3 look_at, float aspect, float distance);
void chunk_render_update();
void chunk_render_request_update_block(int x, int y, int z);

void chunk_render();

#endif
