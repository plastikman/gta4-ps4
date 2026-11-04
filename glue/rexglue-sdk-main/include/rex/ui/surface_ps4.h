#pragma once
/**
 * PS4's RADV WSI has no native window system. Its VK_EXT_headless_surface
 * swapchain is the display surface: mesa-ps4's Orbis WSI registers the
 * swapchain images with VideoOut and submits flips on vkQueuePresentKHR.
 */

#include <cstdint>

#include <rex/ui/surface.h>

namespace rex::ui {

class Ps4HeadlessSurface final : public Surface {
 public:
  // The current Orbis WSI reports a 1920x1080 scanout. SDL only supplies events.
  Ps4HeadlessSurface(uint32_t width, uint32_t height) : width_(width), height_(height) {}
  TypeIndex GetType() const override { return kTypeIndex_Ps4Headless; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override {
    width_out = width_;
    height_out = height_;
    return true;
  }

 private:
  uint32_t width_;
  uint32_t height_;
};

}  // namespace rex::ui
