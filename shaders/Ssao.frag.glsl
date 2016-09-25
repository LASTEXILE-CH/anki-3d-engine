// Copyright (C) 2009-2016, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

// SSAO fragment shader
#include "shaders/Common.glsl"
#include "shaders/Pack.glsl"
#include "shaders/Functions.glsl"

const float RANGE_CHECK_RADIUS = RADIUS * 2.0;

// Initial is 1.0 but the bigger it is the more darker the SSAO factor gets
const float DARKNESS_MULTIPLIER = 1.0;

// The algorithm will chose the number of samples depending on the distance
const float MAX_DISTANCE = 40.0;

layout(location = 0) in vec2 in_texCoords;

layout(location = 0) out float out_color;

layout(ANKI_UBO_BINDING(0, 0), std140, row_major) uniform _blk
{
	vec4 u_projectionParams;
	vec4 u_projectionMat;
};

layout(ANKI_TEX_BINDING(0, 0)) uniform sampler2D u_mMsDepthRt;
layout(ANKI_TEX_BINDING(0, 1)) uniform sampler2D u_msRt;
layout(ANKI_TEX_BINDING(0, 2)) uniform sampler2D u_noiseMap;
layout(ANKI_TEX_BINDING(0, 3)) uniform sampler2DArray u_hemisphereLut;

// Get normal
vec3 readNormal(in vec2 uv)
{
	vec3 normal;
	readNormalFromGBuffer(u_msRt, uv, normal);
	return normal;
}

// Read the noise tex
vec3 readRandom(in vec2 uv)
{
	const vec2 tmp = vec2(float(WIDTH) / float(NOISE_MAP_SIZE), float(HEIGHT) / float(NOISE_MAP_SIZE));

	vec3 noise = texture(u_noiseMap, tmp * uv).xyz;
	// return normalize(noise * 2.0 - 1.0);
	return noise;
}

// Returns the Z of the position in view space
float readZ(in vec2 uv)
{
	float depth = texture(u_mMsDepthRt, uv).r;
	float z = u_projectionParams.z / (u_projectionParams.w + depth);
	return z;
}

// Read position in view space
vec3 readPosition(in vec2 uv)
{
	vec3 fragPosVspace;
	fragPosVspace.z = readZ(uv);

	fragPosVspace.xy = (2.0 * uv - 1.0) * u_projectionParams.xy * fragPosVspace.z;

	return fragPosVspace;
}

void main(void)
{
	vec3 origin = readPosition(in_texCoords);

	vec3 normal = readNormal(in_texCoords);
	vec3 randRadius = readRandom(in_texCoords);

	float theta = atan(normal.y, normal.x); // [-pi, pi]
	// Now move theta to [0, 2*pi]. Adding 2*pi gives the same angle. Then fmod to move back to [0, 2*pi]
	theta = mod(theta + 2.0 * PI, 2.0 * PI);

	float phi = acos(normal.z / 1.0); // [0, PI]

	vec2 lutCoords;
	lutCoords.x = theta / (2.0 * PI);
	lutCoords.y = phi / PI;
	lutCoords = clamp(lutCoords, 0.0, 1.0);

	// Iterate kernel
	float factor = 0.0;
	for(uint i = 0U; i < KERNEL_SIZE; ++i)
	{
		vec3 hemispherePoint = texture(u_hemisphereLut, vec3(lutCoords, float(i))).xyz;
		hemispherePoint = normalize(hemispherePoint);
		hemispherePoint = hemispherePoint * randRadius + origin;

		// project sample position:
		vec4 projHemiPoint = projectPerspective(
			vec4(hemispherePoint, 1.0), u_projectionMat.x, u_projectionMat.y, u_projectionMat.z, u_projectionMat.w);
		projHemiPoint.xy = projHemiPoint.xy / (2.0 * projHemiPoint.w) + 0.5; // persp div & to NDC -> [0, 1]

		// get sample depth:
		float sampleZ = readZ(projHemiPoint.xy);

		// Range check
		float rangeCheck = abs(origin.z - sampleZ) / RANGE_CHECK_RADIUS;
		rangeCheck = 1.0 - clamp(rangeCheck, 0.0, 1.0);

		// Accumulate
		const float ADVANCE = DARKNESS_MULTIPLIER / float(KERNEL_SIZE);
		float f = ceil(sampleZ - hemispherePoint.z);
		f = clamp(f, 0.0, 1.0) * ADVANCE;

		factor += f * rangeCheck;
	}

	out_color = 1.0 - factor;
}
