#version 450
// Draw-storm vertex shader. Emits a tiny triangle per draw, positioned by the push constants,
// and reads a uniform buffer so the vertex stage has descriptors to bind.

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
	vec2 p = corners[gl_VertexIndex] * transform.scale.xy + pc.offset;
	outUV = corners[gl_VertexIndex] * 50.0;
	gl_Position = vec4(p, 0.0, 1.0);
}
