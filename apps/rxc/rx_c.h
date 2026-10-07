#ifndef RX_APPS_RXC_RX_C_H_
#define RX_APPS_RXC_RX_C_H_

// rxc: a C ABI over rx::app::Host for games written in other languages.
//
// The game owns the simulation; rx owns the window, the GPU, audio and the
// frame loop. Content is created by value (vertices, pixels, samples) and
// referred to by small integer handles. Per-frame calls (camera, draws,
// lights, hud) are valid only inside the build_view callback, except the hud
// calls, which are also valid in update.
//
// Every handle is nonzero; 0 means "none" or "failed".

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define RXC_API __declspec(dllexport)
#else
#define RXC_API __attribute__((visibility("default")))
#endif

// ---------------------------------------------------------------- lifecycle

typedef struct rxc_callbacks {
  void *user;
  // Subsystems are up. Create content here. Returning 0 aborts startup.
  int32_t (*init)(void *user);
  // Once per drawn frame (and once per iteration when headless): input, sim.
  void (*update)(void *user, float dt);
  // Once per drawn frame, after update: camera, draws, lights, hud.
  void (*build_view)(void *user, float dt);
  // The renderer is still alive; release nothing the engine owns.
  void (*shutdown)(void *user);
} rxc_callbacks;

typedef struct rxc_config {
  const char *id;    // a cuid2, unique across rx apps
  const char *name;  // slug [a-z0-9_-]: the vfs namespace and settings folder
  const char *title; // the window title
  uint32_t width;    // 0: RX_WIN_W or the window default
  uint32_t height;
  // No window. With offscreen set the renderer still draws, so a run without a
  // display can capture pngs.
  int32_t headless;
  int32_t offscreen;
  // > 0: every frame advances exactly this much (deterministic captures).
  float fixed_delta;
  // 0 auto, 1 low, 2 medium, 3 high, 4 ultra.
  int32_t quality;
  int32_t raytracing;
  int32_t vsync;
} rxc_config;

// Runs the frame loop until rxc_quit or the window closes. Returns the
// process exit code: 0 on a clean exit, nonzero if startup failed.
RXC_API int32_t rxc_run(const rxc_config *config,
                        const rxc_callbacks *callbacks);
RXC_API void rxc_quit(void);
// Seconds since rxc_run started (the host clock, lockstep under fixed_delta).
RXC_API double rxc_time(void);

// ---------------------------------------------------------------- look

typedef struct rxc_look {
  float sun_direction[3]; // direction the light travels
  float sun_color[3];
  float sun_intensity;
  float ambient;
  float exposure; // ev bias, used when auto_exposure is 0
  int32_t auto_exposure;
  float bloom_intensity;
  float vignette;
  float film_grain;
  float chromatic_aberration;
  int32_t sky; // draw the physical sky
  // Indoor lighting: no sky contribution, flat ambient, linear fog.
  int32_t interior;
  float interior_ambient[3];
  float fog_near;
  float fog_far;
  float fog_color[3];
  int32_t ssao;
  int32_t ssr;
  int32_t motion_blur;
} rxc_look;

// Fills `look` with the renderer's current values.
RXC_API void rxc_get_look(rxc_look *look);
RXC_API void rxc_set_look(const rxc_look *look);

// ---------------------------------------------------------------- content

typedef struct rxc_vertex {
  float position[3];
  float normal[3];
  float uv[2];
  uint32_t color; // 0xAABBGGRR, multiplies the base color
} rxc_vertex;

typedef struct rxc_material_desc {
  float base_color[4];
  float roughness;
  float metallic;
  float emissive[3];           // hdr, may exceed 1
  uint32_t base_color_texture; // 0: none
  uint32_t emissive_texture;   // 0: none
  float clearcoat;
  float specular; // specular strength, 1 is physical
  int32_t two_sided;
  int32_t blend; // alpha blended instead of opaque
} rxc_material_desc;

// rgba8 pixels, row major, w*h*4 bytes. srgb for color data.
RXC_API uint32_t rxc_texture_rgba8(uint32_t width, uint32_t height,
                                   const uint8_t *pixels, int32_t srgb);
RXC_API uint32_t rxc_material(const rxc_material_desc *desc);
RXC_API void rxc_material_update(uint32_t material,
                                 const rxc_material_desc *desc);
// One material per mesh. Tangents are derived from the uvs. A dynamic mesh may
// change its vertices (same count) through rxc_mesh_update between frames.
RXC_API uint32_t rxc_mesh(const rxc_vertex *vertices, uint32_t vertex_count,
                          const uint32_t *indices, uint32_t index_count,
                          uint32_t material, int32_t dynamic);
RXC_API int32_t rxc_mesh_update(uint32_t mesh, const rxc_vertex *vertices,
                                uint32_t vertex_count);

// ---------------------------------------------------------------- frame

// Column-major 4x4 world matrix, metres, +Y up.
RXC_API void rxc_camera(const float eye[3], const float target[3], float fov_y,
                        int32_t cut);
