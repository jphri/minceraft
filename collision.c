#include <math.h>
#include "collision.h"

bool
collide(AABB *first, AABB *second, Contact *out)
{
	vec3 position, fullsize;
	vec3 min, max;
	float mindist;
	
	vec3_sub(position, first->position, second->position);
	vec3_add(fullsize, first->halfsize, second->halfsize);

	vec3_sub(min, position, fullsize);
	vec3_add(max, position, fullsize);

	for(int i = 0; i < 3; i++) {
		if(min[i] > 0 || max[i] < 0)
			return false;
	}
	
	mindist = fabsf(min[0]);
	out->penetration_vector[0] = min[0];
	out->penetration_vector[1] = 0;
	out->penetration_vector[2] = 0;

	if(mindist > fabsf(max[0])) {
		mindist = fabsf(max[0]);
		out->penetration_vector[0] = max[0];
		out->penetration_vector[1] = 0;
		out->penetration_vector[2] = 0;
	}

	if(mindist > fabsf(min[1])) {
		mindist = fabsf(min[1]);
		out->penetration_vector[0] = 0;
		out->penetration_vector[1] = min[1];
		out->penetration_vector[2] = 0;
	}

	if(mindist > fabsf(max[1])) {
		mindist = fabsf(max[1]);
		out->penetration_vector[0] = 0;
		out->penetration_vector[1] = max[1];
		out->penetration_vector[2] = 0;
	}

	if(mindist > fabsf(min[2])) {
		mindist = fabsf(min[2]);
		out->penetration_vector[0] = 0;
		out->penetration_vector[1] = 0;
		out->penetration_vector[2] = min[2];
	}

	if(mindist > fabsf(max[2])) {
		mindist = fabsf(max[2]);
		out->penetration_vector[0] = 0;
		out->penetration_vector[1] = 0;
		out->penetration_vector[2] = max[2];
	}
	
	for(int i = 0; i < 3; i++)
		out->normal[i] = ((out->penetration_vector[i] < 0) - (out->penetration_vector[i] > 0));

	return true;
}

