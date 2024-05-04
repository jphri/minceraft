#include "glutil.h"
#include "util.h"
#include "world.h"
#include "chunk_renderer.h"

#include <math.h>
#include <GL/glew.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <unistd.h>


typedef struct {
	int x, y, z;
	ChunkState state;
} Work;

#define MAX_CHUNKS 8192
#define MAX_WORK 1024
#define NUM_WORKERS 4


static volatile Chunk *find_chunk(int x, int y, int z, ChunkState state);
static volatile Chunk *chunk_gen(int x, int y, int z, ChunkState state);
static volatile Chunk *allocate_chunk(int x, int y, int z);
static void chunk_randomize(Chunk *chunk);

static void insert_chunk(Chunk *c);
static void remove_chunk(Chunk *c);

static BlockProperties bprop[] = {
	[BLOCK_NULL]  = { .is_transparent = true },
	[BLOCK_GLASS] = { .is_transparent = true }, 
	[BLOCK_WATER] = { .is_transparent = true }
};

static int running;

static Chunk *chunkmap[65536];
static Chunk *chunks;
static volatile int max_chunk_id;
static int cx, cy, cz, cradius;
static pthread_mutex_t chunk_mutex;

void
world_init()
{
	running = true;

	chunks = malloc(sizeof(Chunk) * MAX_CHUNKS);
	for(int i = 0; i < MAX_CHUNKS; i++) {
		chunks[i].free = true;
		chunks[i].state = CSTATE_FREE;
	}
	memset(chunkmap, 0, sizeof(chunkmap));
	pthread_mutex_init(&chunk_mutex, NULL);
}

void
world_terminate()
{
	running = false;
	free(chunks);
}

void
chunk_randomize(Chunk *chunk)
{
	for(int z = 0; z < CHUNK_SIZE; z++)
	for(int y = 0; y < CHUNK_SIZE; y++)
	for(int x = 0; x < CHUNK_SIZE; x++) {
		if(!(rand() & 15) && x > 1 && x < 14 && z > 1 && z < 14) {
			chunk->blocks[x][y][z] = rand() % (BLOCK_LAST - BLOCK_GRASS) + BLOCK_GRASS;
		} else {
			chunk->blocks[x][y][z] = 0;
		}
	}
}

Block
world_get_block(int x, int y, int z)
{
	int chunk_x = x & CHUNK_MASK;
	int chunk_y = y & CHUNK_MASK;
	int chunk_z = z & CHUNK_MASK;

	volatile Chunk *ch;
	while((ch = chunk_gen(chunk_x, chunk_y, chunk_z, CSTATE_MERGED)) == NULL);
	
	x &= BLOCK_MASK;
	y &= BLOCK_MASK;
	z &= BLOCK_MASK;
	
	return ch->blocks[z][y][x];
}

void
world_set_block(int x, int y, int z, Block block)
{
	int chunk_x = x & CHUNK_MASK;
	int chunk_y = y & CHUNK_MASK;
	int chunk_z = z & CHUNK_MASK;

	volatile Chunk *ch;
	while((ch = chunk_gen(chunk_x, chunk_y, chunk_z, CSTATE_ALLOCATED)) == NULL);

	x &= BLOCK_MASK;
	y &= BLOCK_MASK;
	z &= BLOCK_MASK;

	ch->blocks[z][y][x] = block;
}

RaycastWorld
world_begin_raycast(vec3 position, vec3 direction, float max_distance)
{
	RaycastWorld r;

	vec3_floor(r.position, position);
	vec3_dup(r.direction, direction);
	r.max_distance = max_distance;
	vec3_sign(r.step, direction);
	vec3_nextint(r.tmax, position, direction);
	vec3_div(r.tdelta, r.step, direction);
	r.state = 0;

	return r;
}

int
world_raycast(RaycastWorld *rw)
{
	/* fast voxel traversal algorithm
	 * http://www.cse.yorku.ca/~amana/research/grid.pdf */

	while(true) {
		switch(rw->state) {
		case 0:
			rw->state = 1;
			if((rw->block = world_get_block(rw->position[0], rw->position[1], rw->position[2])) > 0) {
				return 1;
			}
			/* fallthrough */
		case 1:
			rw->state = 0;
			if(rw->tmax[0] < rw->tmax[1]) {
				if(rw->tmax[0] < rw->tmax[2]) {
					if(rw->tmax[0] > rw->max_distance)
						return 0;

					rw->position[0] += rw->step[0];
					rw->tmax[0] += rw->tdelta[0];
					if(rw->step[0] < 0)
						rw->face = RIGHT;
					else
						rw->face = LEFT;
				} else {
					if(rw->tmax[2] > rw->max_distance)
						return 0;

					rw->position[2] += rw->step[2];
					rw->tmax[2] += rw->tdelta[2];
					if(rw->step[2] < 0)
						rw->face = FRONT;
					else
						rw->face = BACK;
				}
			} else {
				if(rw->tmax[1] < rw->tmax[2]) {
					if(rw->tmax[1] > rw->max_distance)
						return 0;

					rw->position[1] += rw->step[1];
					rw->tmax[1] += rw->tdelta[1];
					if(rw->step[1] < 0)
						rw->face = TOP;
					else
						rw->face = BOTTOM;
				} else {
					if(rw->tmax[2] > rw->max_distance)
						return 0;

					rw->position[2] += rw->step[2];
					rw->tmax[2] += rw->tdelta[2];
					if(rw->step[2] < 0)
						rw->face = FRONT;
					else
						rw->face = BACK;
				}
			}
		}
	}
}

