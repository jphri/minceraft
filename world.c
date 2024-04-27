#include "util.h"
#include "world.h"
#include "cubegame.h"

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
static void *chunk_worker_func(void *);
static Chunk *find_chunk(int x, int y, int z);
static Chunk *find_free_chunk();
static Chunk *allocate_chunk();

static BlockProperties bprop[] = {
	[BLOCK_NULL]  = { .is_transparent = true },
	[BLOCK_GLASS] = { .is_transparent = true }
};

static int running;

static Chunk chunks[MAX_CHUNKS];
static int max_chunk_id;

static pthread_t chunk_worker[NUM_WORKERS];
static pthread_mutex_t generate_mutex;
static pthread_cond_t generate_cond;

static pthread_mutex_t chunk_mutex;

static Work work[MAX_WORK];
static int work_begin, work_end;
static int work_size;

void
world_init()
{
	running = true;

	for(int i = 0; i < MAX_CHUNKS; i++) {
		chunks[i].free = true;
		chunks[i].state = READY;
	}

	work_begin = 0;
	work_end = 0;

	pthread_cond_init(&generate_cond, NULL);
	pthread_mutex_init(&generate_mutex, NULL);
	pthread_mutex_init(&chunk_mutex, NULL);
	for(int i = 0; i < NUM_WORKERS; i++) {
		pthread_create(&chunk_worker[i], NULL, chunk_worker_func, NULL);
	}
}

void
world_terminate()
{
	running = false;
	pthread_cond_broadcast(&generate_cond);
	for(int i = 0; i < NUM_WORKERS; i++) {
		pthread_join(chunk_worker[i], NULL);
	}
	pthread_mutex_destroy(&generate_mutex);
	pthread_cond_destroy(&generate_cond);

	pthread_mutex_destroy(&chunk_mutex);
}

void
world_enqueue_load(int x, int y, int z)
{
	pthread_mutex_lock(&generate_mutex);
	for(int k = 0; k < work_size; k++) {
		int i = (work_begin + k) % MAX_WORK;
		if(work[i].x == x && work[i].y == y && work[i].z == z) {
			pthread_mutex_unlock(&generate_mutex);
			return;
		}
	}
	while(work_size >= MAX_WORK)
		pthread_cond_wait(&generate_cond, &generate_mutex);

	work[work_end].x = x;
	work[work_end].y = y;
	work[work_end].z = z;
	work_end ++;
	work_size ++;

	if(work_end >= MAX_WORK) {
		work_end = 0;
	}
	pthread_mutex_unlock(&generate_mutex);
	pthread_cond_signal(&generate_cond);
}

void
world_enqueue_unload(int x, int y, int z)
{
	Chunk *chunk = find_chunk(x, y, z);
	if(!chunk)
		return;

	chunk->free = true;

	if(glIsBuffer(chunk->chunk_vbo)) {
		glDeleteBuffers(1, &chunk->chunk_vbo);
		glDeleteVertexArrays(1, &chunk->chunk_vao);
	}
}

void
world_render()
{
	for(Chunk *chunk = chunks;
		chunk < chunks + max_chunk_id + 1;
		chunk++)
	{
		if(chunk->state != READY) 
			continue;
		
		if(!chunk->chunk_vbo) {
			chunk_renderer_generate_buffers(chunk);
		}

		if(chunk->vert_count > 0) {
			chunk_renderer_render_chunk(chunk->chunk_vao, chunk->vert_count, (vec3){ chunk->x, chunk->y, chunk->z });
		}
	}
	printf("max chunk id: %d\n", max_chunk_id);
}

void *
chunk_worker_func()
{
	while(true) {
		Work my_work;

		if(!running) {
			return NULL;
		}

		pthread_mutex_lock(&generate_mutex);
		while(work_size == 0) {
			if(!running) {
				pthread_mutex_unlock(&generate_mutex);
				return NULL;
			}
			pthread_cond_wait(&generate_cond, &generate_mutex);
		}
		my_work = work[work_begin];
		work_begin++;
		work_size --;
		if(work_begin >= MAX_WORK) {
			work_begin = 0;
		}
		pthread_mutex_unlock(&generate_mutex);
		pthread_cond_signal(&generate_cond);

		if(find_chunk(my_work.x, my_work.y, my_work.z))
			continue;

		Chunk *chunk = allocate_chunk();
		chunk->state = GENERATING;
		chunk->free = false;

		chunk->x = my_work.x & CHUNK_MASK;
		chunk->y = my_work.y & CHUNK_MASK;
		chunk->z = my_work.z & CHUNK_MASK;
		chunk->chunk_vbo = 0;
		chunk->chunk_vao = 0;
		chunk->vert_count = 0;

		chunk_randomize(chunk);
		chunk->state = READY;

		usleep(100000 + (rand() * 1000000) / RAND_MAX);
	}
	return NULL;
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

Chunk *
find_chunk(int x, int y, int z)
{
	Chunk *c = chunks;
	for(; c < chunks + max_chunk_id + 1; c++) {
		if(!c->free && c->x == x && c->y == y&& c->z == z)
			return c;
	}
	return NULL;
}

Chunk *
find_free_chunk()
{
	for(Chunk *c = chunks; c < chunks + MAX_CHUNKS; c++)  {
		if(c->free && c->state == READY)
			return c;
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
	pthread_mutex_unlock(&chunk_mutex);

	return c;
}

Block
world_get_block(int x, int y, int z)
{
	int chunk_x = x & CHUNK_MASK;
	int chunk_y = y & CHUNK_MASK;
	int chunk_z = z & CHUNK_MASK;

	Chunk *ch = find_chunk(chunk_x, chunk_y, chunk_z);
	if(!ch)
		return BLOCK_UNLOADED;

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

	Chunk *ch = find_chunk(chunk_x, chunk_y, chunk_z);
	if(!ch)
		return;

	x &= BLOCK_MASK;
	y &= BLOCK_MASK;
	z &= BLOCK_MASK;

	ch->blocks[z][y][x] = block;
	if(glIsBuffer(ch->chunk_vbo)) {
		/* this will trigger a update */
		glDeleteBuffers(1, &ch->chunk_vbo);
		glDeleteVertexArrays(1, &ch->chunk_vao);

		ch->chunk_vao = 0;
		ch->chunk_vbo = 0;
	}
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
