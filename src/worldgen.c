#include "worldgen.h"
#include "util.h"
#include "world.h"

#include <stdio.h>
#include <linmath.h>
#include <noise1234.h>
#include <assert.h>

#define X_SCALE (0.0625 / 16)
#define Y_SCALE (0.0625 / 16)
#define Z_SCALE (0.0625 / 16)

#define HEIGHT_AMPL 1.25
#define HEIGHT_SCALE (vec2){ 0.0625 / (4 * 16), 0.0625 / (4 * 16) }
#define NOISE3_SCALE (vec3){ 0.125 / 16, 0.2 / 16, 0.125 / 16 }

#define GROUND_HEIGHT 64

typedef struct {
	float x, y;
} SplinePoint;

static float octaved2(vec2 v, int seed);
static float octaved3(vec3 v, int seed);
static int   hash_coord(uint32_t s, int x, int y, int z);

static void generate_block(int cx, int cy, int cz, int x, int y, int z, ChunkState state, bool force, Block block);
static void generate_tree(int cx, int cy, int cz, int x, int y, int z);

static float spline(float in, size_t nsplines, SplinePoint *splines);
static float map(float l, float xmin, float xmax, float ymin, float ymax);

static float heightmap(vec2 v, int seed);

static PCG32State basic_seed;
static uint32_t   heightmap_seed;
static uint32_t   density_seed;
static uint32_t   coord_hash;
static uint32_t   grass_flower_hash;

void 
wgen_set_seed(const char *seed)
{
	basic_seed = hash_string(seed);
	init_pcg32(&basic_seed);

	heightmap_seed    = rand_pcg32(&basic_seed);
	density_seed      = rand_pcg32(&basic_seed);
	coord_hash        = rand_pcg32(&basic_seed);
	grass_flower_hash = rand_pcg32(&basic_seed);
}

void
wgen_shape(int cx, int cy, int cz)
{
	for(int x = 0; x < CHUNK_SIZE; x++)
	for(int z = 0; z < CHUNK_SIZE; z++) 
	{
		int xx = x + cx;
		int zz = z + cz;

		vec2 v;
		vec2_mul(v, (vec2){ xx, zz }, HEIGHT_SCALE);
		
		float height = heightmap(v, heightmap_seed);
		for(int y = 0; y < CHUNK_SIZE; y++) {
			vec3 vv;
			int yy = y + cy;

			vec3_mul(vv, (vec3){ xx, yy, zz }, NOISE3_SCALE);
			float density = octaved3(vv, density_seed) + (height - yy) * HEIGHT_AMPL / GROUND_HEIGHT;
			world_set_density(xx, yy, zz, CSTATE_SHAPING, density);

			if(density > 0) {
				world_set(xx, yy, zz, CSTATE_SHAPING, BLOCK_STONE);
			} else {
				world_set(xx, yy, zz, CSTATE_SHAPING, yy < GROUND_HEIGHT ? BLOCK_WATER : BLOCK_NULL);
			}
		}
	}
}

void
wgen_surface(int cx, int cy, int cz)
{
	for(int z = cz; z < cz + CHUNK_SIZE; z++)
	for(int y = cy; y < cy + CHUNK_SIZE; y++)
	for(int x = cx; x < cx + CHUNK_SIZE; x++) {

		if(world_get(x, y, z, CSTATE_SURFACING) == BLOCK_STONE) {
			int i;
			for(i = 1; i < 4; i++) {
				float den = world_get_density(x, y + i, z, CSTATE_SHAPED);
				if(den == NAN)
					return;

				if(den <= 0)
					break;
			}
			
			switch(i) {
			case 1:
				world_set(x, y, z, CSTATE_SURFACING, y >= GROUND_HEIGHT ? BLOCK_GRASS : BLOCK_SAND);
				break;
			case 2:
			case 3:
				world_set(x, y, z, CSTATE_SURFACING, BLOCK_DIRT);
				break;
			}
		}
	}
}

void
wgen_decorate(int cx, int cy, int cz)
{
	for(int z = cz - CHUNK_SIZE; z < cz + CHUNK_SIZE * 2; z++)
	for(int y = cy - CHUNK_SIZE; y < cy + CHUNK_SIZE * 2; y++)
	for(int x = cx - CHUNK_SIZE; x < cx + CHUNK_SIZE * 2; x++) {

		if((hash_coord(coord_hash, x, y, z) & 15) != 0) {
			continue;
		}

		if(world_get_density(x, y, z, CSTATE_SHAPED) < 0) {
			float den = world_get_density(x, y - 1, z, CSTATE_SHAPED);
			if(den == NAN)
				return;

			if(den < 0)
				continue;
			
			if(y > GROUND_HEIGHT) {
				int hash = hash_coord(grass_flower_hash, x, y, z);
				if(!(hash & 7))
					generate_tree(cx, cy, cz, x, y, z);
				else
					switch(hash & 1) {
					case 0:
						generate_block(cx, cy, cz, x, y, z, CSTATE_DECORATING, false, BLOCK_GRASS_BLADES);
						break;
					case 1:
						generate_block(cx, cy, cz, x, y, z, CSTATE_DECORATING, false, BLOCK_ROSE);
						break;
					}
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

static uint32_t hash(uint32_t i)
{
	i = ((i >> 16) ^ i) * 0x45d9f3b;
	i = ((i >> 16) ^ i) * 0x45d9f3b;
	i = ((i >> 16) ^ i);
	return i;
}

int
hash_coord(uint32_t s, int x, int y, int z)
{
	#define INITIAL 0xDEADBEEF
	#define M 0x12345B
	uint32_t h = INITIAL;
	
	h = (hash(x * s) ^ h) * M;
	h = (hash(y * s) ^ h) * M;
	h = (hash(z * s) ^ h) * M;
	
	return h;
}

void
generate_block(int cx, int cy, int cz, int x, int y, int z, ChunkState state, bool force, Block block)
{
	if(x < cx || x >= (cx + CHUNK_SIZE)) return;
	if(y < cy || y >= (cy + CHUNK_SIZE)) return;
	if(z < cz || z >= (cz + CHUNK_SIZE)) return;
	
	if(force || block_properties(world_get(x, y, z, state))->replaceable)
		world_set(x, y, z, state, block);
}

void
generate_tree(int cx, int cy, int cz, int x, int y, int z)
{
	for(int xx = x - 1; xx <= x + 1; xx++)
	for(int zz = z - 1; zz <= z + 1; zz++) {
		generate_block(cx, cy, cz, xx, y + 6, zz, CSTATE_DECORATING, false, BLOCK_LEAVES);
	}

	for(int xx = x - 2; xx <= x + 2; xx++)
	for(int yy = 1; yy < 3; yy++)
	for(int zz = z - 2; zz <= z + 2; zz++) {
		generate_block(cx, cy, cz, xx, y + 6 - yy, zz, CSTATE_DECORATING, false, BLOCK_LEAVES);
	}

	for(int yy = y + 5; yy >= y; yy--) {
		generate_block(cx, cy, cz, x, yy, z, CSTATE_DECORATING, false, BLOCK_WOOD);
	}
	generate_block(cx, cy, cz, x, y - 1, z, CSTATE_DECORATING, true, BLOCK_DIRT);
}
