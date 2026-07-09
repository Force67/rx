# Runs the FidelityFX shader compiler for one fsr3upscaler pass variant.
# Invoked as a -P script so the {0,1} permutation arguments never pass
# through a shell (ninja's /bin/sh is bash, which would brace-expand them).
#
# Expects: FFX_SC, GLSLANG, GPU_DIR, OUT_DIR, NAME, HALF, SHADER
# Argument set mirrors the SDK's CMakeShadersFSR3Upscaler.txt and
# CMakeCompileFSR3UpscalerShaders.txt (glslang/Vulkan path).

execute_process(
  COMMAND ${FFX_SC}
    -reflection
    -DFFX_GPU=1
    -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0
    -DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0
    -DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1
    -DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0
    -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2
    -compiler=glslang -e CS --target-env vulkan1.2 -S comp -Os -DFFX_GLSL=1
    "-DFFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE={0,1}"
    "-DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT={0,1}"
    "-DFFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS={0,1}"
    "-DFFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS={0,1}"
    "-DFFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH={0,1}"
    "-DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING={0,1}"
    -glslangexe=${GLSLANG}
    -I${GPU_DIR}
    -I${GPU_DIR}/fsr3upscaler
    -name=${NAME}
    -DFFX_HALF=${HALF}
    -output=${OUT_DIR}
    ${SHADER}
  RESULT_VARIABLE result
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err)

if(NOT result EQUAL 0)
  message(FATAL_ERROR "ffx_sc failed for ${NAME} (${result})\n--- stdout ---\n${out}\n--- stderr ---\n${err}")
endif()
