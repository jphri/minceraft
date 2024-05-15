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
	unsigned int water_vbo, water_vao;
	unsigned int water_vert_count;
	bool free;

	GraphicsChunk *next, *prev;
};

typedef struct {
	GraphicsChunk *chunk;
	ArrayBuffer solid_faces;
	ArrayBuffer water_faces;
	int x, y, z;

	enum {
		NEW_LOAD,
		FORCED,
	} mode;
} ChunkFaceWork;

#define BLOCK_SCALE 1.0
#define WATER_OFFSET 0.1
#define MAX_CHUNKS 8192
#define MAX_WORK 1024
#define GCHUNK_SIZE 64
#define GBLOCK_MASK  (GCHUNK_SIZE - 1)
#define GCHUNK_MASK ~GBLOCK_MASK

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
static GraphicsChunk *allocate_chunk_except(int x, int y, int z);
static GraphicsChunk *find_chunk(int x, int y, int z);

static void insert_chunk(GraphicsChunk *chunk);
static void remove_chunk(GraphicsChunk *chunk);

static bool load_texture(Texture *texture, const char *path);
static void get_cube_face(Texture *texture, int tex_id, vec2 min, vec2 max);
static void chunk_generate_face(GraphicsChunk *chunk, int x, int y, int z, ArrayBuffer *out);
static void chunk_generate_face_water(GraphicsChunk *chunk, int x, int y, int z, ArrayBuffer *out);
static void chunk_generate_face_grass(GraphicsChunk *chunk, int x, int y, int z, ArrayBuffer *buffer);

static void faces_worker_func(WorkGroup *wg);

static void load_programs();
static void load_buffers();
static void load_textures();

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

	int nchunk_x = (int)floorf(position[0]) & GCHUNK_MASK;
	int nchunk_y = (int)floorf(position[1]) & GCHUNK_MASK;
	int nchunk_z = (int)floorf(position[2]) & GCHUNK_MASK;
	int nrend    = (int)floorf(rdist) & GCHUNK_MASK;

	if(nchunk_x != chunk_x || nchunk_y != chunk_y || nchunk_z != chunk_z || render_distance != nrend) {
		chunk_x = nchunk_x;
		chunk_y = nchunk_y;
		chunk_z = nchunk_z;
		render_distance = nrend;

		for(int x = -render_distance; x <= render_distance; x += GCHUNK_SIZE)
		for(int y = -render_distance; y <= render_distance; y += GCHUNK_SIZE)
		for(int z = -render_distance; z <= render_distance; z += GCHUNK_SIZE) {
			
			wg_send(facesg, &(ChunkFaceWork){
				.x = (nchunk_x + x),
				.y = (nchunk_y + y),
				.z = (nchunk_z + z),
				.mode = NEW_LOAD
			});
		}	
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
		lock_gl_context();
		glUseProgram(chunk_program);
		glUniform3fv(chunk_position_uni, 1, (vec3){ c->x, c->y, c->z });
		glUniform1f(alpha_uni, 1.0);

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, terrain.texture);

		glEnable(GL_DEPTH_TEST);
		glDisable(GL_BLEND);

			glBindVertexArray(c->chunk_vao);
			glDrawArrays(GL_TRIANGLES, 0, c->vert_count);
		

		glBindTexture(GL_TEXTURE_2D, 0);
		glUseProgram(0);
		unlock_gl_context();
	}
}

void
chunk_render_render_water_chunk(GraphicsChunk *c)
{
	if(c->water_vert_count > 0) {
		lock_gl_context();
		glUseProgram(chunk_program);
		glUniform3fv(chunk_position_uni, 1, (vec3){ c->x, c->y, c->z });
		glUniform1f(alpha_uni, 1.0);

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, terrain.texture);

		glUniform1f(alpha_uni, 0.9);

		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

			glBindVertexArray(c->water_vao);
			glDrawArrays(GL_TRIANGLES, 0, c->water_vert_count);
		

		glBindTexture(GL_TEXTURE_2D, 0);
		glUseProgram(0);
		unlock_gl_context();
	}
}

