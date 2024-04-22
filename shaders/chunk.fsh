#version 330 core

in VS_OUT {
	vec2 texcoord;
} in_FS;

layout(location = 0) out vec4 out_Color;

void
main()
{
	out_Color = vec4(in_FS.texcoord, 0.0, 1.0);
}
