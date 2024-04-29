#include <math.h>
#include <assert.h>
#include <stb_image.h>

#include "linmath.h"
#include "util.h"
#include "glutil.h"
#include "chunk_renderer.h"

typedef struct {
	unsigned int texture;
	int w, h;
} Texture;

typedef struct {
	vec3 position;
	vec2 texcoord;
} Vertex;

#define BLOCK_SCALE 1.0
#define WATER_OFFSET 0.1


#ifndef M_PI
#define M_PI 3.1415926535
#endif

#ifndef M_PI_2
#define M_PI_2 (M_PI * 0.5)
#endif


static bool load_texture(Texture *texture, const char *path);
static void get_cube_face(Texture *texture, int tex_id, vec2 min, vec2 max);
static void chunk_generate_face(Chunk *chunk, int x, int y, int z, ArrayBuffer *out);
static void chunk_generate_face_water(Chunk *chunk, int x, int y, int z, ArrayBuffer *out);

static void load_programs();
static void load_buffers();
static void load_textures();

static Vertex quad_data[] = {
	{ { -1.0, -1.0,  0.0 }, { 0.0, 0.0 } },
	{ {  1.0, -1.0,  0.0 }, { 1.0, 0.0 } },
	{ {  1.0,  1.0,  0.0 }, { 1.0, 1.0 } },

	{ {  1.0,  1.0,  0.0 }, { 1.0, 1.0 } },
	{ { -1.0,  1.0,  0.0 }, { 0.0, 1.0 } },
	{ { -1.0, -1.0,  0.0 }, { 0.0, 0.0 } },
};

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
	}
};

static unsigned int chunk_program;
static unsigned int projection_uni, view_uni, terrain_uni, chunk_position_uni,
					alpha_uni;
static unsigned int quad_buffer, quad_vao;
static mat4x4 projection, view;
static Texture terrain;

void
chunk_render_init()
{
	load_buffers();
	load_programs();
	load_textures();
}

void
chunk_render_terminate()
{
	glDeleteProgram(chunk_program);
}

void
chunk_render_set_camera(vec3 position, vec3 look_at, float aspect)
{
	vec3 scene_center;
	
	vec3_add(scene_center, position, look_at);

	mat4x4_perspective(projection, M_PI_2, aspect, 0.001, 120.0);
	mat4x4_look_at(view, position, scene_center, (vec3){ 0.0, 1.0, 0.0 });
}

void
chunk_render_update()
{
	glUseProgram(chunk_program);
	glUniformMatrix4fv(projection_uni, 1, GL_FALSE, &projection[0][0]);
	glUniformMatrix4fv(view_uni, 1, GL_FALSE, &view[0][0]);
	glUseProgram(0);
}

void
chunk_render_render_solid_chunk(Chunk *c)
{
	glUseProgram(chunk_program);
	glUniform3fv(chunk_position_uni, 1, (vec3){ c->x, c->y, c->z });
	glUniform1f(alpha_uni, 1.0);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, terrain.texture);

	glEnable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);

	if(c->vert_count > 0) {
		glBindVertexArray(c->chunk_vao);
		glDrawArrays(GL_TRIANGLES, 0, c->vert_count);
	}

	glBindTexture(GL_TEXTURE_2D, 0);
	glUseProgram(0);
}

void
chunk_render_render_water_chunk(Chunk *c)
{
	glUseProgram(chunk_program);
	glUniform3fv(chunk_position_uni, 1, (vec3){ c->x, c->y, c->z });
	glUniform1f(alpha_uni, 1.0);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, terrain.texture);

	glUniform1f(alpha_uni, 0.9);

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	if(c->water_vert_count > 0) {
		glBindVertexArray(c->water_vao);
		glDrawArrays(GL_TRIANGLES, 0, c->water_vert_count);
	
	}

	glBindTexture(GL_TEXTURE_2D, 0);
	glUseProgram(0);
}

