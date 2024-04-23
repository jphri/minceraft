#ifndef WORLD_H
#define WORLD_H

#define CHUNK_SIZE 16

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

void world_init();
void world_terminate();
void world_render();

void world_enqueue_load(int x, int y, int z);
void world_enqueue_unload(int x, int y, int z);

Block world_get_block(int x, int y, int z);
void  world_set_block(int x, int y, int z, Block block);
#endif
