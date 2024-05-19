#include "glutil.h"
#include "util.h"
#include "world.h"
#include "chunk_renderer.h"
#include "worldgen.h"

#include <assert.h>
#include <math.h>
#include <GL/glew.h>
#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>
#include <limits.h>

typedef struct {
	int x, y, z;
	ChunkState state;
} Work;

#define MAX_BLOCKS 512
#define CHUNK_MAX_BLOCKS (MAX_BLOCKS / CHUNK_SIZE)
#define MAX_CHUNKS (CHUNK_MAX_BLOCKS * CHUNK_MAX_BLOCKS * CHUNK_MAX_BLOCKS)

static volatile Chunk *find_chunk(int x, int y, int z, ChunkState state);
static volatile Chunk *chunk_gen(int x, int y, int z, ChunkState state);
static volatile Chunk *allocate_chunk(int x, int y, int z);

static void insert_chunk(Chunk *c);
static void remove_chunk(Chunk *c);

static BlockProperties bprop[] = {
	[BLOCK_NULL]  = { .is_transparent = true, .is_ghost = true, .replaceable = true},
	[BLOCK_GLASS] = { .is_transparent = true }, 
	[BLOCK_WATER] = { .is_transparent = true }, 
	[BLOCK_GRASS_BLADES] = {
		.is_transparent = true, 
		.is_ghost = true
	},
	[BLOCK_ROSE] = {
		.is_transparent = true,
		.is_ghost = true,
		.replaceable = true,	
	},
	[BLOCK_LEAVES] = {
		.is_transparent = true,
		.replaceable = true
	}
};

static int running;

static Chunk *chunkmap[0x10000];
static Chunk *chunks;
static int    list_size[0x10000];
static int cx, cy, cz, cradius;
static pthread_mutex_t chunk_mutex;

static Chunk *chunks, *last_chunk;
static volatile int chunk_count;

void
world_init()
{
	running = true;

	chunks = NULL;
	memset(chunkmap, 0, sizeof(chunkmap));
	pthread_mutex_init(&chunk_mutex, NULL);
}

void
world_terminate()
{
	running = false;
	free(chunks);
}

Block
world_get_block(int x, int y, int z)
{
	return world_get(x, y, z, CSTATE_DECORATED);
}

void
world_set_block(int x, int y, int z, Block block)
{
	world_set(x, y, z, CSTATE_ALLOCATED, block);
}

Block
world_get(int x, int y, int z, ChunkState state)
{
	int chunk_x = x & CHUNK_MASK;
	int chunk_y = y & CHUNK_MASK;
	int chunk_z = z & CHUNK_MASK;

	volatile Chunk *ch;
	if(!(ch = chunk_gen(chunk_x, chunk_y, chunk_z, state))) {
		return BLOCK_UNLOADED;
	}
	

	x &= BLOCK_MASK;
	y &= BLOCK_MASK;
	z &= BLOCK_MASK;

	return ch->blocks[z][y][x];
}

float
world_get_density(int x, int y, int z, ChunkState state)
{
	int chunk_x = x & CHUNK_MASK;
	int chunk_y = y & CHUNK_MASK;
	int chunk_z = z & CHUNK_MASK;

	volatile Chunk *ch;
	if(!(ch = chunk_gen(chunk_x, chunk_y, chunk_z, state))) {
		return NAN;
	}

	x &= BLOCK_MASK;
	y &= BLOCK_MASK;
	z &= BLOCK_MASK;

	return (float)ch->density[z][y][x] / 1024.0;
}

void
world_set_density(int x, int y, int z, ChunkState state, float r)
{
	int chunk_x = x & CHUNK_MASK;
	int chunk_y = y & CHUNK_MASK;
	int chunk_z = z & CHUNK_MASK;

	volatile Chunk *ch;
	if(!(ch = chunk_gen(chunk_x, chunk_y, chunk_z, state))) {
		return;
	}

	x &= BLOCK_MASK;
	y &= BLOCK_MASK;
	z &= BLOCK_MASK;

	r *= 1024.0;
	if(r > SHRT_MAX)
		r = SHRT_MAX;
	if(r < SHRT_MIN)
		r = SHRT_MIN;

	ch->density[z][y][x] = (short)r;
}

void
world_set(int x, int y, int z, ChunkState state, Block block)
{
	int chunk_x = x & CHUNK_MASK;
	int chunk_y = y & CHUNK_MASK;
	int chunk_z = z & CHUNK_MASK;

	volatile Chunk *ch;
	if(!(ch = chunk_gen(chunk_x, chunk_y, chunk_z, state))) {
		return;
	}

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

	c->prev_alloc = NULL;
	c->next_alloc = chunks;
	if(chunks)
		chunks->prev_alloc = c;
	if(!last_chunk)
		last_chunk = c;
	chunks = c;

	list_size[hash]++;
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
	list_size[hash]--;

	if(c->prev_alloc)
		c->prev_alloc->next_alloc = c->next_alloc;
	if(c->next_alloc)
		c->next_alloc->prev_alloc = c->prev_alloc;
	if(c == chunks)
		chunks = c->next_alloc;
	if(c == last_chunk)
		last_chunk = c->prev_alloc;
}

volatile Chunk *
chunk_gen(int x, int y, int z, ChunkState target_state)
{
	if(!world_can_load(x, y, z))
		return NULL;

	volatile Chunk *c = find_chunk(x, y, z, 0);
	if(!c) {
		c = allocate_chunk(x, y, z);
	}

	#define MAKE_STATE(STATE) \
		case STATE: \
			if(target_state <= STATE) \
				break;
	
	#define MAKE_ING_STATE(STATE) \
		case STATE: \
			if(target_state <= STATE) \
				return c; \
			else \
			 	return NULL;

	switch(c->state) {
	MAKE_STATE(CSTATE_FREE)
		c->state = CSTATE_ALLOCATED;

	MAKE_STATE(CSTATE_ALLOCATED)
		c->state = CSTATE_SHAPING;
		wgen_shape(c->x, c->y, c->z);
		c->state = CSTATE_SHAPED;

	MAKE_STATE(CSTATE_SHAPED)
		c->state = CSTATE_SURFACING;
		wgen_surface(c->x, c->y, c->z);
		c->state = CSTATE_SURFACED;
		
	MAKE_STATE(CSTATE_SURFACED)
		c->state = CSTATE_DECORATING;
		wgen_decorate(c->x, c->y, c->z);
		c->state = CSTATE_DECORATED;

	MAKE_STATE(CSTATE_DECORATED)
		break;
	
	MAKE_ING_STATE(CSTATE_SHAPING);
	MAKE_ING_STATE(CSTATE_SURFACING);
	MAKE_ING_STATE(CSTATE_DECORATING);
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
	c = NULL;
	if(chunk_count > MAX_CHUNKS)
		for(c = last_chunk; c; c = c->prev_alloc) {
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
		
	if(c == NULL) {
		c = malloc(sizeof(*c));
		chunk_count ++;
	}

	c->free = false;
	c->x = x;
	c->y = y;
	c->z = z;
	c->state = CSTATE_FREE;
	insert_chunk(c);
	pthread_mutex_unlock(&chunk_mutex);
	return c;
}

bool
world_can_load(int x, int y, int z)
{
	int dx = abs(cx - x);
	int dy = abs(cy - y);
	int dz = abs(cz - z);
	return dx <= cradius && dy <= cradius && dz <= cradius;
}

int
world_allocated_chunks_count()
{
	return chunk_count;
}

