// ParallaxTest_Weights.hlsl
// Test material only (M_ParallaxTest): stands in for the biome atlases. Two
// chosen layers, blended in soft world-space patches, so the surface takes the
// same "two strongest layers + height mix" path as on the planet.
//
// Custom node settings:
//   Output Type        CMOT Float 4  (-> W0 of the surface node)
//   Inputs, in this order:
//     LayerA    Scalar Parameter "LayerA"   (0 Sand .. 7 Rock)
//     LayerB    Scalar Parameter "LayerB"
//     Mix       Scalar Parameter "LayerMix" (0 = only LayerA, 1 = patches of A and B)
//     BlendUV   Frac(Absolute World Position / LayerBlendScale)
//   Additional Outputs:
//     OutW1     CMOT Float 4  (-> W1 of the surface node)
// Paste everything below the dashed line into the node's Code field.
// -----------------------------------------------------------------------------
float W[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
int A = clamp((int)round(LayerA), 0, 7);
int B = clamp((int)round(LayerB), 0, 7);
float3 P = BlendUV * 6.2831853;
float F = sin(P.x) + sin(P.y + 1.3) + sin(P.z + 2.1);
float Blend = smoothstep(-0.5, 0.5, F) * saturate(Mix);
W[A] += 1.0 - Blend;
W[B] += Blend;
OutW1 = float4(W[4], W[5], W[6], W[7]);
return float4(W[0], W[1], W[2], W[3]);
