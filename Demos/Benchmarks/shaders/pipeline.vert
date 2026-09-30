#version 450
// Pipeline-creation vertex shader. kPatchValue is replaced in the SPIR-V binary for every
// pipeline, so each pipeline gets a distinct shader module and a distinct MSL source, which
// defeats both MoltenVK's shader cache and Metal's on-disk shader cache between runs.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUV;
layout(location = 0) out vec2 outUV;
layout(location = 1) out vec3 outNormal;

layout(set = 0, binding = 0) uniform Camera {
	mat4 viewProj;
	vec4 params;
} camera;

void main() {
	const float kPatchValue = 1234.5678;
	vec3 p = inPosition;
	for (int i = 0; i < 4; i++) { p = p * camera.params.x + sin(p.yzx * kPatchValue); }
	outUV = inUV;
	outNormal = normalize(p + vec3(0.0, 0.0, 1.0));
	gl_Position = camera.viewProj * vec4(p, 1.0);
}
