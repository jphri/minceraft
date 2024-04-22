#version 330 core

uniform mat4 u_Projection;
uniform mat4 u_View;

layout (location = 0) in vec3 position;
layout (location = 1) in vec2 texcoord;

out VS_OUT {
	vec2 texcoord;
} out_VS;

void
main()
{
	gl_Position = u_Projection * u_View * vec4(position, 1.0);
	out_VS.texcoord = texcoord;
}