void
chunk_render_generate_faces(GraphicsChunk *chunk, ChunkFaceWork *w)
{
	for(int z = 0; z < GCHUNK_SIZE; z++)
		for(int y = 0; y < GCHUNK_SIZE; y++)
			for(int x = 0; x < GCHUNK_SIZE; x++) {
				Block block = world_get_block(x + chunk->x, y + chunk->y, z + chunk->z);
				if(block <= 0)
					continue;
				switch(block) {
				case BLOCK_WATER:
					chunk_generate_face_water(chunk, x, y, z, &w->water_faces);
					break;
				case BLOCK_ROSE:
				case BLOCK_GRASS_BLADES:
					chunk_generate_face_grass(chunk, x, y, z, &w->solid_faces);
					break;
				default:
					chunk_generate_face(chunk, x, y, z, &w->solid_faces);
				}
			}
}

void
chunk_render_generate_buffers(ChunkFaceWork *w)
{
	GraphicsChunk *chunk = w->chunk;

	chunk->vert_count = arrbuf_length(&w->solid_faces, sizeof(Vertex));
	chunk->water_vert_count = arrbuf_length(&w->water_faces, sizeof(Vertex));

	glBindBuffer(GL_ARRAY_BUFFER, chunk->chunk_vbo);
	glBufferData(GL_ARRAY_BUFFER, w->solid_faces.size, w->solid_faces.data, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, chunk->water_vbo);
	glBufferData(GL_ARRAY_BUFFER, w->water_faces.size, w->water_faces.data, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	printf("Chunk generated for: %d %d %d\n", w->chunk->x, w->chunk->y, w->chunk->z);
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
chunk_generate_face_grass(GraphicsChunk *chunk, int x, int y, int z, ArrayBuffer *buffer)
{
	vec2 min, max;
	#define INSERT_VERTEX(...) \
		arrbuf_insert(buffer, sizeof(Vertex), &(Vertex){ __VA_ARGS__ })

	Block block = world_get_block(x + chunk->x, y + chunk->y, z + chunk->z);

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
chunk_generate_face(GraphicsChunk *chunk, int x, int y, int z, ArrayBuffer *buffer)
{
	vec2 min, max;
	int block = world_get_block(x + chunk->x, y + chunk->y, z + chunk->z);
	#define INSERT_VERTEX(...) \
		arrbuf_insert(buffer, sizeof(Vertex), &(Vertex){ __VA_ARGS__ })

	#define PROP_AT(X, Y, Z) block_properties(world_get_block(X + chunk->x, Y + chunk->y, Z + chunk->z))

	float xx = x * BLOCK_SCALE;
	float yy = y * BLOCK_SCALE;
	float zz = z * BLOCK_SCALE;

	if(PROP_AT(x, y, z - 1)->is_transparent) {
		get_cube_face(&terrain, faces[block][BACK], min, max);
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
	}

	if(PROP_AT(x + 1, y, z)->is_transparent) {
		get_cube_face(&terrain, faces[block][RIGHT], min, max);
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  0 + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy,  BLOCK_SCALE + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { min[0], max[1] } );
	}

	if(PROP_AT(x, y, z + 1)->is_transparent) {
		get_cube_face(&terrain, faces[block][FRONT], min, max);
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy, BLOCK_SCALE + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy, BLOCK_SCALE + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy, BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy, BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy, BLOCK_SCALE + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy, BLOCK_SCALE + zz }, .texcoord = { min[0], max[1] } );
	}

	if(PROP_AT(x - 1, y, z)->is_transparent) {
		get_cube_face(&terrain, faces[block][LEFT], min, max);
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
	}

	if(PROP_AT(x, y - 1, z)->is_transparent) {
		get_cube_face(&terrain, faces[block][BOTTOM], min, max);
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  0 + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
	}

	if(PROP_AT(x, y + 1, z)->is_transparent) {
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
chunk_generate_face_water(GraphicsChunk *chunk, int x, int y, int z, ArrayBuffer *buffer)
{
	vec2 min, max;
	int block = world_get_block(x + chunk->x, y + chunk->y, z + chunk->z);
	#define INSERT_VERTEX(...) \
		arrbuf_insert(buffer, sizeof(Vertex), &(Vertex){ __VA_ARGS__ })

	#define PROP_AT(X, Y, Z) block_properties(world_get_block(X + chunk->x, Y + chunk->y, Z + chunk->z))
	#define BLOCK_AT(X, Y, Z) world_get_block(X + chunk->x, Y + chunk->y, Z + chunk->z)

	float xx = x * BLOCK_SCALE;
	float yy = y * BLOCK_SCALE;
	float zz = z * BLOCK_SCALE;

	if(PROP_AT(x, y, z - 1)->is_transparent && BLOCK_AT(x, y, z - 1) != BLOCK_WATER) {
		get_cube_face(&terrain, faces[block][BACK], min, max);
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  0 + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  0 + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  0 + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
	}

	if(PROP_AT(x + 1, y, z)->is_transparent && BLOCK_AT(x + 1, y, z) != BLOCK_WATER) {
		get_cube_face(&terrain, faces[block][RIGHT], min, max);
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  0 + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  0 + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  0 + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  BLOCK_SCALE + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { min[0], max[1] } );
	}

	if(PROP_AT(x, y, z + 1)->is_transparent && BLOCK_AT(x, y, z + 1) != BLOCK_WATER) {
		get_cube_face(&terrain, faces[block][FRONT], min, max);
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy, BLOCK_SCALE + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy, BLOCK_SCALE + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy - WATER_OFFSET, BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy - WATER_OFFSET, BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy - WATER_OFFSET, BLOCK_SCALE + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy, BLOCK_SCALE + zz }, .texcoord = { min[0], max[1] } );
	}

	if(PROP_AT(x - 1, y, z)->is_transparent && BLOCK_AT(x - 1, y, z) != BLOCK_WATER) {
		get_cube_face(&terrain, faces[block][LEFT], min, max);
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  0 + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
	}

	if(PROP_AT(x, y - 1, z)->is_transparent && BLOCK_AT(x, y - 1, z) != BLOCK_WATER) {
		get_cube_face(&terrain, faces[block][BOTTOM], min, max);
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  0 + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
	}

	if(BLOCK_AT(x, y + 1, z) != BLOCK_WATER) {
		get_cube_face(&terrain, faces[block][TOP], min, max);
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  0 + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  0 + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  BLOCK_SCALE + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  0 + zz }, .texcoord = { min[0], max[1] } );
	}

	#undef PROP_AT
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

		glGenBuffers(1, &chunk->water_vbo);
		chunk->water_vao = ugl_create_vao(2, (VaoSpec[]){
			{ 0, 3, GL_FLOAT, sizeof(Vertex), offsetof(Vertex, position), 0, chunk->water_vbo },
			{ 1, 2, GL_FLOAT, sizeof(Vertex), offsetof(Vertex, texcoord), 0, chunk->water_vbo },
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
	ChunkFaceWork w;
	while(wg_recv_nonblock(glbuffersg, &w)) {
		chunk_render_generate_buffers(&w);
		arrbuf_free(&w.solid_faces);
		arrbuf_free(&w.water_faces);
	}

	chunk_render_update();
	for(int zz = -render_distance; zz < render_distance; zz += GCHUNK_SIZE)
	for(int yy = -render_distance; yy < render_distance; yy += GCHUNK_SIZE)
	for(int xx = -render_distance; xx < render_distance; xx += GCHUNK_SIZE) {
		GraphicsChunk *c = find_chunk(xx + chunk_x, yy + chunk_y, zz + chunk_z);
		if(c) {
			chunk_render_render_solid_chunk(c);
		}
	}
	
	for(int zz = -render_distance; zz < render_distance; zz += GCHUNK_SIZE)
	for(int yy = -render_distance; yy < render_distance; yy += GCHUNK_SIZE)
	for(int xx = -render_distance; xx < render_distance; xx += GCHUNK_SIZE) {
		GraphicsChunk *c = find_chunk(xx + chunk_x, yy + chunk_y, zz + chunk_z);
		if(c) {
			chunk_render_render_water_chunk(c);
		}
	}
}

void
faces_worker_func(WorkGroup *wg)
{
	ChunkFaceWork w;
	while(wg_recv(wg, &w)) {
		switch(w.mode) {
		case NEW_LOAD:
			w.chunk = allocate_chunk_except(w.x, w.y, w.z);
			if(w.chunk) {
				w.chunk->water_vert_count = 0;
				w.chunk->vert_count = 0;
			}
			break;
		case FORCED:
			w.chunk = find_or_allocate_chunk(w.x, w.y, w.z);
			break;
		}
		
		if(!w.chunk)
			continue;
		arrbuf_init(&w.solid_faces);
		arrbuf_init(&w.water_faces);
		chunk_render_generate_faces(w.chunk, &w);
		
		lock_gl_context();
		chunk_render_generate_buffers(&w);
		unlock_gl_context();

		printf("Size: %f MBs...\n", (double)w.solid_faces.size / (1024 * 1024));

		arrbuf_free(&w.solid_faces);
		arrbuf_free(&w.water_faces);
	}
}

GraphicsChunk *
allocate_chunk_except(int x, int y, int z)
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
			return NULL;
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
	insert_chunk(free_chunk);
	pthread_mutex_unlock(&chunk_mutex);

	return free_chunk;
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
	insert_chunk(c);
	pthread_mutex_unlock(&chunk_mutex);

	return free_chunk;
}

