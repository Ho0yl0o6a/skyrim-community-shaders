#pragma once

#include "Buffer.h"

// Gives Remix the sky Skyrim itself renders.
//
// The engine already draws its whole sky -- atmosphere, clouds, stars, sun,
// moons and aurora, blended for the current weather and time of day -- into its
// reflections cubemap, one face per frame. Rather than describing that sky to
// Remix as geometry, which would put an opaque shell between the sun and the
// world, the cube is resolved into the equirectangular image Remix's dome light
// already expects and handed over as a texture.
//
// The dome is sampled by every ray that escapes the scene, so it is both the
// visible background and the scene's image-based lighting. It is not sampled
// directly by shadow or next-event rays, so the sun and moons stay separate
// analytic lights.
namespace RemixSky
{
	// Resolves this frame's sky and hands it to Remix. Does nothing where the
	// game shows no sky, which leaves Remix's dome inactive and interiors dark
	// until their own lights arrive.
	void Submit();

	// Drops the resolved image and the shader. The textures the game owns are
	// not touched.
	void Release();
}
