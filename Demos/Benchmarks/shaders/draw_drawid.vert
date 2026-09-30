#version 460
// Variant of draw.vert that reads gl_DrawID. MoltenVK passes the draw index to such shaders
// in an implicit buffer, which it fills separately for every draw.

layout(push_constant) uniform PushConstants {
	vec4 color;
	vec2 offset;
	uint drawIndex;
} pc;

layout(set = 0, binding = 0) uniform Transform {
	vec4 scale;
} transform;

layout(location = 0) out vec2 outUV;

void main() {
	const vec2 corners[3] = vec2[](vec2(0.0, 0.0), vec2(0.02, 0.0), vec2(0.0, 0.02));
	vec2 p = corners[gl_VertexIndex] * transform.scale.xy + pc.offset + vec2(float(gl_DrawID) * 0.001);
	outUV = corners[gl_VertexIndex] * 50.0;
	gl_Position = vec4(p, 0.0, 1.0);
}
