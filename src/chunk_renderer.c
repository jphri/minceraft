#include <pthread.h>
#include <math.h>
#include <assert.h>
#include <stb_image.h>

#include "global.h"
#include "linmath.h"
#include "util.h"
#include "glutil.h"
#include "chunk_renderer.h"
#include "world.h"

typedef struct {
	unsigned int texture;
	int w, h;
} Texture;

typedef struct {
	vec3 position;
	vec2 texcoord;
} Vertex;

typedef struct GraphicsChunk GraphicsChunk;
struct GraphicsChunk {
	int x, y, z;
	unsigned int chunk_vbo, chunk_vao;
	unsigned int vert_count;
	unsigned int water_vert_count;
	bool free, dirty;

	GraphicsChunk *next, *prev;

	enum {
		GSTATE_INIT,
		GSTATE_MESHING,
		GSTATE_MESHED,
		GSTATE_DONE
	} state;
	int xx, yy, zz;
};

typedef struct {
	enum {
		NEW_LOAD,
		TRY_LATER,
		FORCED,
	} mode;
	GraphicsChunk *chunk;
} ChunkFaceWork;

#define BLOCK_SCALE 1.0
#define WATER_OFFSET 0.1
#define MAX_CHUNKS 16384
#define MAX_WORK 16384

#define GCHUNK_SIZE_W 32
#define GCHUNK_SIZE_D 32
#define GCHUNK_SIZE_H 32

#define GBLOCK_MASK_X (GCHUNK_SIZE_W - 1)
#define GBLOCK_MASK_Y (GCHUNK_SIZE_H - 1)
#define GBLOCK_MASK_Z (GCHUNK_SIZE_D - 1)

#define GCHUNK_MASK_X ~GBLOCK_MASK_X
#define GCHUNK_MASK_Y ~GBLOCK_MASK_Y
#define GCHUNK_MASK_Z ~GBLOCK_MASK_Z


#ifndef M_PI
#define M_PI 3.1415926535
#endif

#ifndef M_PI_2
#define M_PI_2 (M_PI * 0.5)
#endif

static GraphicsChunk chunks[MAX_CHUNKS];
static GraphicsChunk *chunkmap[65536];
static int max_chunk_id;

static GraphicsChunk *find_or_allocate_chunk(int x, int y, int z);

static void insert_chunk(GraphicsChunk *chunk);
static void remove_chunk(GraphicsChunk *chunk);

static bool load_texture(Texture *texture, const char *path);
static void get_cube_face(Texture *texture, int tex_id, vec2 min, vec2 max);
static void chunk_generate_face(int x, int y, int z, Block block, Block face_blocks[6], ArrayBuffer *out);
static void chunk_generate_face_water(int x, int y, int z, Block block, Block face_blocks[6], ArrayBuffer *out);
static void chunk_generate_face_grass(int x, int y, int z, Block block, ArrayBuffer *buffer);
static void faces_worker_func(WorkGroup *wg);

static void load_programs();
static void load_buffers();
static void load_textures();
static void manhattan_load(int x, int y, int z, int r);

static int faces[BLOCK_LAST][6] = {
	[BLOCK_GRASS] = {
		2, 2, 2, 2, 0, 1,
	},
	[BLOCK_DIRT] = {
		0, 0, 0, 0, 0, 0
	},
	[BLOCK_STONE] = {
		3, 3, 3, 3, 3, 3
	},
	[BLOCK_SAND] = {
		4, 4, 4, 4, 4, 4
	},
	[BLOCK_PLANKS] = {
		5, 5, 5, 5, 5, 5
	},
	[BLOCK_GLASS] = {
		6, 6, 6, 6, 6, 6
	}, 
	[BLOCK_WATER] = {
		7, 7, 7, 7, 7, 7
	},
	[BLOCK_GRASS_BLADES] = {
		8, 8, 8, 8, 8, 8
	},
	[BLOCK_ROSE] = {
		9, 9, 9, 9, 9, 9
	},
	[BLOCK_WOOD] = {
		11, 11, 11, 11, 10, 10,
	},
	[BLOCK_LEAVES] = {
		12, 12, 12, 12, 12, 12
	}
};

