// Interposed in front of the SDK's ffx_types.h (this directory comes first
// on the include path; include_next continues to the real header). The SDK
// sizes its opaque context blobs for Windows where wchar_t is 2 bytes; with
// 4-byte wchar_t the private FSR3 upscaler context is ~820 KB and overflows
// the default 512 KB, failing a static_assert. Every consumer sees this
// header, so the public and private sizes stay consistent across TUs.
#pragma once

#include_next <FidelityFX/host/ffx_types.h>

#undef FFX_SDK_DEFAULT_CONTEXT_SIZE
#define FFX_SDK_DEFAULT_CONTEXT_SIZE (1024 * 256)
