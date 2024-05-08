#include "worldgen.h"
#include "util.h"
#include "world.h"

#include <stdio.h>
#include <linmath.h>
#include <noise1234.h>

#define X_SCALE (0.0625 / CHUNK_SIZE)
#define Y_SCALE (0.0625 / CHUNK_SIZE)
#define Z_SCALE (0.0625 / CHUNK_SIZE)

#define HEIGHT_AMPL 5.0
#define HEIGHT_SCALE (vec2){ 0.0625 / CHUNK_SIZE, 0.0625 / CHUNK_SIZE }
#define NOISE3_SCALE (vec3){ 0.25 / CHUNK_SIZE, 0.4 / CHUNK_SIZE, 0.25 / CHUNK_SIZE }

#define GROUND_HEIGHT 64

typedef struct {
	float x, y;
} SplinePoint;

static float octaved2(vec2 v, int seed);
static float octaved3(vec3 v, int seed);
static float spline(float in, size_t nsplines, SplinePoint *splines);
static float map(float l, float xmin, float xmax, float ymin, float ymax);

static float heightmap(vec2 v, int seed);

static void surface(int x, int y, int z, float height);
static void stage_shape(int x, int y, int z);

static PCG32State basic_seed;
static uint32_t   heightmap_seed;
static uint32_t   density_seed;

void 
wgen_set_seed(const char *seed)
{
	basic_seed = hash_string(seed);
	init_pcg32(&basic_seed);

	heightmap_seed = rand_pcg32(&basic_seed);
	density_seed   = rand_pcg32(&basic_seed);
}
 
void
wgen_generate(int cx, int cy, int cz)
{
	stage_shape(cx, cy, cz);
}

void
stage_shape(int cx, int cy, int cz)
{
	/* cache */
	float density_queue[4];
	int density_index = 0, density_size = 0;

	for(int x = 0; x < CHUNK_SIZE; x++)
	for(int z = 0; z < CHUNK_SIZE; z++) 
	{
		int xx = x + cx;
		int zz = z + cz;

		vec2 v;
		vec2_mul(v, (vec2){ xx, zz }, HEIGHT_SCALE);
		
		density_size = 0;
		density_index = 0;
		float height = heightmap(v, heightmap_seed);
		for(int y = 0; y < CHUNK_SIZE; y++) {
			vec3 vv;
			int yy = y + cy;

			vec3_mul(vv, (vec3){ xx, yy, zz }, NOISE3_SCALE);
			float density = octaved3(vv, density_seed) + (height - yy) * HEIGHT_AMPL / GROUND_HEIGHT;
			density_queue[density_index++] = density;
			if(density_size < LENGTH(density_queue)) {
				density_size++;
			} else {
				density_index %= LENGTH(density_queue);
			}

			if(density > 0) {
				surface(xx, yy, zz, height);
			} else {
				if(yy < GROUND_HEIGHT)
					world_set(xx, yy, zz, CSTATE_ALLOCATED, BLOCK_WATER);
				else
					world_set(xx, yy, zz, CSTATE_ALLOCATED, BLOCK_NULL);
			}
		}
	}
}

float
octaved2(vec2 v, int seed)
{
	float a = 4.0;
	float r = 0.0;

	for(int i = 0; i < 8; i++) {
		r += noise3(v[0] * a, v[1] * a, seed) * a;
		a *= 0.5;
	}

	return r / 4;
}

float
octaved3(vec3 position, int seed)
{
	float a = 4.0;
	float r = 0.0;

	for(int i = 0; i < 8; i++) {
		r += noise4(position[0] * a, position[1] * a, position[2] * a, seed) * a;
		a *= 0.5;
	}

	return r / 4;
}

float
map(float l, float xmin, float xmax, float ymin, float ymax)
{
	const float slope = (ymax - ymin) / (xmax - xmin);
	return ymin + (l - xmin) * slope;
}

float
spline(float in, size_t nsplines, SplinePoint *splines)
{
	for(size_t i = 1; i < nsplines; i++) {
		if(splines[i].x > in && splines[i - 1].x < in) {
			return map(in, splines[i - 1].x, splines[i].x, splines[i - 1].y, splines[i].y);
		}
	}
	return 0;
}

float
heightmap(vec2 v, int seed)
{
	static SplinePoint splines[] = {
		{ -1.00, GROUND_HEIGHT - 20 },
		{ -0.50, GROUND_HEIGHT - 10 },
		{ -0.40, GROUND_HEIGHT - 2   },
		{  0.40, GROUND_HEIGHT + 2   },
		{  0.80, GROUND_HEIGHT + 2  },
		{  0.95, GROUND_HEIGHT + 40 }
	};

	return spline(octaved2(v, seed), LENGTH(splines), splines);
}

void
surface(int x, int y, int z, float height)
{
	vec3 vv;
	int i;
	for(i = 1; i < 4; i++) {
		vec3_mul(vv, (vec3){ x, y + i, z }, NOISE3_SCALE);
		float density = octaved3(vv, density_seed) + (height - y - i) * HEIGHT_AMPL / GROUND_HEIGHT;

		if(density < 0)
			break;
	}
	switch(i) {
	case 1:
		world_set(x, y, z, CSTATE_ALLOCATED, BLOCK_GRASS);
		break;
	case 2:
	case 3:
		world_set(x, y, z, CSTATE_ALLOCATED, BLOCK_DIRT);
		break;
	case 4:
		world_set(x, y, z, CSTATE_ALLOCATED, BLOCK_STONE);
	}
}