static unsigned int chunk_program;
static unsigned int projection_uni, view_uni, terrain_uni, chunk_position_uni,
					alpha_uni;
static mat4x4 projection, view;
static Texture terrain;

static WorkGroup *facesg;
static WorkGroup *glbuffersg;

static pthread_mutex_t chunk_mutex;

static int chunk_x, chunk_y, chunk_z;
static int render_distance;

void
chunk_render_init()
{
	load_buffers();
	load_programs();
	load_textures();

	for(int i = 0; i < MAX_CHUNKS; i++) {
		chunks[i].free = true;
	}
	max_chunk_id = max_chunk_id + 1;

	pthread_mutex_init(&chunk_mutex, NULL);

	facesg = wg_init(faces_worker_func, sizeof(ChunkFaceWork), MAX_WORK, 6);
	glbuffersg = wg_init(NULL, sizeof(ChunkFaceWork), MAX_WORK, 0);
}

void
chunk_render_terminate()
{
	wg_terminate(facesg);
	wg_terminate(glbuffersg);
	glDeleteProgram(chunk_program);
	for(int i = 0; i < MAX_CHUNKS; i++) {
		if(glIsVertexArray(chunks[i].chunk_vao)) {
			glDeleteBuffers(1, &chunks[i].chunk_vbo);
			glDeleteVertexArrays(1, &chunks[i].chunk_vao);
		}
	}
}

void
chunk_render_set_camera(vec3 position, vec3 look_at, float aspect, float rdist)
{
	vec3 scene_center;
	
	vec3_add(scene_center, position, look_at);
	mat4x4_perspective(projection, M_PI_2, aspect, 0.001, 1000.0);
	mat4x4_look_at(view, position, scene_center, (vec3){ 0.0, 1.0, 0.0 });
	
	int nchunk_x = (int)floorf(position[0]) & GCHUNK_MASK_X;
	int nchunk_y = (int)floorf(position[1]) & GCHUNK_MASK_Y;
	int nchunk_z = (int)floorf(position[2]) & GCHUNK_MASK_Z;
	int nrend    = (int)floorf(rdist);

	if(nchunk_x != chunk_x || nchunk_y != chunk_y || nchunk_z != chunk_z || render_distance != nrend) {
		chunk_x = nchunk_x;
		chunk_y = nchunk_y;
		chunk_z = nchunk_z;
		render_distance = nrend;
	}
}

void
chunk_render_update()
{
	lock_gl_context();
	glUseProgram(chunk_program);
	glUniformMatrix4fv(projection_uni, 1, GL_FALSE, &projection[0][0]);
	glUniformMatrix4fv(view_uni, 1, GL_FALSE, &view[0][0]);
	glUseProgram(0);
	unlock_gl_context();
}

void
chunk_render_render_solid_chunk(GraphicsChunk *c)
{
	if(c->vert_count > 0) {
		glUniform3fv(chunk_position_uni, 1, (vec3){ c->x, c->y, c->z });
		glBindVertexArray(c->chunk_vao);
		glDrawArrays(GL_TRIANGLES, 0, c->vert_count);
	}
}

void
chunk_render_render_water_chunk(GraphicsChunk *c)
{
	if(c->water_vert_count > 0) {
		glUniform3fv(chunk_position_uni, 1, (vec3){ c->x, c->y, c->z });
		glBindVertexArray(c->chunk_vao);
		glDrawArrays(GL_TRIANGLES, c->vert_count, c->water_vert_count);
	}
}

