#ifndef CHUNK_RENDERER_H
#define CHUNK_RENDERER_H

#include "util.h"
#include "world.h"

void chunk_render_init();
void chunk_render_terminate();

void chunk_render_set_camera(vec3 position, vec3 look_at, float aspect);
void chunk_render_update();

void chunk_render_generate_faces(Chunk *chunk, ChunkFaceWork *w);
void chunk_render_generate_buffers(ChunkFaceWork *w);
void chunk_render_render_solid_chunk(Chunk *chunk);
void chunk_render_render_water_chunk(Chunk *chunk);

#endif
