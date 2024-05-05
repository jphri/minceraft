#include "worldgen.h"
#include "util.h"
#include "world.h"

#include <stdio.h>
#include <linmath.h>
#include <noise1234.h>

#define X_SCALE (0.0625 / CHUNK_SIZE)
#define Y_SCALE (0.0625 / CHUNK_SIZE)
#define Z_SCALE (0.0625 / CHUNK_SIZE)

#define GROUND_HEIGHT 64

typedef struct {
	float x, y;
} SplinePoint;

static float octaved2(vec2 v, int seed);
static float octaved3(vec3 v, int seed);
static float spline(float in, size_t nsplines, SplinePoint *splines);
static float map(float l, float xmin, float xmax, float ymin, float ymax);

static float heightmap(vec2 v, int seed);

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
	for(int x = 0; x < CHUNK_SIZE; x++)
	for(int z = 0; z < CHUNK_SIZE; z++) 
	{
		int xx = x + cx;
		int zz = z + cz;

		vec2 v;
		vec2_mul(v, (vec2){ xx, zz }, (vec2){ 0.0625 / CHUNK_SIZE, 0.0625 / CHUNK_SIZE });
		
		float height = heightmap(v, heightmap_seed);
		for(int y = 0; y < CHUNK_SIZE; y++) {
			vec3 vv;
			int yy = y + cy;

			vec3_mul(vv, (vec3){ xx, yy, zz }, (vec3){ 0.25 / CHUNK_SIZE, 0.75 / CHUNK_SIZE, 0.25 / CHUNK_SIZE });

			float density = octaved3(vv, density_seed) + (height - yy) * 6.75 / GROUND_HEIGHT;

			if(density > 0) {
				world_set_block(xx, yy, zz, BLOCK_GRASS);
			} else {
				world_set_block(xx, yy, zz, BLOCK_NULL);
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
		{  0.60, GROUND_HEIGHT + 10  },
		{  0.95, GROUND_HEIGHT + 15 }
	};

	return spline(octaved2(v, seed), LENGTH(splines), splines);
}