bool
chunk_render_generate_faces(GraphicsChunk *chunk, ArrayBuffer *solid_faces, ArrayBuffer *water_faces)
{
	Block face_blocks[6];

	#define LOAD_BLOCK(BLOCK, X, Y, Z) if((BLOCK = world_get_block(X, Y, Z)) == BLOCK_UNLOADED) return false;
	
	switch(chunk->state) {
	case GSTATE_INIT:
		chunk->state = GSTATE_MESHING;
		for(chunk->zz = 0; chunk->zz < GCHUNK_SIZE_D; chunk->zz++)
			for(chunk->yy = 0; chunk->yy < GCHUNK_SIZE_H; chunk->yy++)
				for(chunk->xx = 0; chunk->xx < GCHUNK_SIZE_W; chunk->xx++) {
					Block block;
					int x, y, z;
					
	case GSTATE_MESHING:
					x = chunk->xx + chunk->x;
					y = chunk->yy + chunk->y;
					z = chunk->zz + chunk->z;

					LOAD_BLOCK(block, x, y, z);
					LOAD_BLOCK(face_blocks[TOP], x, y + 1, z);
					LOAD_BLOCK(face_blocks[BOTTOM], x, y - 1, z);
					LOAD_BLOCK(face_blocks[LEFT], x - 1, y, z);
					LOAD_BLOCK(face_blocks[RIGHT], x + 1, y, z);
					LOAD_BLOCK(face_blocks[FRONT], x, y, z + 1);
					LOAD_BLOCK(face_blocks[BACK], x, y, z - 1);
					
					switch(block) {
						case BLOCK_UNLOADED:
							return false;
						case 0:
							continue;
						case BLOCK_WATER:
							chunk_generate_face_water(chunk->xx, chunk->yy, chunk->zz, block, face_blocks, water_faces);
							break;
						case BLOCK_ROSE:
						case BLOCK_GRASS_BLADES:
							chunk_generate_face_grass(chunk->xx, chunk->yy, chunk->zz, block, solid_faces);
							break;
						default:
							chunk_generate_face(chunk->xx, chunk->yy, chunk->zz, block, face_blocks, solid_faces);
					}
				}
		chunk->state = GSTATE_MESHED;
	default:
		break;
	}

	return true;
}

void
chunk_render_generate_buffers(GraphicsChunk *chunk, ArrayBuffer *solid_faces, ArrayBuffer *water_faces)
{

	chunk->vert_count = arrbuf_length(solid_faces, sizeof(Vertex));
	chunk->water_vert_count = arrbuf_length(water_faces, sizeof(Vertex));

	size_t size = solid_faces->size + water_faces->size;

	glBindBuffer(GL_ARRAY_BUFFER, chunk->chunk_vbo);
	glBufferData(GL_ARRAY_BUFFER, size, NULL, GL_STATIC_DRAW);
	glBufferSubData(GL_ARRAY_BUFFER, 0, solid_faces->size, solid_faces->data);
	glBufferSubData(GL_ARRAY_BUFFER, solid_faces->size, water_faces->size, water_faces->data);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

bool
load_texture(Texture *texture, const char *path)
{
	void *image_data;

	image_data = stbi_load(path, &texture->w, &texture->h, NULL, 4);
	if(!image_data) {
		fprintf(stderr, "Cannot load '%s' as image.\n", path);
		return false;
	}

	glGenTextures(1, &texture->texture);
	glBindTexture(GL_TEXTURE_2D, texture->texture);
	glTexImage2D(GL_TEXTURE_2D,
			0,
			GL_RGBA,
			texture->w, texture->h,
			0,
			GL_RGBA,
			GL_UNSIGNED_BYTE,
			image_data);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glBindTexture(GL_TEXTURE_2D, 0);
	
	return true;
}

void
chunk_generate_face_grass(int x, int y, int z, Block block, ArrayBuffer *buffer)
{
	vec2 min, max;
	#define INSERT_VERTEX(...) \
		arrbuf_insert(buffer, sizeof(Vertex), &(Vertex){ __VA_ARGS__ })

	float xx = x * BLOCK_SCALE;
	float yy = y * BLOCK_SCALE;
	float zz = z * BLOCK_SCALE;

	get_cube_face(&terrain, block, min, max);
	INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
	INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], max[1] } );
	INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
	INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
	INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { min[0], min[1] } );
	INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
	
	INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
	INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], max[1] } );
	INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
	INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
	INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { min[0], min[1] } );
	INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
	
}

