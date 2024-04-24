#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <assert.h>

#include "cubegame.h"
#include "util.h"
#include "glutil.h"
#include "linmath.h"
#include "world.h"

#include "stb_image.h"

#ifndef M_PI
#define M_PI 3.1415926535
#endif

#ifndef M_PI_2
#define M_PI_2 (M_PI * 0.5)
#endif

#define EPSLON 0.00001
#define MAX_PITCH (M_PI_2 - EPSLON)
#define PLAYER_SPEED 4.0

#define CHUNK_SIZE 16
#define LAST_BLOCK (CHUNK_SIZE - 1)
#define BLOCK_SCALE 1.0
typedef struct {
	vec3 position;
	vec2 texcoord;
} Vertex;

typedef struct {
	vec3 position;
	float pitch, yaw;
	vec3 camera_view;
} Player;

typedef struct {
	unsigned int texture;
	int w, h;
} Texture;

static bool locking;

static void chunk_generate_face(Chunk *chunk, int x, int y, int z, ArrayBuffer *output);
static void player_update(Player *player, float delta);
static void get_cube_face(Texture *texture, int tex_id, vec2 min, vec2 max);

static bool load_texture(Texture *texture, const char *path);

static void load_programs();
static void load_buffers();
static void load_textures();

static void error_callback(int errcode, const char *msg);
static void mouse_click_callback(GLFWwindow *window, int button, int action, int mods);
static void keyboard_callback(GLFWwindow *window, int scan, int key, int action, int mods);

static Vertex quad_data[] = {
	{ { -1.0, -1.0,  0.0 }, { 0.0, 0.0 } },
	{ {  1.0, -1.0,  0.0 }, { 1.0, 0.0 } },
	{ {  1.0,  1.0,  0.0 }, { 1.0, 1.0 } },

	{ {  1.0,  1.0,  0.0 }, { 1.0, 1.0 } },
	{ { -1.0,  1.0,  0.0 }, { 0.0, 1.0 } },
	{ { -1.0, -1.0,  0.0 }, { 0.0, 0.0 } },
};

static int faces[LAST_BLOCK][6] = {
	[BLOCK_GRASS] = {
		2, 2, 2, 2, 0, 1,
	},
	[BLOCK_DIRT] = {
		0, 0, 0, 0, 0, 0
	}
};

static GLFWwindow *window;
static unsigned int chunk_program;
static unsigned int projection_uni, view_uni, terrain_uni, chunk_position_uni;

static unsigned int quad_buffer, quad_vao;

static mat4x4 projection, view;
static Chunk chunk;

static Texture terrain;
static Player player;

int
main()
{
	double pre_time;
	glfwSetErrorCallback(error_callback);
	if(!glfwInit())
		return -1;

	glfwDefaultWindowHints();
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
	window = glfwCreateWindow(800, 600, "hello", NULL, NULL);
	if(!window)
		return -2;
	glfwSetMouseButtonCallback(window, mouse_click_callback);
	glfwSetKeyCallback(window, keyboard_callback);
	glfwMakeContextCurrent(window);
	if(glewInit() != GLEW_OK)
		return -3;

	load_programs();
	load_buffers();
	load_textures();

	world_init();
	for(int x = 0; x < 10; x++)
		for(int z = 0; z < 10; z++) {
			world_enqueue_load(x * CHUNK_SIZE, 0, z * CHUNK_SIZE);
		}

	player.yaw = 0.0;
	player.pitch = 0.0;
	player.position[0] = 0;
	player.position[1] = 0;
	player.position[2] = 0;

	glfwShowWindow(window);
	pre_time = glfwGetTime();
	while(!glfwWindowShouldClose(window)) {
		int w, h;
		double curr_time = glfwGetTime();
		double delta = curr_time - pre_time;
		pre_time = curr_time;

		player_update(&player, delta);

		glfwGetWindowSize(window, &w, &h);
		mat4x4_perspective(projection, M_PI_2, (float)w/h, 0.001, 1000.0);

		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glEnable(GL_DEPTH_TEST);
		
		glUseProgram(chunk_program);
		glUniformMatrix4fv(projection_uni, 1, GL_FALSE, &projection[0][0]);
		glUniformMatrix4fv(view_uni, 1, GL_FALSE, &view[0][0]);
		glUniform1i(terrain_uni, 0);

		world_render();

		glUseProgram(0);

		glDisable(GL_DEPTH_TEST);
		glDisable(GL_CULL_FACE);

		glfwSwapBuffers(window);
		glfwPollEvents();

		while(glGetError() != GL_NO_ERROR);
	}

	world_terminate();

	glfwDestroyWindow(window);
	glfwTerminate();
	return 0;
}

