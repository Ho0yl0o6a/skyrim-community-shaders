// Resolves the sky the game renders into its reflections cubemap onto the
// equirectangular image Remix's dome light samples.
//
// The mapping is the exact inverse of the runtime's, in
// rtx/utility/math.slangh:
//
//   theta = acos(direction.z)
//   phi   = atan2(direction.x, direction.y)
//   uv    = float2(0.5 + phi / 2pi, theta / pi)
//
// so a texel's direction is recovered by solving that for direction. Both
// spaces are Z-up, which is why the dome needs no rotation to sit right.

TextureCube<float4> SkyCubemap : register(t0);
RWTexture2D<float4> LatLong : register(u0);
SamplerState LinearSampler : register(s0);

static const float kPi = 3.14159265358979323846;
static const float kTwoPi = 6.28318530717958647692;

// Skyrim writes its sky into an 8-bit target without a gamma-corrected format,
// so the values are gamma encoded and have to be brought into linear before
// they can be used as radiance.
float3 GammaToLinear(float3 color)
{
	const float3 low = color / 12.92;
	const float3 high = pow(abs(color + 0.055) / 1.055, 2.4);
	return lerp(high, low, step(color, 0.04045));
}

[numthreads(8, 8, 1)] void main(uint3 threadId
								: SV_DispatchThreadID) {
	uint width, height;
	LatLong.GetDimensions(width, height);
	if (threadId.x >= width || threadId.y >= height)
		return;

	const float2 uv = (float2(threadId.xy) + 0.5) / float2(width, height);

	const float theta = uv.y * kPi;
	const float phi = (uv.x - 0.5) * kTwoPi;

	const float sinTheta = sin(theta);

	// Inverse of the runtime's mapping above.
	float3 direction;
	direction.z = cos(theta);
	direction.x = sinTheta * sin(phi);
	direction.y = sinTheta * cos(phi);

	const float3 sky = SkyCubemap.SampleLevel(LinearSampler, direction, 0).rgb;

	LatLong[threadId.xy] = float4(GammaToLinear(sky), 1.0);
}
