#include <stdio.h>
#include <math.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include "glutil.h"
#include "linmath.h"

#ifndef M_PI
#define M_PI 3.1415926535
#endif

#ifndef M_PI_2
#define M_PI_2 (M_PI * 0.5)
#endif

#define EPSLON 0.0000001
#define MAX_PITCH (M_PI_2 - EPSLON)
#define PLAYER_SPEED 4.0

typedef struct {
	vec3 position;
	vec2 texcoord;
} Vertex;

typedef struct {
	vec3 position;
	float pitch, yaw;
	vec3 camera_view;
} Player;

static void player_update(Player *player, float delta);

static void load_programs();
static void load_buffers();
static void error_callback(int errcode, const char *msg);

static Vertex quad_data[] = {
	{ { -1.0, -1.0,  0.0 }, { 0.0, 0.0 } },
	{ {  1.0, -1.0,  0.0 }, { 1.0, 0.0 } },
	{ {  1.0,  1.0,  0.0 }, { 1.0, 1.0 } },

	{ {  1.0,  1.0,  0.0 }, { 1.0, 1.0 } },
	{ { -1.0,  1.0,  0.0 }, { 0.0, 1.0 } },
	{ { -1.0, -1.0,  0.0 }, { 0.0, 0.0 } },
};

static GLFWwindow *window;
static unsigned int chunk_program;
static unsigned int projection_uni, view_uni;

static unsigned int quad_buffer, quad_vao;

static mat4x4 projection, view;

int
main()
{
	Player player;
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
	glfwMakeContextCurrent(window);
	if(glewInit() != GLEW_OK)
		return -3;

	load_programs();
	load_buffers();

	player.yaw = 0.0;
	player.pitch = 0.0;
	player.position[0] =   0;
	player.position[1] =   0;
	player.position[2] = -10;

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
		
		glUseProgram(chunk_program);
		glUniformMatrix4fv(projection_uni, 1, GL_FALSE, &projection[0][0]);
		glUniformMatrix4fv(view_uni, 1, GL_FALSE, &view[0][0]);

		glBindVertexArray(quad_vao);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		glBindVertexArray(0);

		glUseProgram(0);

		glfwSwapBuffers(window);
		glfwPollEvents();
	}

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
	vec3_add(front_dir, player->position, front_dir);

	mat4x4_look_at(view, player->position, front_dir, (vec3){ 0.0, 1.0, 0.0 });
}
