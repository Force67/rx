# rx::gpu: the RHI (rhi/) and its backends (vulkan/, d3d12/, null/). One module:
# Device::Create picks the backend at runtime, so the interface and the
# backends it constructs are one unit; the build selects which backends exist.
rx_add_module(gpu
  rhi/rhi.cc
  rhi/gpu_profiler.cc
  null/null_device.cc
)

# Graphics backends. Vulkan is the default everywhere it can run; d3d12 is the
# native windows/xbox path, and on Linux it runs natively over vkd3d
# (D3D12-on-Vulkan) for cross-backend validation. The null backend is always
# built.
option(RX_RHI_VULKAN "Build the Vulkan render backend" ON)
if(WIN32)
  option(RX_RHI_D3D12 "Build the D3D12 render backend" ON)
else()
  # Default to ON when a D3D12 provider is available (the nix dev shell
  # provides both WineHQ vkd3d and vkd3d-proton).
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(VKD3D_PROBE QUIET libvkd3d libvkd3d-utils)
    if(NOT VKD3D_PROBE_FOUND)
      pkg_check_modules(VKD3D_PROBE QUIET libvkd3d-proton-d3d12)
    endif()
  endif()
  if(VKD3D_PROBE_FOUND)
    option(RX_RHI_D3D12 "Build the D3D12 render backend" ON)
  else()
    option(RX_RHI_D3D12 "Build the D3D12 render backend" OFF)
  endif()
endif()

if(RX_RHI_VULKAN)
  target_sources(rx_gpu PRIVATE
    vulkan/vk_device.cc
    vulkan/vk_swapchain.cc
    vulkan/vk_command_list.cc
    vulkan/vk_convert.cc
    vulkan/vk_interop.cc
    vulkan/vma_impl.cc
    rhi/shader_util.cc
  )
  # PUBLIC: interop consumers (gui backend, thumbnailer, upscalers) gate their
  # Vulkan paths on this define.
  target_compile_definitions(rx_gpu PUBLIC RX_RHI_VULKAN)
  # The device floor is a property of the shaders, not a taste: a driver that
  # cannot load the SPIR-V this build emits must be rejected at enumeration
  # rather than accepted and then failing every pipeline.
  if(RX_SPIRV_1_4)
    target_compile_definitions(rx_gpu PUBLIC RX_SPIRV_1_4=1)
  endif()
  # NGX's instance and device extensions (VK_NVX_binary_import among them)
  # must be enabled when the device is created, long before render's DLSS
  # code runs. Without them NGX calls a null vkCreateCuModuleNVX.
  if(RX_DLSS)
    target_compile_definitions(rx_gpu PRIVATE RX_HAS_DLSS)
    target_link_libraries(rx_gpu PRIVATE $<BUILD_INTERFACE:rx::dlss>)
  endif()
endif()

if(RX_RHI_D3D12)
  target_sources(rx_gpu PRIVATE
    d3d12/d3d12_device.cc
    d3d12/d3d12_command_list.cc
    d3d12/d3d12_swapchain.cc
    d3d12/d3d12_convert.cc
  )
  # PUBLIC: RX_SHADER grows the DXIL sidecar reference under this define, so
  # every consumer of the embed headers must agree.
  target_compile_definitions(rx_gpu PUBLIC RX_RHI_D3D12)
  # Linux D3D12 provider: WineHQ vkd3d (default) or vkd3d-proton, Proton's
  # implementation, which exposes DXR 1.1 / mesh shaders / SM 6.6 and so runs
  # the backend's ray tracing paths natively.
  option(RX_VKD3D_PROTON "Use vkd3d-proton as the Linux D3D12 provider" OFF)
  if(WIN32)
    target_link_libraries(rx_gpu PRIVATE d3d12 dxgi)
  elseif(RX_VKD3D_PROTON)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(VKD3D_PROTON REQUIRED IMPORTED_TARGET libvkd3d-proton-d3d12)
    target_compile_definitions(rx_gpu PRIVATE RX_VKD3D_PROTON)
    target_link_libraries(rx_gpu PRIVATE $<BUILD_INTERFACE:PkgConfig::VKD3D_PROTON>)
    set(RX_INSTALL_RHI_D3D12 ON PARENT_SCOPE)
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(VKD3D REQUIRED IMPORTED_TARGET libvkd3d libvkd3d-utils)
    # BUILD_INTERFACE: kept out of the installed export; rxConfig.cmake
    # re-resolves vkd3d via pkg-config for find_package consumers.
    target_link_libraries(rx_gpu PRIVATE $<BUILD_INTERFACE:PkgConfig::VKD3D>)
    set(RX_INSTALL_RHI_D3D12 ON PARENT_SCOPE)
  endif()
endif()

# volk / VMA are PUBLIC (their headers propagate through the RHI interop
# surface). Wrapped in BUILD_INTERFACE so they stay out of the installed export;
# rxConfig.cmake re-adds bundled imported equivalents for find_package consumers.
# Under RX_SHARED volk is compiled hidden (see the top-level CMakeLists), so each
# DSO that links it gets a private, non-interposing copy it initializes itself.
target_link_libraries(rx_gpu
  PUBLIC rx::foundation rx::window
         $<BUILD_INTERFACE:volk> $<BUILD_INTERFACE:GPUOpen::VulkanMemoryAllocator>)

if(NOT MSVC)
  set_source_files_properties(vulkan/vma_impl.cc PROPERTIES COMPILE_OPTIONS -w)
endif()

# The fullscreen blit the d3d12 backend presents with. The renderer's passes use
# the same pair, through rx::gpu.
rx_embed_shaders(rx_gpu
  shaders/fullscreen.vs.slang
  shaders/blit.ps.slang)
