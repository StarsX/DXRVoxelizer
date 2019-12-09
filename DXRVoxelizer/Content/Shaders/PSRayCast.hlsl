//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SharedConst.h"

#define NUM_SAMPLES			128
#define NUM_LIGHT_SAMPLES	32
#define ABSORPTION			1.0
#define ZERO_THRESHOLD		0.01
#define ONE_THRESHOLD		0.999

//--------------------------------------------------------------------------------------
// Constant buffers
//--------------------------------------------------------------------------------------
/*cbuffer cbImmutable
{
	float4  g_vViewport;
	float4  g_vDirectional;
	float4  g_vAmbient;
};*/

cbuffer cbPerObject
{
	float3	g_localSpaceLightPt;
	float3	g_localSpaceEyePt;
	matrix	g_screenToLocal;
};

//static const float3 g_vLightRad = g_vDirectional.xyz * g_vDirectional.w;	// 4.0
//static const float3 g_vAmbientRad = g_vAmbient.xyz * g_vAmbient.w;			// 1.0

static const min16float g_maxDist = 2.0 * sqrt(3.0);
static const min16float g_stepScale = g_maxDist / NUM_SAMPLES;
static const min16float g_lightStepScale = g_maxDist / NUM_LIGHT_SAMPLES;

static const min16float3 g_clearColor = min16float3(CLEAR_COLOR);

//--------------------------------------------------------------------------------------
// Textures
//--------------------------------------------------------------------------------------
#if	USE_MUTEX
Texture3D<float>	g_txGrid;
#else
Texture3D			g_txGrid;
#endif

//--------------------------------------------------------------------------------------
// Unordered access textures
//--------------------------------------------------------------------------------------
RWTexture2D<float4>	g_rwPresent;

//--------------------------------------------------------------------------------------
// Texture samplers
//--------------------------------------------------------------------------------------
SamplerState		g_smpLinear;

//--------------------------------------------------------------------------------------
// Screen space to loacal space
//--------------------------------------------------------------------------------------
float3 ScreenToLocal(float3 screenPos)
{
	float4 pos = mul(float4(screenPos, 1.0), g_screenToLocal);
	
	return pos.xyz / pos.w;
}

//--------------------------------------------------------------------------------------
// Compute start point of the ray
//--------------------------------------------------------------------------------------
bool ComputeStartPoint(inout float3 pos, float3 rayDir)
{
	if (abs(pos.x) <= 1.0 && abs(pos.y) <= 1.0 && abs(pos.z) <= 1.0) return true;

	//float U = asfloat(0x7f800000);	// INF
	float U = 3.402823466e+38;			// FLT_MAX
	bool isHit = false;

	[unroll]
	for (uint i = 0; i < 3; ++i)
	{
		const float u = (-sign(rayDir[i]) - pos[i]) / rayDir[i];
		if (u < 0.0h) continue;

		const uint j = (i + 1) % 3, k = (i + 2) % 3;
		if (abs(rayDir[j] * u + pos[j]) > 1.0) continue;
		if (abs(rayDir[k] * u + pos[k]) > 1.0) continue;
		if (u < U)
		{
			U = u;
			isHit = true;
		}
	}

	pos = clamp(rayDir * U + pos, -1.0, 1.0);

	return isHit;
}

//--------------------------------------------------------------------------------------
// Sample density field
//--------------------------------------------------------------------------------------
min16float GetSample(float3 tex)
{
#if	USE_MUTEX
	const min16float density = min16float(g_txGrid.SampleLevel(g_smpLinear, tex, SHOW_MIP).x);
#else
	const min16float density = min16float(g_txGrid.SampleLevel(g_smpLinear, tex, SHOW_MIP).w);
#endif

	return min(density * 8.0, 16.0);
}

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
min16float4 main(float4 sspos : SV_POSITION) : SV_TARGET
{
	float3 pos = ScreenToLocal(float3(sspos.xy, 0.0));	// The point on the near plane
	const float3 rayDir = normalize(pos - g_localSpaceEyePt);
	if (!ComputeStartPoint(pos, rayDir)) return min16float4(g_clearColor, 0.0);

	const float3 step = rayDir * g_stepScale;

#ifndef _POINT_LIGHT_
	const float3 lightStep = normalize(g_localSpaceLightPt) * g_lightStepScale;
#endif

	// Transmittance
	min16float transmit = 1.0;
	// In-scattered radiance
	min16float scatter = 0.0;

	for (uint i = 0; i < NUM_SAMPLES; ++i)
	{
		if (abs(pos.x) > 1.0 || abs(pos.y) > 1.0 || abs(pos.z) > 1.0) break;
		float3 tex = float3(0.5, -0.5, 0.5) * pos + 0.5;

		// Get a sample
		const min16float density = GetSample(tex);

		// Skip empty space
		if (density > ZERO_THRESHOLD)
		{
			// Attenuate ray-throughput
			const min16float scaledDens = density * g_stepScale;
			transmit *= saturate(1.0 - scaledDens * ABSORPTION);
			if (transmit < ZERO_THRESHOLD) break;

			// Point light direction in texture space
#ifdef _POINT_LIGHT_
			const float3 lightStep = normalize(g_localSpaceLightPt - pos) * g_lightStepScale;
#endif

			// Sample light
			min16float lightTrans = 1.0;	// Transmittance along light ray
			float3 lightPos = pos + lightStep;

			for (uint j = 0; j < NUM_LIGHT_SAMPLES; ++j)
			{
				if (abs(lightPos.x) > 1.0 || abs(lightPos.y) > 1.0 || abs(lightPos.z) > 1.0) break;
				tex = min16float3(0.5, -0.5, 0.5) * lightPos + 0.5;

				// Get a sample along light ray
				const min16float lightDens = GetSample(tex);

				// Attenuate ray-throughput along light direction
				lightTrans *= saturate(1.0 - ABSORPTION * g_lightStepScale * lightDens);
				if (lightTrans < ZERO_THRESHOLD) break;

				// Update position along light ray
				lightPos += lightStep;
			}

			scatter += lightTrans * transmit * scaledDens;
		}

		pos += step;
	}

	//clip(ONE_THRESHOLD - transmit);

	min16float3 result = scatter * 0.8 + 0.2;
	result = lerp(result, g_clearColor * g_clearColor, transmit);
	
	return min16float4(sqrt(result), 1.0);
}