void
chunk_generate_face(int x, int y, int z, Block block, Block face_blocks[6], ArrayBuffer *buffer)
{
	vec2 min, max;
	#define INSERT_VERTEX(...) \
		arrbuf_insert(buffer, sizeof(Vertex), &(Vertex){ __VA_ARGS__ })

	#define BLOCK_AT(DIRECT) face_blocks[DIRECT]
	#define PROP_AT(DIRECT)  block_properties(face_blocks[DIRECT])

	float xx = x * BLOCK_SCALE;
	float yy = y * BLOCK_SCALE;
	float zz = z * BLOCK_SCALE;

	if(PROP_AT(BACK)->is_transparent) {
		get_cube_face(&terrain, faces[block][BACK], min, max);
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
	}

	if(PROP_AT(RIGHT)->is_transparent) {
		get_cube_face(&terrain, faces[block][RIGHT], min, max);
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  0 + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy,  BLOCK_SCALE + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { min[0], max[1] } );
	}

	if(PROP_AT(FRONT)->is_transparent) {
		get_cube_face(&terrain, faces[block][FRONT], min, max);
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy, BLOCK_SCALE + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy, BLOCK_SCALE + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy, BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy, BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy, BLOCK_SCALE + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy, BLOCK_SCALE + zz }, .texcoord = { min[0], max[1] } );
	}

	if(PROP_AT(LEFT)->is_transparent) {
		get_cube_face(&terrain, faces[block][LEFT], min, max);
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
	}

	if(PROP_AT(BOTTOM)->is_transparent) {
		get_cube_face(&terrain, faces[block][BOTTOM], min, max);
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  0 + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
	}

	if(PROP_AT(TOP)->is_transparent) {
		get_cube_face(&terrain, faces[block][TOP], min, max);
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy,  BLOCK_SCALE + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
	}

	#undef PROP_AT
}

void
chunk_generate_face_water(int x, int y, int z, Block block, Block face_blocks[6], ArrayBuffer *buffer)
{
	vec2 min, max;
	#define INSERT_VERTEX(...) \
		arrbuf_insert(buffer, sizeof(Vertex), &(Vertex){ __VA_ARGS__ })

	#define BLOCK_AT(DIRECT) face_blocks[DIRECT]
	#define PROP_AT(DIRECT)  block_properties(face_blocks[DIRECT])

	float xx = x * BLOCK_SCALE;
	float yy = y * BLOCK_SCALE;
	float zz = z * BLOCK_SCALE;

	if(PROP_AT(BACK)->is_transparent && BLOCK_AT(BACK) != BLOCK_WATER) {
		get_cube_face(&terrain, faces[block][BACK], min, max);
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  0 + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  0 + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  0 + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
	}

	if(PROP_AT(RIGHT)->is_transparent && BLOCK_AT(RIGHT) != BLOCK_WATER) {
		get_cube_face(&terrain, faces[block][RIGHT], min, max);
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  0 + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  0 + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  0 + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  BLOCK_SCALE + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { min[0], max[1] } );
	}

	if(PROP_AT(FRONT)->is_transparent && BLOCK_AT(FRONT) != BLOCK_WATER) {
		get_cube_face(&terrain, faces[block][FRONT], min, max);
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy, BLOCK_SCALE + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy, BLOCK_SCALE + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy - WATER_OFFSET, BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy - WATER_OFFSET, BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy - WATER_OFFSET, BLOCK_SCALE + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy, BLOCK_SCALE + zz }, .texcoord = { min[0], max[1] } );
	}

	if(PROP_AT(LEFT)->is_transparent && BLOCK_AT(LEFT) != BLOCK_WATER) {
		get_cube_face(&terrain, faces[block][LEFT], min, max);
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  0 + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
	}

	if(PROP_AT(BOTTOM)->is_transparent && BLOCK_AT(BOTTOM) != BLOCK_WATER) {
		get_cube_face(&terrain, faces[block][BOTTOM], min, max);
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  0 + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
	}

	if(BLOCK_AT(TOP) != BLOCK_WATER) {
		get_cube_face(&terrain, faces[block][TOP], min, max);
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  0 + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  0 + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  BLOCK_SCALE + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  0 + zz }, .texcoord = { min[0], max[1] } );
	}
}