void
chunk_render_generate_buffers(Chunk *chunk)
{
	ArrayBuffer buffer;
	ArrayBuffer water_buffer;

	arrbuf_init(&buffer);
	arrbuf_init(&water_buffer);

	for(int z = 0; z < CHUNK_SIZE; z++)
		for(int y = 0; y < CHUNK_SIZE; y++)
			for(int x = 0; x < CHUNK_SIZE; x++) {
				Block b = chunk->blocks[z][y][x];
				if(b <= 0)
					continue;

				switch(chunk->blocks[z][y][x]) {
				case BLOCK_WATER:
					chunk_generate_face_water(chunk, x, y, z, &water_buffer);
					break;
				default:
					chunk_generate_face(chunk, x, y, z, &buffer);
				}
			}
	chunk->vert_count = arrbuf_length(&buffer, sizeof(Vertex));
	chunk->water_vert_count = arrbuf_length(&water_buffer, sizeof(Vertex));

	chunk->chunk_vbo = ugl_create_buffer(GL_STATIC_DRAW, buffer.size, buffer.data);
	chunk->chunk_vao = ugl_create_vao(2, (VaoSpec[]){
		{ 0, 3, GL_FLOAT, sizeof(Vertex), offsetof(Vertex, position), 0, chunk->chunk_vbo },
		{ 1, 2, GL_FLOAT, sizeof(Vertex), offsetof(Vertex, texcoord), 0, chunk->chunk_vbo },
	});

	chunk->water_vbo = ugl_create_buffer(GL_STATIC_DRAW, water_buffer.size, water_buffer.data);
	chunk->water_vao = ugl_create_vao(2, (VaoSpec[]){
		{ 0, 3, GL_FLOAT, sizeof(Vertex), offsetof(Vertex, position), 0, chunk->water_vbo },
		{ 1, 2, GL_FLOAT, sizeof(Vertex), offsetof(Vertex, texcoord), 0, chunk->water_vbo },
	});

	arrbuf_free(&buffer);
	arrbuf_free(&water_buffer);
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
chunk_generate_face(Chunk *chunk, int x, int y, int z, ArrayBuffer *buffer)
{
	vec2 min, max;
	int block = chunk->blocks[z][y][x];
	#define INSERT_VERTEX(...) \
		arrbuf_insert(buffer, sizeof(Vertex), &(Vertex){ __VA_ARGS__ })

	float xx = x * BLOCK_SCALE;
	float yy = y * BLOCK_SCALE;
	float zz = z * BLOCK_SCALE;

	if(z == 0 || block_properties(chunk->blocks[z - 1][y][x])->is_transparent) {
		get_cube_face(&terrain, faces[block][BACK], min, max);
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
	}

	if(x == LAST_BLOCK || block_properties(chunk->blocks[z][y][x + 1])->is_transparent) {
		get_cube_face(&terrain, faces[block][RIGHT], min, max);
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  0 + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy,  BLOCK_SCALE + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { min[0], max[1] } );
	}

	if(z == LAST_BLOCK || block_properties(chunk->blocks[z + 1][y][x])->is_transparent) {
		get_cube_face(&terrain, faces[block][FRONT], min, max);
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy, BLOCK_SCALE + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy, BLOCK_SCALE + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy, BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy, BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy, BLOCK_SCALE + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy, BLOCK_SCALE + zz }, .texcoord = { min[0], max[1] } );
	}

	if(x == 0 || block_properties(chunk->blocks[z][y][x - 1])->is_transparent) {
		get_cube_face(&terrain, faces[block][LEFT], min, max);
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
	}

	if(y == 0 || block_properties(chunk->blocks[z][y - 1][x])->is_transparent) {
		get_cube_face(&terrain, faces[block][BOTTOM], min, max);
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  0 + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
	}

	if(y == LAST_BLOCK || block_properties(chunk->blocks[z][y + 1][x])->is_transparent) {
		get_cube_face(&terrain, faces[block][TOP], min, max);
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy,  BLOCK_SCALE + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
	}
}

void
chunk_generate_face_water(Chunk *chunk, int x, int y, int z, ArrayBuffer *buffer)
{
	vec2 min, max;
	int block = chunk->blocks[z][y][x];
	#define INSERT_VERTEX(...) \
		arrbuf_insert(buffer, sizeof(Vertex), &(Vertex){ __VA_ARGS__ })

	float xx = x * BLOCK_SCALE;
	float yy = y * BLOCK_SCALE;
	float zz = z * BLOCK_SCALE;

	if(z == 0 || block_properties(chunk->blocks[z - 1][y][x])->is_transparent) {
		get_cube_face(&terrain, faces[block][BACK], min, max);
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  0 + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  0 + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  0 + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
	}

	if(x == LAST_BLOCK || block_properties(chunk->blocks[z][y][x + 1])->is_transparent) {
		get_cube_face(&terrain, faces[block][RIGHT], min, max);
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  0 + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  0 + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  0 + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  BLOCK_SCALE + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { min[0], max[1] } );
	}

	if(z == LAST_BLOCK || block_properties(chunk->blocks[z + 1][y][x])->is_transparent) {
		get_cube_face(&terrain, faces[block][FRONT], min, max);
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy, BLOCK_SCALE + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy, BLOCK_SCALE + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy - WATER_OFFSET, BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy - WATER_OFFSET, BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy - WATER_OFFSET, BLOCK_SCALE + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy, BLOCK_SCALE + zz }, .texcoord = { min[0], max[1] } );
	}

	if(x == 0 || block_properties(chunk->blocks[z][y][x - 1])->is_transparent) {
		get_cube_face(&terrain, faces[block][LEFT], min, max);
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  0 + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
	}

	if(y == 0 || block_properties(chunk->blocks[z][y - 1][x])->is_transparent) {
		get_cube_face(&terrain, faces[block][BOTTOM], min, max);
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  0 + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
	}

	//if(y == LAST_BLOCK || block_properties(chunk->blocks[z][y + 1][x])->is_transparent) {
		get_cube_face(&terrain, faces[block][TOP], min, max);
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  0 + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  0 + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  BLOCK_SCALE + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy - WATER_OFFSET,  0 + zz }, .texcoord = { min[0], max[1] } );
	//}
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
	quad_buffer = ugl_create_buffer(GL_STATIC_DRAW, sizeof(quad_data), quad_data);
	quad_vao = ugl_create_vao(2, (VaoSpec[]){
		{ 0, 3, GL_FLOAT, sizeof(Vertex), offsetof(Vertex, position), 0, quad_buffer },
		{ 1, 2, GL_FLOAT, sizeof(Vertex), offsetof(Vertex, texcoord), 0, quad_buffer },
	});
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