void
error_callback(int errcode, const char *msg)
{
	printf("GLFW error (%x): %s\n", errcode, msg);
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
	
	projection_uni = glGetUniformLocation(chunk_program, "u_Projection");
	view_uni       = glGetUniformLocation(chunk_program, "u_View");
	terrain_uni    = glGetUniformLocation(chunk_program, "u_Terrain");
	chunk_position_uni = glGetUniformLocation(chunk_program, "u_ChunkPosition");
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
player_update(Player *player, float delta)
{
	int w, h;
	double mx, my, mdx, mdy;
	vec3 front_dir, right_dir;

	if(locking) {
		glfwGetWindowSize(window, &w, &h);
		glfwGetCursorPos(window, &mx, &my);
		glfwSetCursorPos(window, w >> 1, h >> 1);
	
		mdx = (mx - (w >> 1)) * 0.005f;
		mdy = (my - (h >> 1)) * 0.005f;

		player->pitch -= mdy;
		player->yaw -= mdx;

		if(player->yaw < 0)          player->yaw =  2 * M_PI + player->yaw;
		if(player->yaw > (2 * M_PI)) player->yaw -= 2 * M_PI;

		if(player->pitch >  (MAX_PITCH)) player->pitch =  MAX_PITCH;
		if(player->pitch < -(MAX_PITCH)) player->pitch = -MAX_PITCH;
	}

	front_dir[0] = sinf(player->yaw);
	front_dir[1] = 0.0;
	front_dir[2] = cosf(player->yaw);

	vec3_mul_cross(right_dir, front_dir, (vec3){ 0.0, 1.0, 0.0 });
	
	if(glfwGetKey(window, GLFW_KEY_W))
		vec3_add_scaled(player->position, player->position, front_dir, delta * PLAYER_SPEED);
	if(glfwGetKey(window, GLFW_KEY_S))
		vec3_add_scaled(player->position, player->position, front_dir, -delta * PLAYER_SPEED);
	if(glfwGetKey(window, GLFW_KEY_A))
		vec3_add_scaled(player->position, player->position, right_dir, -delta * PLAYER_SPEED);
	if(glfwGetKey(window, GLFW_KEY_D))
		vec3_add_scaled(player->position, player->position, right_dir, delta * PLAYER_SPEED);
	if(glfwGetKey(window, GLFW_KEY_SPACE))
		vec3_add_scaled(player->position, player->position, (vec3){ 0.0, 1.0, 0.0 }, delta * PLAYER_SPEED);
	if(glfwGetKey(window, GLFW_KEY_LEFT_SHIFT))
		vec3_add_scaled(player->position, player->position, (vec3){ 0.0, 1.0, 0.0 }, -delta * PLAYER_SPEED);

	front_dir[0] = front_dir[0] * cosf(player->pitch);
	front_dir[1] = sinf(player->pitch);
	front_dir[2] = front_dir[2] * cosf(player->pitch);
	vec3_dup(player->camera_view, front_dir);
	vec3_add(front_dir, player->position, front_dir);

	mat4x4_look_at(view, player->position, front_dir, (vec3){ 0.0, 1.0, 0.0 });
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

	if(z == 0 || chunk->blocks[z - 1][y][x] == 0) {
		get_cube_face(&terrain, faces[block][BACK], min, max);
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
	}

	if(x == LAST_BLOCK || chunk->blocks[z][y][x + 1] == 0) {
		get_cube_face(&terrain, faces[block][RIGHT], min, max);
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  0 + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy,  BLOCK_SCALE + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { min[0], max[1] } );
	}

	if(z == LAST_BLOCK || chunk->blocks[z + 1][y][x] == 0) {
		get_cube_face(&terrain, faces[block][FRONT], min, max);
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy, BLOCK_SCALE + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy, BLOCK_SCALE + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy, BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy, BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy, BLOCK_SCALE + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy, BLOCK_SCALE + zz }, .texcoord = { min[0], max[1] } );
	}

	if(x == 0 || chunk->blocks[z][y][x - 1] == 0) {
		get_cube_face(&terrain, faces[block][LEFT], min, max);
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
	}

	if(y == 0 || chunk->blocks[z][y - 1][x] == 0) {
		get_cube_face(&terrain, faces[block][BOTTOM], min, max);
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  0 + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  BLOCK_SCALE + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  0 + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
	}

	if(y == LAST_BLOCK || chunk->blocks[z][y + 1][x] == 0) {
		get_cube_face(&terrain, faces[block][TOP], min, max);
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { max[0], max[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  0 + xx,  BLOCK_SCALE + yy,  BLOCK_SCALE + zz }, .texcoord = { max[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy,  BLOCK_SCALE + zz }, .texcoord = { min[0], min[1] } );
		INSERT_VERTEX(.position = {  BLOCK_SCALE + xx,  BLOCK_SCALE + yy,  0 + zz }, .texcoord = { min[0], max[1] } );
	}
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
chunk_renderer_render_chunk(unsigned int vao, unsigned int vertex_count, vec3 position)
{
	glUniform3fv(chunk_position_uni, 1, position);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, terrain.texture);

	glBindVertexArray(vao);
	glDrawArrays(GL_TRIANGLES, 0, vertex_count);
	glBindVertexArray(0);

	glBindTexture(GL_TEXTURE_2D, 0);
}

