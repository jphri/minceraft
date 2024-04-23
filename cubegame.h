#ifndef CUBEGAME_H
#define CUBEGAME_H

#include "linmath.h"
#include "world.h"

void chunk_renderer_generate_buffers(Chunk *chunk);
void chunk_renderer_render_chunk(unsigned int vao, unsigned int vertex_count, vec3 position);

#endif
