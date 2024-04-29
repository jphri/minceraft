#ifndef CUBEGAME_H
#define CUBEGAME_H

#include <linmath.h>
#include "world.h"

void chunk_renderer_generate_buffers(Chunk *chunk);
void chunk_renderer_render_solid_chunk(Chunk *chunk);
void chunk_renderer_render_water_chunk(Chunk *chunk);

#endif
