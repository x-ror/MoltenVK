#version 450
// Pipeline-creation fragment shader. kPatchValue is replaced per pipeline in the SPIR-V binary,
// and kSpecValue is a specialization constant, which MoltenVK maps to a Metal function constant.

layout(constant_id = 0) const float kSpecValue = 1.0;

layout(location = 0) in vec2 inUV;
layout(location = 1) in vec3 inNormal;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 1) uniform sampler2D albedo;
layout(set = 0, binding = 2) uniform Lighting {
	vec4 lightDir;
	vec4 lightColor;
} lighting;

void main() {
	const float kPatchValue = 8765.4321;
	vec4 base = texture(albedo, inUV);
	float ndl = max(dot(normalize(inNormal), normalize(lighting.lightDir.xyz)), 0.0);
	vec3 c = base.rgb * lighting.lightColor.rgb * ndl * kSpecValue;
	for (int i = 0; i < 4; i++) { c = c * 0.9 + fract(c * kPatchValue) * 0.1; }
	outColor = vec4(c, base.a);
}