void
load_programs()
{
	chunk_program = glCreateProgram();

	unsigned int chunk_vertex = ugl_compile_shader_file("shaders/chunk.vsh", GL_VERTEX_SHADER);
	unsigned int chunk_fragment = ugl_compile_shader_file("shaders/chunk.fsh", GL_FRAGMENT_SHADER);

	ugl_link_program(chunk_program, "chunk_program", 2, (unsigned int[]){
		chunk_vertex,
		chunk_fragment
	});
	glDeleteShader(chunk_vertex);
	glDeleteShader(chunk_fragment);
	
	projection_uni     = glGetUniformLocation(chunk_program, "u_Projection");
	view_uni           = glGetUniformLocation(chunk_program, "u_View");
	terrain_uni        = glGetUniformLocation(chunk_program, "u_Terrain");
	chunk_position_uni = glGetUniformLocation(chunk_program, "u_ChunkPosition");
	alpha_uni          = glGetUniformLocation(chunk_program, "u_Alpha");
	UGL_ASSERT();
}

void
load_buffers()
{
	for(int i = 0; i < MAX_CHUNKS; i++) {
		GraphicsChunk *chunk = chunks + i;

		glGenBuffers(1, &chunk->chunk_vbo);
		chunk->chunk_vao = ugl_create_vao(2, (VaoSpec[]){
			{ 0, 3, GL_FLOAT, sizeof(Vertex), offsetof(Vertex, position), 0, chunk->chunk_vbo },
			{ 1, 2, GL_FLOAT, sizeof(Vertex), offsetof(Vertex, texcoord), 0, chunk->chunk_vbo },
		});
	}
	UGL_ASSERT();
}


void 
get_cube_face(Texture *texture, int tex_id, vec2 min, vec2 max)
{
	max[0] = 16.0 / texture->w;
	max[1] = 16.0 / texture->h;
	div_t d = div(tex_id, texture->w);
	
	min[0] = max[0] * d.rem;
	min[1] = max[1] * d.quot;
	vec2_add(max, max, min);
}

void
load_textures()
{
	assert(load_texture(&terrain, "textures/terrain.png"));
}

void
chunk_render()
{
	chunk_render_update();
	lock_gl_context();
	glUseProgram(chunk_program);
	glUniform1f(alpha_uni, 1.0);
	
	glEnable(GL_CULL_FACE);
	glEnable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, terrain.texture);

	int rdist_x = render_distance & GCHUNK_MASK_X;
	int rdist_y = render_distance & GCHUNK_MASK_Y;
	int rdist_z = render_distance & GCHUNK_MASK_Z;

	for(int i = 0; i <= render_distance; i += GCHUNK_SIZE_W) {
		manhattan_load(chunk_x, chunk_y, chunk_z, i);
	}

	glUniform1f(alpha_uni, 0.9);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	for(int zz = -rdist_z; zz < rdist_z; zz += GCHUNK_SIZE_D)
	for(int yy = -rdist_y; yy < rdist_y; yy += GCHUNK_SIZE_H)
	for(int xx = -rdist_x; xx < rdist_x; xx += GCHUNK_SIZE_W) {
		GraphicsChunk *c = find_or_allocate_chunk(xx + chunk_x, yy + chunk_y, zz + chunk_z);
		if(c && c->state == GSTATE_DONE) {
			chunk_render_render_water_chunk(c);
		}
	}

	glBindTexture(GL_TEXTURE_2D, 0);
	glUseProgram(0);
	unlock_gl_context();
}

