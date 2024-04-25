#ifndef COLLISION_H
#define COLLISION_H

#include <stdbool.h>
#include <linmath.h>

typedef struct {
	vec3 penetration_vector;
	vec3 normal;
} Contact;

bool collide(vec3 a, vec3 asize, vec3 b, vec3 bsize, Contact *out);

#endif