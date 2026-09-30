#version 450
// Draw-storm fragment shader. Statically uses many descriptors, as engines commonly do,
// so that the per-draw descriptor bind work in the driver is significant.

layout(push_constant) uniform PushConstants {
	vec4 color;
	vec2 offset;
	uint drawIndex;
} pc;

layout(set = 1, binding = 0) uniform sampler2D textures[16];
layout(set = 1, binding = 1) uniform Material {
	vec4 tint;
} materials[8];

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

void main() {
	vec4 c = pc.color;
	for (int i = 0; i < 16; i++) { c += texture(textures[i], inUV) * 0.01; }
	for (int i = 0; i < 8; i++)  { c += materials[i].tint * 0.01; }
	outColor = c;
}
