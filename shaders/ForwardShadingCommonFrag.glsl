// Copyright (C) 2009-2019, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

// Common code for all fragment shaders of FS
#include <shaders/Common.glsl>
#include <shaders/Functions.glsl>

// Global resources
layout(ANKI_TEX_BINDING(0, 0)) uniform sampler2D u_gbufferDepthRt;
layout(ANKI_TEX_BINDING(0, 1)) uniform sampler3D u_lightVol;
#define LIGHT_SET 0
#define LIGHT_UBO_BINDING 0
#define LIGHT_SS_BINDING 0
#define LIGHT_TEX_BINDING 2
#define LIGHT_LIGHTS
#define LIGHT_COMMON_UNIS
#include <shaders/ClusteredShadingCommon.glsl>

#define anki_u_time u_time
#define RENDERER_SIZE (u_rendererSize)

layout(location = 0) out Vec4 out_color;

void writeGBuffer(Vec4 color)
{
	out_color = Vec4(color.rgb, color.a);
}

Vec4 readAnimatedTextureRgba(sampler2DArray tex, F32 period, Vec2 uv, F32 time)
{
	const F32 layerCount = F32(textureSize(tex, 0).z);
	const F32 layer = mod(time * layerCount / period, layerCount);
	return texture(tex, Vec3(uv, layer));
}

// Iterate the clusters to compute the light color
Vec3 computeLightColorHigh(Vec3 diffCol, Vec3 worldPos)
{
	diffCol = diffuseLambert(diffCol);
	Vec3 outColor = Vec3(0.0);

	// Find the cluster and then the light counts
	const U32 clusterIdx = computeClusterIndex(
		u_clustererMagic, gl_FragCoord.xy / RENDERER_SIZE, worldPos, u_clusterCountX, u_clusterCountY);

	U32 idxOffset = u_clusters[clusterIdx];

	// Point lights
	U32 idx;
	ANKI_LOOP while((idx = u_lightIndices[idxOffset++]) != MAX_U32)
	{
		const PointLight light = u_pointLights[idx];

		const Vec3 diffC = diffCol * light.m_diffuseColor;

		const Vec3 frag2Light = light.m_position - worldPos;
		const F32 att = computeAttenuationFactor(light.m_squareRadiusOverOne, frag2Light);

#if LOD > 1
		const F32 shadow = 1.0;
#else
		F32 shadow = 1.0;
		if(light.m_shadowAtlasTileScale >= 0.0)
		{
			shadow = computeShadowFactorPointLight(light, frag2Light, u_shadowTex);
		}
#endif

		outColor += diffC * (att * shadow);
	}

	// Spot lights
	ANKI_LOOP while((idx = u_lightIndices[idxOffset++]) != MAX_U32)
	{
		const SpotLight light = u_spotLights[idx];

		const Vec3 diffC = diffCol * light.m_diffuseColor;

		const Vec3 frag2Light = light.m_position - worldPos;
		const F32 att = computeAttenuationFactor(light.m_squareRadiusOverOne, frag2Light);

		const Vec3 l = normalize(frag2Light);

		const F32 spot = computeSpotFactor(l, light.m_outerCos, light.m_innerCos, light.m_dir);

#if LOD > 1
		const F32 shadow = 1.0;
#else
		F32 shadow = 1.0;
		const F32 shadowmapLayerIdx = light.m_shadowmapId;
		if(shadowmapLayerIdx >= 0.0)
		{
			shadow = computeShadowFactorSpotLight(light, worldPos, u_shadowTex);
		}
#endif

		outColor += diffC * (att * spot * shadow);
	}

	return outColor;
}

// Just read the light color from the vol texture
Vec3 computeLightColorLow(Vec3 diffCol, Vec3 worldPos)
{
	const Vec2 uv = gl_FragCoord.xy / RENDERER_SIZE;
	const Vec3 uv3d = computeClustererVolumeTextureUvs(u_clustererMagic, uv, worldPos, u_lightVolumeLastCluster + 1u);

	const Vec3 light = textureLod(u_lightVol, uv3d, 0.0).rgb;
	return diffuseLambert(diffCol) * light;
}

void particleAlpha(Vec4 color, Vec4 scaleColor, Vec4 biasColor)
{
	writeGBuffer(color * scaleColor + biasColor);
}

void fog(Vec3 color, F32 fogAlphaScale, F32 fogDistanceOfMaxThikness, F32 zVSpace)
{
	const Vec2 screenSize = 1.0 / RENDERER_SIZE;

	const Vec2 texCoords = gl_FragCoord.xy * screenSize;
	const F32 depth = texture(u_gbufferDepthRt, texCoords).r;
	F32 zFeatherFactor;

	const Vec4 fragPosVspace4 = u_invProjMat * Vec4(Vec3(UV_TO_NDC(texCoords), depth), 1.0);
	const F32 sceneZVspace = fragPosVspace4.z / fragPosVspace4.w;

	const F32 diff = max(0.0, zVSpace - sceneZVspace);

	zFeatherFactor = min(1.0, diff / fogDistanceOfMaxThikness);

	writeGBuffer(Vec4(color, zFeatherFactor * fogAlphaScale));
}
