#include "worldgen.h"
#include "util.h"
#include "world.h"

#include <stdio.h>
#include <noise1234.h>

#define X_SCALE (0.0625 / CHUNK_SIZE)
#define Z_SCALE (0.0625 / CHUNK_SIZE)

float octaved2(float x, float z, int seed);

static PCG32State basic_seed;
static uint32_t   heightmap_seed;

void 
wgen_set_seed(const char *seed)
{
	basic_seed = hash_string(seed);
	init_pcg32(&basic_seed);

	heightmap_seed = rand_pcg32(&basic_seed);
}

void
wgen_generate(int cx, int cy, int cz)
{
	for(int x = 0; x < CHUNK_SIZE; x++)
	for(int z = 0; z < CHUNK_SIZE; z++) 
	{
		int xx = x + cx;
		int zz = z + cz;
		
		float height = octaved2(xx, zz, heightmap_seed) * 8 + 64;
		for(int y = 0; y < CHUNK_SIZE; y++) {
			int yy = y + cy;
			if(yy <= height) {
				world_set_block(xx, yy, zz, BLOCK_GRASS);
			} else {
				world_set_block(xx, yy, zz, BLOCK_NULL);
			}
		}
	}
}

float
octaved2(float x, float z, int seed)
{
	float a = 4.0;
	float r = 0.0;

	for(int i = 0; i < 8; i++) {
		r += noise3(x * a * X_SCALE, z * a * X_SCALE, seed) * a;
		a *= 0.5;
	}

	return r / 4;
}
