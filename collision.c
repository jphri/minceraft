#include <math.h>
#include "collision.h"

bool
collide(vec3 a, vec3 asize, vec3 b, vec3 bsize, Contact *out)
{
	vec3 bfullsize;
	vec3 subpos;
	vec3 min, max;

	vec3_add(bfullsize, asize, bsize);
	vec3_sub(subpos, a, b);
	for(int i = 0; i < 3; i++) {
		if(subpos[i] + bfullsize[i] < 0 || subpos[i] + bfullsize[i] > 0)
			return false;
	}

	/* get the minimum distance we should travel to undo the collision */
	vec3_sub(min, subpos, bfullsize);
	vec3_add(max, subpos, bfullsize);

	float min_dist = fabsf(min[0]);
	out->penetration_vector[0] = min[0];
	out->penetration_vector[1] = 0;
	out->penetration_vector[2] = 0;

	out->normal[0] = -1.0;
	out->normal[1] = 0.0;
	out->normal[2] = 0.0;

	if(fabsf(max[0]) < min_dist) {
		min_dist = fabsf(max[0]);
		out->penetration_vector[0] = max[0];
		out->penetration_vector[1] = 0;
		out->penetration_vector[2] = 0;

		out->normal[0] = +1.0;
		out->normal[1] =  0.0;
		out->normal[2] =  0.0;
	}

	if(fabsf(min[1]) < min_dist) {
		min_dist = fabsf(min[1]);
		out->penetration_vector[0] = 0;
		out->penetration_vector[1] = min[1];
		out->penetration_vector[2] = 0;

		out->normal[0] =  0.0;
		out->normal[1] = -1.0;
		out->normal[2] =  0.0;

	}

	if(fabsf(max[1]) < min_dist) {
		min_dist = fabsf(max[1]);
		out->penetration_vector[0] = 0;
		out->penetration_vector[1] = max[1];
		out->penetration_vector[2] = 0;

		out->normal[0] =  0.0;
		out->normal[1] =  1.0;
		out->normal[2] =  0.0;
	}

	if(fabsf(min[2]) < min_dist) {
		min_dist = fabsf(min[2]);
		out->penetration_vector[0] = 0;
		out->penetration_vector[1] = 0;
		out->penetration_vector[2] = min[2];

		out->normal[0] =  0.0;
		out->normal[1] =  0.0;
		out->normal[2] = -1.0;
	}

	if(fabsf(max[2]) < min_dist) {
		min_dist = fabsf(max[2]);
		out->penetration_vector[0] = 0;
		out->penetration_vector[1] = 0;
		out->penetration_vector[2] = max[2];

		out->normal[0] =  0.0;
		out->normal[1] =  0.0;
		out->normal[2] =  1.0;
	}

	return true;
}

