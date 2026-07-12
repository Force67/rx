#ifndef RX_NET_BUBBLE_DEBUG_H_
#define RX_NET_BUBBLE_DEBUG_H_

// 3D visualizer for the streaming bubbles: one depth-tested translucent wire
// sphere per player bubble, colored by peer (PeerColor), drawn through the
// FrameView scene hooks. Lives in its own target (rx::net_viz) so headless
// servers link rx::net without pulling the renderer in.
//
// Feed it either side's bubbles: the server's live interest map
// (ServerSession::interest().bubbles()) or a client's replicated mirror
// (ClientSession::bubbles()). Gated at runtime by RX_NET_BUBBLES (on by
// default once Emit is called; the option lets a build ship with the wiring
// in place but the overlay off).

#include <volk.h>

#include <base/containers/vector.h>

#include "core/export.h"
#include "net/protocol.h"
#include "render/core/renderer.h"

namespace rx::net {

class RX_NET_VIZ_EXPORT BubbleVisualizer {
 public:
  ~BubbleVisualizer();

  // Binds the renderer. Vulkan backend only; returns false (and stays inert)
  // otherwise. The pipeline itself is built lazily on the first recorded
  // frame, from the actual scene-target formats the hook hands over.
  bool Init(render::Renderer& renderer);
  void Shutdown();

  // Installs the scene hook on this frame's view when there is anything to
  // draw and RX_NET_BUBBLES is not 0. Chains an already-installed
  // scene_transparent hook, so it composes with an app's own passes. The
  // bubble list is copied (the frame records after this call returns).
  void Emit(render::FrameView& view, const base::Vector<BubbleState>& bubbles);

  bool ready() const { return ready_; }

 private:
  void Record(const render::SceneHookContext& ctx);
  bool BuildPipeline(const render::SceneHookContext& ctx);

  render::Renderer* renderer_ = nullptr;
  render::Device* device_ = nullptr;
  VkDevice vk_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  base::Vector<BubbleState> bubbles_;
  bool pipeline_failed_ = false;
  bool ready_ = false;
};

}  // namespace rx::net

#endif  // RX_NET_BUBBLE_DEBUG_H_