void
faces_worker_func(WorkGroup *wg)
{
	ChunkFaceWork w;
	ArrayBuffer solid_faces, water_faces;
	GraphicsChunk *chunk;

	arrbuf_init(&solid_faces);
	arrbuf_init(&water_faces);
	while(wg_recv(wg, &w)) {
		if(!w.chunk)
			continue;

		chunk = w.chunk;
		chunk->state = GSTATE_INIT;
		arrbuf_clear(&solid_faces);
		arrbuf_clear(&water_faces);
		if(!chunk_render_generate_faces(w.chunk, &solid_faces, &water_faces)) {
			if(world_can_load(chunk->x, chunk->y, chunk->z)) {
				w.mode = TRY_LATER;
				w.chunk = chunk;
				wg_send(facesg, &w);
			}
			continue;
		}
		
		lock_gl_context();
		chunk_render_generate_buffers(chunk, &solid_faces, &water_faces);
		unlock_gl_context();

		chunk->state = GSTATE_DONE;
	}
	arrbuf_free(&solid_faces);
	arrbuf_free(&water_faces);
}

GraphicsChunk *
find_or_allocate_chunk(int x, int y, int z)
{
	/* 
		this reduces the amount of iterations to one,
		as before i would require to look up for all chunks
		first to check if the chunk exists, 
		then start over and look for a free chunk
	*/

	pthread_mutex_lock(&chunk_mutex);
	uint32_t h = chunk_coord_hash(x, y, z);
	GraphicsChunk *c = chunkmap[h];
	while(c) {
		if(c->x == x && c->y == y && c->z == z) {
			pthread_mutex_unlock(&chunk_mutex);
			return c;
		}
		c = c->next;
	}

	GraphicsChunk *free_chunk = NULL;
	c = chunks;
	for(; c < chunks + max_chunk_id + 1; c++) {
		if(c->free) {
			free_chunk = c;
			break;
		} else {
			/* if it is too far way, treat as a freed too */
			int dx = abs(c->x - chunk_x);
			int dy = abs(c->y - chunk_y);
			int dz = abs(c->z - chunk_z);
			if(dx > render_distance || dy > render_distance || dz > render_distance) {
				remove_chunk(c);
				free_chunk = c;
				break;
			}
		}
	}
	if(!free_chunk) {
		if(c >= chunks + MAX_CHUNKS) {
			pthread_mutex_unlock(&chunk_mutex);	
			return NULL;
		}
		free_chunk = c;
		max_chunk_id ++;
	}
	free_chunk->x = x;
	free_chunk->y = y;
	free_chunk->z = z;
	free_chunk->free = false;
	free_chunk->dirty = true;
	insert_chunk(c);
	pthread_mutex_unlock(&chunk_mutex);

	return free_chunk;
}

void
insert_chunk(GraphicsChunk *c)
{
	uint32_t hash = chunk_coord_hash(c->x, c->y, c->z);
	c->prev = NULL;
	c->next = chunkmap[hash];
	if(chunkmap[hash])
		chunkmap[hash]->prev = c;
	chunkmap[hash] = c;
}

void
remove_chunk(GraphicsChunk *c)
{
	uint32_t hash = chunk_coord_hash(c->x, c->y, c->z);
	if(c->prev)
		c->prev->next = c->next;
	if(c->next)
		c->next->prev = c->prev;
	if(chunkmap[hash] == c)
		chunkmap[hash] = c->next;
}

void
chunk_render_request_update_block(int x, int y, int z)
{
	GraphicsChunk *c = find_or_allocate_chunk(x, y, z);
	c->dirty = true;
}

void
manhattan_load(int x, int y, int z, int r)
{
	for(int xx = -r; xx <= r; xx += GCHUNK_SIZE_W)
	for(int yy = -r; yy <= r; yy += GCHUNK_SIZE_H)
	for(int zz = -r; zz <= r; zz += GCHUNK_SIZE_D) {
		if(abs(xx) + abs(yy) + abs(zz) != r)
			continue;

		GraphicsChunk *c = find_or_allocate_chunk(xx + x, yy + y, zz + z);
		if(c->dirty) {
			c->dirty = false;
			c->state = GSTATE_INIT;
			wg_send(facesg, &(ChunkFaceWork) {
				.chunk = c,
				.mode = FORCED
			});
		}

		if(c && c->state == GSTATE_DONE) {
			chunk_render_render_solid_chunk(c);
		}
	}
}

