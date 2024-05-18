#include <bits/time.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <assert.h>

#include "chunk_renderer.h"
#include "global.h"
#include "util.h"
#include "glutil.h"
#include "linmath.h"
#include "worldgen.h"
#include "world.h"
#include "collision.h"

#include <stb_image.h>
#include <time.h>

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
static pthread_mutex_t context_mtx;

static Player player;
static int frames;
static float fps_time;
static int old_chunk_count;

#if 1
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

	glfwSwapInterval(1);

	pthread_mutex_init(&context_mtx, NULL);

	world_init();
	chunk_render_init();

	player.yaw = 0.0;
	player.pitch = 0.0;
	player.position[0] = 0;
	player.position[1] = 80;
	player.position[2] = 0;

	wgen_set_seed("Gente que passa o dia inteiro no twitter e em chan não deveria nem ter direito a voto.");

	glfwShowWindow(window);
	pre_time = glfwGetTime();
	old_chunk_count = world_allocated_chunks_count();
	while(!glfwWindowShouldClose(window)) {
		int w, h;
		double curr_time = glfwGetTime();
		double delta = curr_time - pre_time;
		pre_time = curr_time;

		if(delta > 0.25)
			delta = 0.25;

		player_update(&player, delta);

		glfwGetWindowSize(window, &w, &h);
		lock_gl_context();
		glViewport(0, 0, w, h);
		glClearColor(0.5, 0.7, 0.9, 1.0);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		unlock_gl_context();

		chunk_render_set_camera(player.eye_position, player.camera_view, (float)w/h, 256);
		chunk_render();

		glfwSwapBuffers(window);
		glfwPollEvents();

		while(glGetError() != GL_NO_ERROR);

		frames++;
		fps_time += delta;
		if(fps_time > 1.0) {
			int current = world_allocated_chunks_count();
			int cdelta = current - old_chunk_count;
			old_chunk_count = current;

			printf("FPS: %d (%d chunks (%0.2f MB), %d new chunks...)\n", frames, current, (current * sizeof(Chunk) / (1024.0 * 1024.0)), cdelta);
			frames = 0;
			fps_time = 0;
		}
	}

	world_set_load_border(0, 0, 0, -2147483648);
	chunk_render_terminate();
	world_terminate();


	glfwDestroyWindow(window);
	glfwTerminate();
	return 0;
}
#else

int
main()
{
	struct timespec start, end;
	world_init();
	world_set_load_border(0, 0, 0, 1024);

	clock_gettime(CLOCK_REALTIME, &start);
	
	#define W 32
	#define H 32
	#define D 32

	#define CZ (CHUNK_SIZE)

	#pragma omp parallel for num_threads(6)
	for(int i = 0; i < W * H * D; i++) {
		Block block;
		int x, y, z;

		x = (i % W);
		y = (i / W) % H;
		z = (i / W) / H;

		for(int j = 0; j < CZ * CZ * CZ; j++) {
			int xx = x * CHUNK_SIZE + j % (CZ);
			int yy = y * CHUNK_SIZE + (j / CZ) % CZ;
			int zz = z * CHUNK_SIZE + (j / CZ) / CZ;

			while((block = world_get_block(xx, yy, zz)) == BLOCK_UNLOADED) printf("Locked... %d\n",     omp_get_thread_num());
			while((block = world_get_block(xx - 1, yy, zz)) == BLOCK_UNLOADED) printf("Locked... %d\n", omp_get_thread_num());;
			while((block = world_get_block(xx + 1, yy, zz)) == BLOCK_UNLOADED) printf("Locked... %d\n", omp_get_thread_num());;
			while((block = world_get_block(xx, yy - 1, zz)) == BLOCK_UNLOADED) printf("Locked... %d\n", omp_get_thread_num());;
			while((block = world_get_block(xx, yy + 1, zz)) == BLOCK_UNLOADED) printf("Locked... %d\n", omp_get_thread_num());;
			while((block = world_get_block(xx, yy, zz - 1)) == BLOCK_UNLOADED) printf("Locked... %d\n", omp_get_thread_num());;
			while((block = world_get_block(xx, yy, zz + 1)) == BLOCK_UNLOADED) printf("Locked... %d\n", omp_get_thread_num());;
		}
	}

	clock_gettime(CLOCK_REALTIME, &end);
	
	double start_time = start.tv_sec + start.tv_nsec / 1000000000.0;
	double end_time   = end.tv_sec + end.tv_nsec / 1000000000.0;

	printf("Process time: %0.2f ms\n", (end_time - start_time) * 1000); 

	world_terminate();
	return 0;
}

#endif

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
		if(player->velocity[1] < -40.0)	
			player->velocity[1] = -40.0;

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
			if(b > 0 && !block_properties(b)->is_ghost) {
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
	} else if(button == 2 && action == GLFW_PRESS) {
		player.position[0] += 10000;
		player_update(&player, 0);
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

void
lock_gl_context()
{
	pthread_mutex_lock(&context_mtx);
	glfwMakeContextCurrent(window);
}

void
unlock_gl_context()
{
	glfwMakeContextCurrent(NULL);
	pthread_mutex_unlock(&context_mtx);
}
