#version 330 core

uniform sampler2D u_Terrain;
uniform float u_Alpha;

in VS_OUT {
	vec2 texcoord;
} in_FS;

layout(location = 0) out vec4 out_Color;

void
main()
{
	vec4 color = texture(u_Terrain, in_FS.texcoord);
	if(color.a < 0.5)
		discard;
	color.a *= u_Alpha;
	out_Color = color;
}
