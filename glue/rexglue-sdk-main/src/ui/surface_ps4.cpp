/**
 * PS4's SDL dummy window is only used for events; the Vulkan presenter owns
 * the display through RADV's VK_EXT_headless_surface WSI.
 */
#include <rex/ui/surface_ps4.h>
