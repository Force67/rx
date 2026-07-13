{
  description = "rx: a standalone real-time vulkan rendering engine";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

    # Same pins as the FetchContent declarations in CMakeLists.txt, so
    # `nix build` works inside the sandbox without network access.
    vulkan-headers-src = {
      url = "github:KhronosGroup/Vulkan-Headers/v1.4.304";
      flake = false;
    };
    volk-src = {
      url = "github:zeux/volk/1.4.304";
      flake = false;
    };
    vma-src = {
      url = "github:GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/v3.1.0";
      flake = false;
    };

    # third_party/NRD-MathLib FetchContent-fetches sse2neon on aarch64; under
    # FETCHCONTENT_FULLY_DISCONNECTED that fetch fails unless we hand it a
    # source dir. Pinned to the master ref NRD-MathLib declares.
    sse2neon-src = {
      url = "github:DLTcollab/sse2neon";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, vulkan-headers-src, volk-src, vma-src, sse2neon-src }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems
        (system: f nixpkgs.legacyPackages.${system});

      fetchContentFlags = [
        "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"
        "-DFETCHCONTENT_SOURCE_DIR_VULKANHEADERS=${vulkan-headers-src}"
        "-DFETCHCONTENT_SOURCE_DIR_VOLK=${volk-src}"
        "-DFETCHCONTENT_SOURCE_DIR_VULKANMEMORYALLOCATOR=${vma-src}"
        "-DFETCHCONTENT_SOURCE_DIR_SSE2NEON=${sse2neon-src}"
      ];

      # WineHQ vkd3d (native Linux D3D12-on-Vulkan), built from the release
      # tarball: the git tree needs wine's widl to generate the d3d12 headers,
      # which nixpkgs cannot supply on aarch64, while the dist tarball ships
      # them pregenerated. Provides libvkd3d (the D3D12 implementation),
      # libvkd3d-utils (D3D12CreateDevice & friends) and libvkd3d-shader with
      # the DXIL (SM6) frontend. Used to run the engine's d3d12 backend on
      # Linux.
      vkd3dFor = pkgs: pkgs.stdenv.mkDerivation rec {
        pname = "vkd3d";
        version = "2.0";
        src = pkgs.fetchurl {
          url = "https://dl.winehq.org/vkd3d/source/vkd3d-${version}.tar.xz";
          hash = "sha256-mtKbsjaAgYakfsZuhTsh5vylm53GKpR0sF1ePtonEO8=";
        };
        nativeBuildInputs = with pkgs; [ pkg-config bison flex perl perlPackages.JSON ];
        buildInputs = with pkgs; [ spirv-headers vulkan-headers vulkan-loader xorg.libxcb ];
        configureFlags = [ "--disable-tests" "--disable-doxygen-doc" ];
        enableParallelBuilding = true;
      };

      # vkd3d-proton (the Proton D3D12 implementation), built as native Linux
      # libraries. Unlike WineHQ vkd3d 2.0 it exposes DXR 1.1, mesh shaders and
      # SM 6.6, so the d3d12 backend's ray tracing paths can run on Linux.
      # Selected in CMake with -DRX_VKD3D_PROTON=ON.
      vkd3dProtonFor = pkgs: pkgs.stdenv.mkDerivation {
        pname = "vkd3d-proton";
        version = "3.0.1";
        src = pkgs.fetchFromGitHub {
          owner = "HansKristian-Work";
          repo = "vkd3d-proton";
          rev = "v3.0.1";
          fetchSubmodules = true;
          hash = "sha256-7f2Ups5GZMe2vCeb3xveuMnGFSZgk605oy9aUXhvhlk=";
        };
        # wine's widl generates the d3d12 headers from the .idl sources.
        nativeBuildInputs = with pkgs; [ meson ninja pkg-config glslang
                                         wineWow64Packages.minimal ];
        mesonFlags = [ "-Denable_tests=false" ];
        mesonAutoFeatures = "auto";
        enableParallelBuilding = true;
        # d3d12.so dlopens d3d12core.so from its own directory on the RUNPATH,
        # which meson drops at install; put it back so consumers only need to
        # link (or LD_LIBRARY_PATH) the front library.
        postFixup = ''
          find $out/lib -name libvkd3d-proton-d3d12.so | while read -r lib; do
            ${pkgs.patchelf}/bin/patchelf --add-rpath "$(dirname "$lib")" "$lib"
          done
        '';
      };
    in
    {
      packages = forAllSystems (pkgs: {
        default = pkgs.stdenv.mkDerivation {
          pname = "rx";
          version = "0.1.0";
          src = self;

          nativeBuildInputs = with pkgs; [ cmake ninja directx-shader-compiler pkg-config ];
          # wayland: libwayland-client for the KDE HDR-toggle monitor (linux).
          buildInputs = with pkgs; [ sdl3 ]
            ++ pkgs.lib.optionals pkgs.stdenv.isLinux [ pkgs.wayland ];

          cmakeFlags = fetchContentFlags;

          installPhase = ''
            runHook preInstall
            mkdir -p $out/bin
            cp runtime/rx $out/bin/
            runHook postInstall
          '';

          meta.mainProgram = "rx";
        };
      });

      devShells = forAllSystems (pkgs:
        let
          # Launcher for nix-built vulkan binaries. volk dlopens
          # libvulkan.so.1, which is never linked, so the loader has to be
          # on the search path; the host GPU driver (the ICD, e.g.
          # libGLX_nvidia) lives in the system lib dir and was built
          # against the host glibc, so nix's glibc and libstdc++ go first
          # and everything resolves against the newer ones. Only the
          # driver's own libraries are exposed, through a symlink dir:
          # putting the whole host lib dir on LD_LIBRARY_PATH shadows nix
          # libraries (SDL's libxkbcommon, etc.) with older host ones.
          # This must not leak into the whole shell either: host binaries
          # run with the host ld.so and crash when handed nix's libc.
          vkrun = pkgs.writeShellScriptBin "vkrun" ''
            driver_libs="''${XDG_RUNTIME_DIR:-/tmp}/rx-driver-libs"
            mkdir -p "$driver_libs"
            # /run/opengl-driver/lib is the NixOS driver dir; the Debian-style
            # paths cover other hosts. The nvidia libs land in the symlink dir so
            # the loader can resolve them by soname without the whole dir on the
            # path. libnvidia-ngx (the NGX core behind DLSS) is dlopened this way.
            # libcuda/libnvcuextend too: the driver's ray tracing stack dlopens
            # libcuda.so.1 (observed on GB10), and without it vkCreateDevice with
            # the acceleration-structure extensions fails INITIALIZATION_FAILED.
            for f in /usr/lib/*-linux-gnu/libnvidia*.so* /usr/lib/*-linux-gnu/libGLX_nvidia.so* \
                     /usr/lib/*-linux-gnu/libEGL_nvidia.so* \
                     /usr/lib/*-linux-gnu/libcuda.so* /usr/lib/*-linux-gnu/libnvcuextend.so* \
                     /run/opengl-driver/lib/libnvidia*.so* /run/opengl-driver/lib/libGLX_nvidia.so* \
                     /run/opengl-driver/lib/libEGL_nvidia.so* \
                     /run/opengl-driver/lib/libcuda.so* /run/opengl-driver/lib/libnvcuextend.so*; do
              [ -e "$f" ] && ln -sf "$f" "$driver_libs/"
            done
            if [ -e /usr/share/vulkan/icd.d/nvidia_icd.json ]; then
              export VK_DRIVER_FILES=''${VK_DRIVER_FILES:-/usr/share/vulkan/icd.d/nvidia_icd.json}
            fi
            # X client libs, libglvnd (the driver dlopens libEGL.so.1
            # during init) and libbsd are needed by libGLX_nvidia and ABI
            # stable, nix's copies serve both worlds.
            export LD_LIBRARY_PATH=${pkgs.lib.makeLibraryPath [
              pkgs.glibc
              pkgs.stdenv.cc.cc.lib
              pkgs.vulkan-loader
              pkgs.libglvnd
              pkgs.libx11
              pkgs.libxext
              pkgs.libxcb
              pkgs.libxau
              pkgs.libxdmcp
              pkgs.libbsd
              pkgs.libmd
            ]}:"$driver_libs"''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
            exec "$@"
          '';

          # Full software path: an Xvfb display plus mesa's lavapipe ICD.
          # The GB10 driver has no headless surface and its X11 WSI needs
          # the nvidia X driver, so windowed runs on a virtual display go
          # through lavapipe. CPU rendering, but it executes the entire
          # frame for validation.
          swrun = pkgs.writeShellScriptBin "swrun" ''
            ${pkgs.xorg.xvfb}/bin/Xvfb :99 -screen 0 1280x720x24 &
            xvfb_pid=$!
            trap 'kill $xvfb_pid 2>/dev/null' EXIT
            sleep 0.5
            export DISPLAY=:99
            export VK_DRIVER_FILES=${pkgs.mesa}/share/vulkan/icd.d/lvp_icd.${pkgs.stdenv.hostPlatform.uname.processor}.json
            # lavapipe's threaded acceleration structure builds race and
            # segfault (observed on aarch64, mesa 25 / llvm 21); a single
            # rasterizer thread is slow but deterministic.
            export LP_NUM_THREADS=''${LP_NUM_THREADS:-1}
            export LD_LIBRARY_PATH=${pkgs.lib.makeLibraryPath [
              pkgs.vulkan-loader
            ]}''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
            "$@"
          '';
        in
        {
          default = pkgs.mkShell {
            packages = with pkgs; [
              cmake
              ninja
              gdb
              directx-shader-compiler  # dxc, hlsl -> spirv
              glslang                  # FidelityFX shader permutations
              pkg-config
              sdl3
              freetype                 # libultragui text rasterization (editor UI)
              harfbuzz                 # libultragui text shaping (editor UI)
              openssl                  # zetanet crypto backend (optional net sessions)
              wayland                  # libwayland-client, KDE HDR-toggle monitor
              vulkan-headers
              vulkan-loader
              vulkan-validation-layers
              vulkan-tools
              (vkd3dFor pkgs)         # native D3D12-on-Vulkan, for the d3d12 rhi backend
              (vkd3dProtonFor pkgs)   # proton d3d12 (DXR/mesh/SM6.6), RX_VKD3D_PROTON=ON
              vkrun
              swrun
            ];

            shellHook = ''
              export VK_LAYER_PATH=${pkgs.vulkan-validation-layers}/share/vulkan/explicit_layer.d
              export RX_FETCHCONTENT_FLAGS="${builtins.concatStringsSep " " fetchContentFlags}"

              if [ -z "''${DISPLAY:-}" ] && [ -S /tmp/.X11-unix/X0 ]; then
                export DISPLAY=:0
              fi
            '';
          };
        });
    };
}
