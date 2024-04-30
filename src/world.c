#include "glutil.h"
#include "util.h"
#include "world.h"
#include "chunk_renderer.h"

#include <math.h>
#include <GL/glew.h>
#include <pthread.h>
#include <unistd.h>

typedef enum {
	GENERATING,
	READY,
} ChunkState;

typedef struct {
	int x, y, z;
} Work;

#define MAX_CHUNKS 8192
#define MAX_WORK 1024
#define NUM_WORKERS 4

static void chunk_randomize(Chunk *chunk);
static void chunk_worker_func(WorkGroup *wg);
static volatile Chunk *find_chunk(int x, int y, int z);
static volatile Chunk *find_complete_chunk(int x, int y, int z);
static Chunk *find_free_chunk();
static Chunk *allocate_chunk();

static BlockProperties bprop[] = {
	[BLOCK_NULL]  = { .is_transparent = true },
	[BLOCK_GLASS] = { .is_transparent = true }, 
	[BLOCK_WATER] = { .is_transparent = true }
};

static int running;

static Chunk *chunks;
static volatile int max_chunk_id;

static pthread_mutex_t chunk_mutex;

static WorkGroup *workg;

static int cx, cy, cz, cradius;
static int count_chunks;

void
world_init()
{
	running = true;

	chunks = malloc(sizeof(Chunk) * MAX_CHUNKS);
	for(int i = 0; i < MAX_CHUNKS; i++) {
		chunks[i].free = true;
		chunks[i].state = READY;
	}

	workg = wg_init(chunk_worker_func, sizeof(Work), MAX_WORK, 4);
}

void
world_terminate()
{
	running = false;
	wg_terminate(workg);
	pthread_mutex_destroy(&chunk_mutex);
	free(chunks);
}

void
world_enqueue_load(int x, int y, int z)
{
	wg_send(workg, &(Work){
		.x = x,
		.y = y, 
		.z = z	
	});
}

void
chunk_worker_func(WorkGroup *wg)
{
	Work my_work;
	while(wg_recv(wg, &my_work)) {
		if(find_chunk(my_work.x, my_work.y, my_work.z))
			continue;
		
		Chunk *chunk = allocate_chunk();
		chunk->state = GENERATING;

		chunk->x = my_work.x & CHUNK_MASK;
		chunk->y = my_work.y & CHUNK_MASK;
		chunk->z = my_work.z & CHUNK_MASK;
		chunk_randomize(chunk);
		chunk->state = READY;
	}
}

void
chunk_randomize(Chunk *chunk)
{
	for(int z = 0; z < CHUNK_SIZE; z++)
	for(int y = 0; y < CHUNK_SIZE; y++)
	for(int x = 0; x < CHUNK_SIZE; x++) {
		if(rand() & 1) {
			chunk->blocks[x][y][z] = rand() % (BLOCK_LAST - BLOCK_GRASS) + BLOCK_GRASS;
		} else {
			chunk->blocks[x][y][z] = 0;
		}
	}
}

volatile Chunk *
find_chunk(int x, int y, int z)
{
	volatile Chunk *c = chunks;
	for(; c < chunks + max_chunk_id + 1; c++) {
		if(!c->free && c->x == x && c->y == y && c->z == z)
			return c;
	}
	return NULL;
}

Chunk *
find_free_chunk()
{
	for(Chunk *c = chunks; c < chunks + MAX_CHUNKS; c++)  {
		if(c->state == READY) {
			if(!c->free) {
				/* if it is too far way, treat as a freed too */
				int dx = abs(c->x - cx);
				int dy = abs(c->y - cy);
				int dz = abs(c->z - cz);
				if(dx > cradius || dy > cradius || dz > cradius) {
					return c;
				}
			} else {
				return c;
			}
		}
	}
	return NULL;
}

Chunk *
allocate_chunk()
{
	pthread_mutex_lock(&chunk_mutex);
	Chunk *c = find_free_chunk();
	if(!c) {
		return NULL;
	}
	c->free = false;
	if(c - chunks > max_chunk_id)
		max_chunk_id = (c - chunks);
	count_chunks++;
	pthread_mutex_unlock(&chunk_mutex);

	return c;
}

Block
world_get_block(int x, int y, int z)
{
	int chunk_x = x & CHUNK_MASK;
	int chunk_y = y & CHUNK_MASK;
	int chunk_z = z & CHUNK_MASK;

	volatile Chunk *ch = find_chunk(chunk_x, chunk_y, chunk_z);
	if(!ch) {
		world_enqueue_load(chunk_x, chunk_y, chunk_z);
		/* spinlock */
		while((ch = find_complete_chunk(chunk_x, chunk_y, chunk_z)) == NULL);
	}

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

	volatile Chunk *ch = find_chunk(chunk_x, chunk_y, chunk_z);
	if(!ch) {
		world_enqueue_load(chunk_x, chunk_y, chunk_z);
		/* spinlock */
		while((ch = find_complete_chunk(chunk_x, chunk_y, chunk_z)) == NULL);
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

volatile Chunk *
find_complete_chunk(int x, int y, int z)
{
	volatile Chunk *c = find_chunk(x, y, z);
	if(c && c->state == READY)
		return c;
	return NULL;
}

void
world_set_load_border(int x, int y, int z, int radius)
{
	cx = x;
	cy = y;
	cz = z;
	cradius = radius;
}
