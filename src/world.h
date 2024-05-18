#ifndef WORLD_H
#define WORLD_H

#define BLOCK_BITS  4
#define CHUNK_SIZE  (1 << BLOCK_BITS)
#define LAST_BLOCK  (CHUNK_SIZE - 1)
#define BLOCK_MASK  (CHUNK_SIZE - 1)
#define CHUNK_MASK  (~BLOCK_MASK)

#include "util.h"
#include <linmath.h>
#include <stdbool.h>

typedef enum {
	BLOCK_NULL,
	BLOCK_GRASS,
	BLOCK_DIRT,
	BLOCK_STONE,
	BLOCK_SAND,
	BLOCK_PLANKS,
	BLOCK_GLASS,
	BLOCK_WATER,
	BLOCK_GRASS_BLADES,
	BLOCK_ROSE,
	BLOCK_WOOD,
	BLOCK_LEAVES,
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

/* order is important */
typedef enum {
	CSTATE_FREE,
	CSTATE_ALLOCATED,
	CSTATE_SHAPING,
	CSTATE_SHAPED,
	CSTATE_SURFACING,
	CSTATE_SURFACED,
	CSTATE_DECORATING,
	CSTATE_DECORATED,
} ChunkState;

typedef struct Chunk Chunk;
struct Chunk {
	short density[CHUNK_SIZE][CHUNK_SIZE][CHUNK_SIZE];
	char surface[CHUNK_SIZE][CHUNK_SIZE][CHUNK_SIZE];
	char blocks[CHUNK_SIZE][CHUNK_SIZE][CHUNK_SIZE];
	ChunkState state;
	int x, y, z;
	bool free;
	Chunk *next, *prev;
	Chunk *next_alloc;
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

typedef struct {
	bool is_transparent;
	bool is_ghost;
	bool replaceable;
} BlockProperties;

void world_init();
void world_terminate();

void world_enqueue_load(int x, int y, int z);
void world_enqueue_unload(int x, int y, int z);

Block world_get_block(int x, int y, int z);
void  world_set_block(int x, int y, int z, Block block);

Block world_get(int x, int y, int z, ChunkState state);
void  world_set(int x, int y, int z, ChunkState state, Block block);

float world_get_density(int x, int y, int z, ChunkState state);
void  world_set_density(int x, int y, int z, ChunkState state, float r);

RaycastWorld world_begin_raycast(vec3 position, vec3 direction, float max_distance);
int          world_raycast(RaycastWorld *rw);

void block_face_to_dir(Direction dir, vec3 out);
const BlockProperties *block_properties(Block block);

void world_set_load_border(int x, int y, int z, int radius);
bool world_can_load(int x, int y, int z);

uint32_t chunk_coord_hash(int x, int y, int z);

int world_allocated_chunks_count();

#endif
