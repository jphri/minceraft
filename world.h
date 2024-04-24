#ifndef WORLD_H
#define WORLD_H

#define CHUNK_SIZE 16
#include "linmath.h"

typedef enum {
	BLOCK_NULL,
	BLOCK_GRASS,
	BLOCK_DIRT,
	BLOCK_LAST,
	BLOCK_UNLOADED = -1
} Block;

typedef enum {
	BACK,
	FRONT,
	LEFT,
	RIGHT,
	BOTTOM,
	TOP,
} Direction;

typedef struct Chunk Chunk;
struct Chunk {
	int blocks[CHUNK_SIZE][CHUNK_SIZE][CHUNK_SIZE];
	unsigned int chunk_vbo, chunk_vao;
	unsigned int vert_count;
	int state;
	int x, y, z;
	Chunk *next, *prev;
	Chunk *genqueue_next;
};

typedef struct RaycastWorld RaycastWorld;
struct RaycastWorld {
	vec3 direction, sign, position;
	vec3 step, tmax, tdelta;
	float max_distance;
	int state;
	Block block;
	Direction face;
};

void world_init();
void world_terminate();
void world_render();

void world_enqueue_load(int x, int y, int z);
void world_enqueue_unload(int x, int y, int z);

Block world_get_block(int x, int y, int z);
void  world_set_block(int x, int y, int z, Block block);

RaycastWorld world_begin_raycast(vec3 position, vec3 direction, float max_distance);
int          world_raycast(RaycastWorld *rw);

void block_face_to_dir(Direction dir, vec3 out);

#endif