void
chunk_renderer_generate_buffers(Chunk *chunk)
{
	ArrayBuffer buffer;

	arrbuf_init(&buffer);
	for(int z = 0; z < CHUNK_SIZE; z++)
		for(int y = 0; y < CHUNK_SIZE; y++)
			for(int x = 0; x < CHUNK_SIZE; x++) {
				if(chunk->blocks[z][y][x])
					chunk_generate_face(chunk, x, y, z, &buffer);
			}
	chunk->vert_count = arrbuf_length(&buffer, sizeof(Vertex));

	chunk->chunk_vbo = ugl_create_buffer(GL_STATIC_DRAW, buffer.size, buffer.data);
	chunk->chunk_vao = ugl_create_vao(2, (VaoSpec[]){
		{ 0, 3, GL_FLOAT, sizeof(Vertex), offsetof(Vertex, position), 0, chunk->chunk_vbo },
		{ 1, 2, GL_FLOAT, sizeof(Vertex), offsetof(Vertex, texcoord), 0, chunk->chunk_vbo },
	});
	arrbuf_free(&buffer);
}

void
mouse_click_callback(GLFWwindow *window, int button, int action, int mods)
{
	if(action == GLFW_RELEASE)
		return;

	if(!locking) {
		int w, h;
		glfwGetWindowSize(window, &w, &h);
		glfwSetCursorPos(window, w >> 1, h >> 1);
		locking = true;
		return;
	}

	if(button == 0) {
		RaycastWorld rw = world_begin_raycast(player.position, player.camera_view, 5.0);
		while(world_raycast(&rw)) {
			if(rw.block > 0) {
				world_set_block(rw.position[0], rw.position[1], rw.position[2], BLOCK_NULL);
				break;
			}
		}
	}
}

void
keyboard_callback(GLFWwindow *window, int key, int scan, int action, int mods)
{
	if(key == GLFW_KEY_ESCAPE) {
		locking = false;
	}
}
