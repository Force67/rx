// Deriving normals and bounding radii from a model matrix.
//
// Both operations here read the matrix by COLUMN. HLSL's m[i] is the i-th
// logical row no matter what the storage layout says, so columns come out of
// the transpose. Getting that backwards fails silently: the rows of a
// rotation * scale are plausible-looking mixtures of the per-axis scales, so
// the result stays finite and merely wrong.
#ifndef RX_MODEL_TRANSFORM_HLSLI_
#define RX_MODEL_TRANSFORM_HLSLI_

// The columns of a model matrix are the images of the object-space axes, so
// for any rotation * scale their lengths are exactly the per-axis scales.
float3 RxAxisScales(float3x3 m) {
  const float3x3 c = transpose(m);
  return float3(length(c[0]), length(c[1]), length(c[2]));
}

// A bounding sphere takes the largest scale, so a non-uniform transform never
// shrinks the sphere below the geometry it has to enclose. Mirrors
// render::MaxScale (render/gi/rt_instance_cull.cc); the GPU culls must agree
// with the CPU ones or they discard geometry the CPU kept.
float RxMaxAxisScale(float3x3 m) {
  const float3 s = RxAxisScales(m);
  return max(s.x, max(s.y, s.z));
}

// Whether a meshlet's normal cone survives this transform. A cone is an axis
// plus an angular spread; a rotation times a uniform scale rotates the axis and
// leaves the spread alone, so the cheap raw-matrix transform is exact there. A
// non-uniform scale tilts the axis AND widens the spread by different amounts
// per direction, and no single corrected axis stays conservative, so a cone
// test would start discarding meshlets that are actually facing the camera.
// Backface culling is an optimization, never a correctness requirement, so the
// answer is to stop cone-culling rather than to approximate it. The tolerance
// only has to absorb float noise in a nominally uniform matrix.
bool RxConeCullValid(float3x3 m) {
  const float3 s = RxAxisScales(m);
  return max(s.x, max(s.y, s.z)) <= 1.05 * min(s.x, min(s.y, s.z));
}

// Cofactor matrix (adjugate transpose) of a transform's linear part, that is
// det(m) * m^-T: the matrix that carries covectors. A normal is a covector, so
// under a non-uniform scale m tilts it off the surface while the cofactor
// keeps it perpendicular; under a rotation or a uniform scale the two agree in
// direction and differ only in length. Column i is the cross product of the
// other two columns of m, which yields the determinant on the way, so no
// inverse is needed - about 30 flops on a matrix that is wave-uniform in
// registers anyway.
//
// Deliberately fp32: cofactor entries scale as s^2, which overflows fp16.
float3x3 RxCofactor(float3x3 m, out float det) {
  const float3x3 c = transpose(m);  // the transpose's rows are m's columns
  const float3 k0 = cross(c[1], c[2]);
  const float3 k1 = cross(c[2], c[0]);
  const float3 k2 = cross(c[0], c[1]);
  det = dot(c[0], k0);
  return transpose(float3x3(k0, k1, k2));  // k* are the cofactor's columns
}

// For callers that only need the direction, chiefly ray hits that force the
// normal to face the ray and so discard the sign regardless.
float3x3 RxCofactor(float3x3 m) {
  float det;
  return RxCofactor(m, det);
}

// +1, or -1 for a mirrored (negative determinant) instance. The raw cofactor
// agrees with the mirrored winding, which points the normal INTO the surface;
// this factor recovers the outward m^-T direction. Tangent handedness takes
// the same factor, because a mirror swaps the side the bitangent falls on and
// the vertex data is shared with unmirrored instances of the same mesh.
//
// Not sign(): a degenerate (det == 0) transform would collapse the normal and
// the handedness to zero, and zero handedness makes the pixel stage's
// cross(n, t) * w bitangent vanish rather than merely be arbitrary.
float RxMirrorSign(float det) { return det < 0.0 ? -1.0 : 1.0; }

#endif  // RX_MODEL_TRANSFORM_HLSLI_