// `key` identifies the draw across frames for motion vectors: the same key
// next frame means the same object moved. 0 means static.
RXC_API void rxc_draw(uint32_t mesh, const float transform[16],
                      uint32_t tint_rgb, uint64_t key);

typedef struct rxc_light {
  float position[3];
  float radius; // influence radius, metres
  float color[3];
  float intensity;
  int32_t type;       // 0 point, 1 spot
  float direction[3]; // spot axis
  float cos_inner;
  float cos_outer;
} rxc_light;

RXC_API void rxc_add_light(const rxc_light *light);
// A camera-facing sprite. Emissive particles blend additively.
RXC_API void rxc_particle(const float position[3], float size,
                          const float rgba[4]);
RXC_API void rxc_particles_emissive(int32_t emissive);
// Engine-drawn world label: A-Z, 0-9, space and + - . / : only.
RXC_API void rxc_world_text(const float position[3], float size, uint32_t rgba,
                            const char *text);
RXC_API void rxc_debug_line(const float a[3], const float b[3], uint32_t rgba);

// ---------------------------------------------------------------- hud

// Screen pixels, origin top left. Colors are 0xAABBGGRR.
RXC_API void rxc_screen_size(float *width, float *height);
RXC_API void rxc_hud_rect(float x, float y, float w, float h, uint32_t rgba);
RXC_API void rxc_hud_quad(const float xy[8], uint32_t rgba);
RXC_API void rxc_hud_line(float x0, float y0, float x1, float y1,
                          float thickness, uint32_t rgba);
RXC_API void rxc_hud_text(float x, float y, float size, uint32_t rgba,
                          const char *text);
RXC_API float rxc_hud_text_width(float size, const char *text);

// ---------------------------------------------------------------- input

enum {
  RXC_KEY_W,
  RXC_KEY_A,
  RXC_KEY_S,
  RXC_KEY_D,
  RXC_KEY_Q,
  RXC_KEY_E,
  RXC_KEY_F,
  RXC_KEY_T,
  RXC_KEY_C,
  RXC_KEY_R,
  RXC_KEY_G,
  RXC_KEY_X,
  RXC_KEY_Z,
  RXC_KEY_B,
  RXC_KEY_V,
  RXC_KEY_SPACE,
  RXC_KEY_LSHIFT,
  RXC_KEY_LCTRL,
  RXC_KEY_ESCAPE,
  RXC_KEY_F1,
  RXC_KEY_F2,
  RXC_KEY_F3,
  RXC_KEY_F4,
  RXC_KEY_F5,
  RXC_KEY_DELETE,
  RXC_KEY_BACKSPACE,
  RXC_KEY_RETURN,
  RXC_KEY_1,
  RXC_KEY_2,
  RXC_KEY_3,
  RXC_KEY_4,
  RXC_KEY_J,
  RXC_KEY_UP,
  RXC_KEY_DOWN,
  RXC_KEY_LEFT,
  RXC_KEY_RIGHT,
  RXC_KEY_TAB,
  RXC_KEY_M,
  RXC_KEY_5,
  RXC_KEY_6,
  RXC_KEY_L,
  RXC_KEY_COUNT
};

enum { RXC_MOUSE_LEFT, RXC_MOUSE_RIGHT, RXC_MOUSE_MIDDLE };

RXC_API int32_t rxc_key_down(int32_t key);
RXC_API int32_t rxc_key_pressed(int32_t key);
RXC_API int32_t rxc_mouse_down(int32_t button);
RXC_API int32_t rxc_mouse_pressed(int32_t button);
// Relative motion in pixels and wheel steps since the last frame.
RXC_API void rxc_mouse_delta(float *dx, float *dy);
RXC_API float rxc_mouse_wheel(void);
RXC_API void rxc_capture_mouse(int32_t capture);
RXC_API int32_t rxc_window_focused(void);

// ---------------------------------------------------------------- audio

// Interleaved float samples in [-1, 1]. The engine keeps a copy.
RXC_API uint32_t rxc_sound(const float *samples, uint32_t frames,
                           uint32_t channels, uint32_t sample_rate);

typedef struct rxc_play_params {
  float gain;
  int32_t loop;
  int32_t positional;
  float position[3];
  float ref_distance; // metres at full gain
  float max_distance;
} rxc_play_params;

// Returns a voice handle, 0 when audio is off.
RXC_API uint32_t rxc_play(uint32_t sound, const rxc_play_params *params);
RXC_API void rxc_voice_stop(uint32_t voice, float fade_seconds);
RXC_API void rxc_voice_gain(uint32_t voice, float gain);
RXC_API void rxc_voice_position(uint32_t voice, const float position[3]);
RXC_API void rxc_master_volume(float volume);

// ---------------------------------------------------------------- capture

// Writes the next presented frame (hud included) to a png.
RXC_API void rxc_screenshot(const char *path);

#ifdef __cplusplus
}
#endif

#endif // RX_APPS_RXC_RX_C_H_
