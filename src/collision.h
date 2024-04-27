#ifndef COLLISION_H
#define COLLISION_H

#include <stdbool.h>
#include <linmath.h>

typedef struct {
	vec3 penetration_vector;
	vec3 normal;
} Contact;

typedef struct {
	vec3 position;
	vec3 halfsize;
} AABB;

bool collide(AABB *first, AABB *second, Contact *out);

#endif