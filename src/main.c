#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <assert.h>

#include "chunk_renderer.h"
#include "cubegame.h"
#include "util.h"
#include "glutil.h"
#include "linmath.h"
#include "world.h"
#include "collision.h"

#include <stb_image.h>

#ifndef M_PI
#define M_PI 3.1415926535
#endif

#ifndef M_PI_2
#define M_PI_2 (M_PI * 0.5)
#endif

#define WATER_OFFSET 0.1

#define EPSLON 0.00001
#define MAX_PITCH (M_PI_2 - EPSLON)
#define PLAYER_SPEED 100

#define PHYSICS_DELTA (1.0/480.0)


typedef struct {
	vec3 position, velocity, accel;
	vec3 eye_position;
	float pitch, yaw;
	vec3 camera_view;
	bool jumping;
	int old_chunk_x, old_chunk_y, old_chunk_z;
} Player;

static bool locking;
static float physics_accum;

static void player_update(Player *player, float delta);

static void error_callback(int errcode, const char *msg);
static void mouse_click_callback(GLFWwindow *window, int button, int action, int mods);
static void keyboard_callback(GLFWwindow *window, int scan, int key, int action, int mods);

static GLFWwindow *window;

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

	world_init();
	chunk_render_init();

	player.yaw = 0.0;
	player.pitch = 0.0;
	player.position[0] = 0;
	player.position[1] = 30;
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
		glViewport(0, 0, w, h);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		chunk_render_set_camera(player.eye_position, player.camera_view, (float)w/h, 16);
		chunk_render();

		glfwSwapBuffers(window);
		glfwPollEvents();

		while(glGetError() != GL_NO_ERROR);
	}

	chunk_render_terminate();
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

	vec3_dup(player->accel, (vec3){ 0.0, 0.0, 0.0 });
	if(glfwGetKey(window, GLFW_KEY_W))
		vec3_add_scaled(player->accel, player->accel, front_dir, PLAYER_SPEED);
	if(glfwGetKey(window, GLFW_KEY_S))
		vec3_add_scaled(player->accel, player->accel, front_dir, -PLAYER_SPEED);
	if(glfwGetKey(window, GLFW_KEY_A))
		vec3_add_scaled(player->accel, player->accel, right_dir, -PLAYER_SPEED);
	if(glfwGetKey(window, GLFW_KEY_D))
		vec3_add_scaled(player->accel, player->accel, right_dir, PLAYER_SPEED);
	if(!player->jumping)
		if(glfwGetKey(window, GLFW_KEY_SPACE)) {
			vec3_add(player->velocity, player->velocity, (vec3){ 0.0, 9.0, 0.0 });
			player->jumping = true;
		}

	vec3_add(player->eye_position, player->position, (vec3){ 0.0, 0.6, 0.0 });

	front_dir[0] = front_dir[0] * cosf(player->pitch);
	front_dir[1] = sinf(player->pitch);
	front_dir[2] = front_dir[2] * cosf(player->pitch);
	vec3_dup(player->camera_view, front_dir);
	
	physics_accum += delta;
	vec3_add(player->accel, player->accel, (vec3){ 0.0, -32.0, 0.0 });
	vec3_add_scaled(player->accel, player->accel, (vec3){ player->velocity[0], 0.0, player->velocity[2] }, -16.0);
	while(physics_accum > PHYSICS_DELTA) {
		vec3_add_scaled(player->position, player->position, player->velocity, PHYSICS_DELTA);
		vec3_add_scaled(player->velocity, player->velocity, player->accel, PHYSICS_DELTA);

		AABB player_aabb;
		vec3_dup(player_aabb.position, player->position);
		vec3_dup(player_aabb.halfsize, (vec3){ 0.4, 0.8, 0.4 });
		for(int x = -1; x <= 1; x++)
		for(int y = -1; y <= 1; y++)
		for(int z = -1; z <= 1; z++) {
			int player_x = floorf(x + player->position[0]);
			int player_y = floorf(y + player->position[1]);
			int player_z = floorf(z + player->position[2]);

			Block b = world_get_block(player_x, player_y, player_z);
			if(b > 0) {
				Contact c;
				AABB block_aabb = {
					.position = { player_x + 0.5, player_y + 0.5, player_z + 0.5 },
					.halfsize = { 0.5, 0.5, 0.5 }
				};

				if(collide(&player_aabb, &block_aabb, &c)) {
					vec3 subv;

					vec3_sub(player->position, player->position, c.penetration_vector);
					for(int i = 0; i < 3; i++)
						subv[i] = fabsf(player->velocity[i]) * c.normal[i];
					vec3_add(player->velocity, player->velocity, subv);

					if(c.normal[1] > 0.0) {
						player->jumping = false;
					}
				}
			}
		}
		physics_accum -= PHYSICS_DELTA;
	}

	int chunk_x = (int)floorf(player->position[0]) & CHUNK_MASK;
	int chunk_y = (int)floorf(player->position[1]) & CHUNK_MASK;
	int chunk_z = (int)floorf(player->position[2]) & CHUNK_MASK;
	
	if(chunk_x != player->old_chunk_x || chunk_y != player->old_chunk_y || chunk_z != player->old_chunk_z) {
		player->old_chunk_x = chunk_x;
		player->old_chunk_y = chunk_y;
		player->old_chunk_z = chunk_z;

		world_set_load_border(chunk_x, chunk_y, chunk_z, 256);
	}
}

void
mouse_click_callback(GLFWwindow *window, int button, int action, int mods)
{
	UNUSED(mods);
	if(action == GLFW_RELEASE)
		return;

	if(!locking) {
		int w, h;
		glfwGetWindowSize(window, &w, &h);
		glfwSetCursorPos(window, w >> 1, h >> 1);
		locking = true;
		return;
	}

	vec3 eye_position;
	vec3_add(eye_position, player.position, (vec3){ 0.0, 0.6, 0.0 });

	if(button == 0) {
		RaycastWorld rw = world_begin_raycast(eye_position, player.camera_view, 5.0);
		while(world_raycast(&rw)) {
			if(rw.block > 0) {
				world_set_block(rw.position[0], rw.position[1], rw.position[2], BLOCK_NULL);
				chunk_render_request_update_block(rw.position[0], rw.position[1], rw.position[2]);
				break;
			}
		}
	} else if(button == 1) {
		vec3 dir, block;
		RaycastWorld rw = world_begin_raycast(eye_position, player.camera_view, 5.0);
		while(world_raycast(&rw)) {
			if(rw.block > 0) {
				block_face_to_dir(rw.face, dir);
				vec3_add(block, rw.position, dir);

				if(world_get_block(block[0], block[1], block[2]) == BLOCK_NULL) {
					world_set_block(block[0], block[1], block[2], BLOCK_DIRT);
					chunk_render_request_update_block(block[0], block[1], block[2]);
				}
				break;
			}
		}
	}
}

void
keyboard_callback(GLFWwindow *window, int key, int scan, int action, int mods)
{
	UNUSED(action);
	UNUSED(scan);
	UNUSED(mods);
	UNUSED(window);
	
	if(key == GLFW_KEY_ESCAPE) {
		locking = false;
	}
}
