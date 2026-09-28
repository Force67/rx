// Scene-pass variant of mesh.ps with the skin-diffuse MRT export (SV_Target2).
// The blend pass keeps the plain mesh.ps so its two-attachment pipelines do
// not write a target that is not bound.
#define RX_SSS_MRT 1
#include "mesh.ps.hlsl"
