#ifndef RX_GI_MATERIAL_CLASS_HLSLI_
#define RX_GI_MATERIAL_CLASS_HLSLI_

// Per-pixel material class packed into the prepass normal target's .a channel
// (RGBA16F; the channel was previously written as 0 and read by nothing).
// Denoisers reject cross-class neighbours so wind-blown vegetation, skinned
// characters and translucency do not bleed their noisy indirect light onto
// opaque surfaces during spatial filtering -- AC Shadows' denoiser-mask fix.
// Values 0..3 also map 1:1 onto NRD's 2-bit A2 material-ID slot
// (NormalEncoding::R10_G10_B10_A2_UNORM), so the same id can feed IN_MATERIALID.

static const float kMatClassOpaque = 0.0;
static const float kMatClassVegetation = 1.0;   // kFlagWind: vertex wind sway (foliage/cloth)
static const float kMatClassCharacter = 2.0;    // kFlagSkin / kFlagHair: skinned characters
static const float kMatClassTranslucent = 3.0;  // blend mode (not emitted by the opaque prepass)

// Decode the stored class (nearest integer; the atlas is float so it is exact).
float RxMatClass(float packed) { return round(clamp(packed, 0.0, 3.0)); }
// Same-class hard test used as a denoiser bilateral weight (1 keep, 0 reject).
float RxMatClassMatch(float a, float b) { return RxMatClass(a) == RxMatClass(b) ? 1.0 : 0.0; }
bool RxIsVegetation(float packed) { return RxMatClass(packed) == kMatClassVegetation; }

#endif  // RX_GI_MATERIAL_CLASS_HLSLI_
