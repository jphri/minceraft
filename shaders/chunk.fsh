#version 330 core

uniform sampler2D u_Terrain;

in VS_OUT {
	vec2 texcoord;
} in_FS;

layout(location = 0) out vec4 out_Color;

void
main()
{
	out_Color = texture(u_Terrain, in_FS.texcoord);
}