void
block_face_to_dir(Direction dir, vec3 out)
{
	switch(dir) {
	case LEFT:   vec3_dup(out, (vec3){ -1.0,  0.0,  0.0 }); break;
	case RIGHT:  vec3_dup(out, (vec3){  1.0,  0.0,  0.0 }); break;
	case TOP:    vec3_dup(out, (vec3){  0.0,  1.0,  0.0 }); break;
	case BOTTOM: vec3_dup(out, (vec3){  0.0, -1.0,  0.0 }); break;
	case FRONT:  vec3_dup(out, (vec3){  0.0,  0.0,  1.0 }); break;
	case BACK:   vec3_dup(out, (vec3){  0.0,  0.0, -1.0 }); break;
	}
}

const BlockProperties *
block_properties(Block b)
{
	return &bprop[b];
}

void
world_set_load_border(int x, int y, int z, int radius)
{
	cx = x;
	cy = y;
	cz = z;
	cradius = radius;
}

uint32_t
chunk_coord_hash(int x, int y, int z)
{
	uint32_t h = hash_int3(x, y, z);
	return (uint16_t)(h >> 16) ^ (h & 0xFFFF);
}

void
insert_chunk(Chunk *c)
{
	uint32_t hash = chunk_coord_hash(c->x, c->y, c->z);
	c->prev = NULL;
	c->next = chunkmap[hash];
	if(chunkmap[hash])
		chunkmap[hash]->prev = c;
	chunkmap[hash] = c;
}

void
remove_chunk(Chunk *c)
{
	uint32_t hash = chunk_coord_hash(c->x, c->y, c->z);
	if(c->prev)
		c->prev->next = c->next;
	if(c->next)
		c->next->prev = c->prev;
	if(chunkmap[hash] == c)
		chunkmap[hash] = c->next;
}

volatile Chunk *
chunk_gen(int x, int y, int z, ChunkState target_state)
{
	volatile Chunk *c = find_chunk(x, y, z, 0);
	if(!c) {
		c = allocate_chunk(x, y, z);
	}

	#define MAKE_STATE(STATE) \
		case STATE: \
			if(target_state <= STATE) \
				break;

	switch(c->state) {
	MAKE_STATE(CSTATE_FREE)
		c->state = CSTATE_ALLOCATED;

	MAKE_STATE(CSTATE_ALLOCATED)
		c->state = CSTATE_GENERATING;
		chunk_randomize(c);
		c->state = CSTATE_GENERATED;

	MAKE_STATE(CSTATE_GENERATED)
		c->state = CSTATE_MERGING;
		for(int z = -CHUNK_SIZE; z <= CHUNK_SIZE; z += CHUNK_SIZE)
		for(int y = -CHUNK_SIZE; y <= CHUNK_SIZE; y += CHUNK_SIZE)
		for(int x = -CHUNK_SIZE; x <= CHUNK_SIZE; x += CHUNK_SIZE) {
			chunk_gen(x + c->x, y + c->y, z + c->z, CSTATE_GENERATED);
		}
		c->state = CSTATE_MERGED;
	MAKE_STATE(CSTATE_MERGED)
		break;
	
	default:
		/* 
			-ing states are ignored and returns null for
			possible spinlock implementations.
		*/
		return NULL;
	}

	return c;
}

volatile Chunk *
find_chunk(int x, int y, int z, ChunkState state)
{
	volatile Chunk *c;
	uint32_t hash = chunk_coord_hash(x, y, z);
	c = chunkmap[hash];
	while(c) {
		if(!c->free && c->state >= state && c->x == x && c->y == y && c->z == z) {
			return c;
		}
		c = c->next;
	}
	return c;
}

volatile Chunk *
allocate_chunk(int x, int y, int z)
{
	Chunk *c;
	pthread_mutex_lock(&chunk_mutex);
	for(c = chunks; c <= chunks + max_chunk_id; c++) {
		if(c->free) {
			break;
		}

		int dx = abs(cx - c->x);
		int dy = abs(cy - c->y);
		int dz = abs(cz - c->z);
		if(dx > cradius || dy > cradius || dz > cradius) {
			remove_chunk(c);
			break;
		}
	}
	if(c - chunks > max_chunk_id) { 
		max_chunk_id++;
		c = chunks + max_chunk_id;
	}

	c->free = false;
	c->x = x;
	c->y = y;
	c->z = z;
	c->state = CSTATE_ALLOCATED;
	insert_chunk(c);
	pthread_mutex_unlock(&chunk_mutex);
	return c;
}