void
chunk_render_request_update_block(int x, int y, int z)
{
	int chunk_x = x & GCHUNK_MASK;
	int chunk_y = y & GCHUNK_MASK;
	int chunk_z = z & GCHUNK_MASK;

	int block_x = x & GBLOCK_MASK;
	int block_y = y & GBLOCK_MASK;
	int block_z = z & GBLOCK_MASK;

	wg_send(facesg, &(ChunkFaceWork){
		.x = chunk_x,
		.y = chunk_y,
		.z = chunk_z,
		.mode = FORCED
	});

	if(block_x == 0) {
		wg_send(facesg, &(ChunkFaceWork){
			.x = chunk_x - GCHUNK_SIZE,
			.y = chunk_y, 
			.z = chunk_z,
			.mode = FORCED
		});
	}

	if(block_x == (GCHUNK_SIZE - 1)) {
		wg_send(facesg, &(ChunkFaceWork){
			.x = chunk_x + GCHUNK_SIZE,
			.y = chunk_y, 
			.z = chunk_z,
			.mode = FORCED
		});
	}

	if(block_y == 0) {
		wg_send(facesg, &(ChunkFaceWork){
			.x = chunk_x,
			.y = chunk_y - GCHUNK_SIZE, 
			.z = chunk_z,
			.mode = FORCED
		});
	}

	if(block_y == (GCHUNK_SIZE - 1)) {
		wg_send(facesg, &(ChunkFaceWork){
			.x = chunk_x,
			.y = chunk_y + GCHUNK_SIZE, 
			.z = chunk_z,
			.mode = FORCED
		});
	}

	if(block_z == 0) {
		wg_send(facesg, &(ChunkFaceWork){
			.x = chunk_x,
			.y = chunk_y, 
			.z = chunk_z - GCHUNK_SIZE,
			.mode = FORCED
		});
	}

	if(block_z == (GCHUNK_SIZE - 1)) {
		wg_send(facesg, &(ChunkFaceWork){
			.x = chunk_x,
			.y = chunk_y, 
			.z = chunk_z + GCHUNK_SIZE,
			.mode = FORCED
		});
	}
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

GraphicsChunk *
find_chunk(int x, int y, int z)
{
	uint32_t h = chunk_coord_hash(x, y, z);
	pthread_mutex_lock(&chunk_mutex);
	GraphicsChunk *c = chunkmap[h];
	while(c) {
		if(!c->free && c->x == x && c->y == y && c->z == z) {
			pthread_mutex_unlock(&chunk_mutex);
			return c;
		}
		c = c->next;
	}
	pthread_mutex_unlock(&chunk_mutex);

	return NULL;
}
