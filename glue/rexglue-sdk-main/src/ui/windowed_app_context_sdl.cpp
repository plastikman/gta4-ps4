/**
 * @file        ui/windowed_app_context_sdl.cpp
 * @brief       SDL3 implementation of the windowed app UI loop context
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/windowed_app_context_sdl.h>

#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <vector>


#include <SDL3/SDL.h>
#include <rex/platform.h>
#include <rex/diagnostics/gpu_flight_recorder.h>
#if REX_PLATFORM_PS4
#include <orbis/libkernel.h>
extern "C" int32_t sceGnmSubmitDone(void);
extern "C" int32_t sceGnmAreSubmitsAllowed(void);
#endif

#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/window_sdl.h>

namespace rex::ui {

SDLWindowedAppContext::~SDLWindowedAppContext() {
  // Execute leftover pending functions before the loop machinery goes away,
  // mirroring the shutdown contract documented in WindowedAppContext.
  ExecutePendingFunctionsFromUIThread();
  for (auto& [id, gamepad] : ui_gamepads_) {
    (void)id;
    SDL_CloseGamepad(gamepad);
  }
  ui_gamepads_.clear();
  if (gamepad_initialized_) {
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
  }
  if (SDL_WasInit(SDL_INIT_VIDEO)) {
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
  }
}

bool SDLWindowedAppContext::Initialize() {
  // Touch is routed as a distinct multi-pointer stream. Disable both SDL
  // compatibility synthesis directions; WindowSDL also filters their reserved
  // IDs defensively in case a platform backend still produces one.
  if (!SDL_SetHintWithPriority(SDL_HINT_TOUCH_MOUSE_EVENTS, "0", SDL_HINT_OVERRIDE) ||
      !SDL_SetHintWithPriority(SDL_HINT_MOUSE_TOUCH_EVENTS, "0", SDL_HINT_OVERRIDE)) {
    REXLOG_WARN("Failed to disable SDL touch/mouse compatibility synthesis: {}", SDL_GetError());
  }
#if REX_PLATFORM_MAC
  // Games need held keys to repeat. SDL's macOS default enables the system
  // accent/character chooser while text input is active, so override it before
  // initializing video (when SDL registers its Cocoa application defaults).
  if (!SDL_SetHintWithPriority(SDL_HINT_MAC_PRESS_AND_HOLD, "0", SDL_HINT_OVERRIDE)) {
    REXLOG_ERROR("Failed to disable the macOS press-and-hold character chooser: {}",
                 SDL_GetError());
    return false;
  }

  // AppKit's system fullscreen Space intentionally constrains a window to the
  // display safe area on Macs with a camera housing. LibertyRecomp renders its
  // own fullscreen UI and receives SDL's safe-area rectangle separately, so
  // use SDL's supported non-Space fullscreen path. On Cocoa this creates a
  // borderless window from the complete display bounds, allowing the Metal
  // drawable to cover the auxiliary regions beside the camera housing.
  // NSPrefersDisplaySafeAreaCompatibilityMode=false in the app bundle prevents
  // macOS from shrinking those display bounds back to compatibility mode.
  if (!SDL_SetHintWithPriority(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "0", SDL_HINT_OVERRIDE)) {
    REXLOG_ERROR("Failed to select full-display macOS fullscreen: {}", SDL_GetError());
    return false;
  }
#endif
#if REX_PLATFORM_GNU_LINUX
  // The Linux surface type the presenters consume is XcbWindow, so force X11
  // and keep SDL off Wayland where no surface shim exists yet.
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
#endif
  if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
    REXLOG_ERROR("SDL_InitSubSystem(SDL_INIT_VIDEO) failed: {}", SDL_GetError());
    return false;
  }
  // Gamepads drive UI navigation (installer, overlays) even before the input
  // driver starts; SDL reference-counts the subsystem.
  gamepad_initialized_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
  if (!gamepad_initialized_) {
    REXLOG_WARN("SDL_InitSubSystem(SDL_INIT_GAMEPAD) failed: {}", SDL_GetError());
  }
  uint32_t first = SDL_RegisterEvents(2);
  if (first == 0) {
    REXLOG_ERROR("SDL_RegisterEvents failed: {}", SDL_GetError());
    return false;
  }
  wakeup_event_type_ = first;
  paint_event_type_ = first + 1;
  return true;
}

void SDLWindowedAppContext::NotifyUILoopOfPendingFunctions() {
  // SDL_PushEvent is thread-safe by SDL contract.
  SDL_Event event{};
  event.type = wakeup_event_type_;
  SDL_PushEvent(&event);
}

void SDLWindowedAppContext::PlatformQuitFromUIThread() {
  REXLOG_INFO("app-quit-trace: source=quit-from-ui-thread");
  // RunMainMessageLoop re-checks HasQuitFromUIThread after every event; a
  // wakeup guarantees SDL_WaitEvent returns promptly if the queue is empty.
  NotifyUILoopOfPendingFunctions();
}

int SDLWindowedAppContext::RunMainMessageLoop() {
#if REX_PLATFORM_PS4
  // Do not tie the Shell's submit-done heartbeat to SDL's event loop. The SDL
  // wait can stop returning during a guest/render stall, which leaves the
  // suspend path without its required GNM notification. Keep this lightweight
  // caller independent of the UI and guest rendering threads.
  std::atomic<bool> stop_submit_done{false};
  std::thread submit_done_thread([&stop_submit_done] {
    unsigned submit_done_count = 0;
    while (!stop_submit_done.load(std::memory_order_acquire)) {
      const int32_t allowed = sceGnmAreSubmitsAllowed();
      const auto begin = std::chrono::steady_clock::now();
      const int32_t result = sceGnmSubmitDone();
      const int64_t elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - begin).count();
      ++submit_done_count;
      if (submit_done_count == 1 || submit_done_count % 100 == 0 || result != 0 ||
          elapsed > 100000) {
        char line[192];
        const int length = std::snprintf(
            line, sizeof(line),
            "[PS4 GNM heartbeat] count=%u allowed=%d result=%d us=%lld\n",
            submit_done_count, allowed, result, static_cast<long long>(elapsed));
        if (length > 0) sceKernelDebugOutText(0, line);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });
  unsigned sdl_event_count = 0;
  while (!HasQuitFromUIThread()) {
    SDL_Event event;
    if (SDL_WaitEvent(&event)) {
      ++sdl_event_count;
      char event_line[128];
      const int event_length = std::snprintf(
          event_line, sizeof(event_line),
          "[PS4 SDL event enter] count=%u type=%u\n", sdl_event_count,
          static_cast<unsigned>(event.type));
      if (event_length > 0) sceKernelDebugOutText(0, event_line);
      ProcessEvent(event);
    }
  }
  stop_submit_done.store(true, std::memory_order_release);
  submit_done_thread.join();
  const int32_t exit_result = sceGnmSubmitDone();
  char exit_line[96];
  const int exit_length = std::snprintf(
      exit_line, sizeof(exit_line), "[PS4 GNM loop-exit] result=%d\n", exit_result);
  if (exit_length > 0) sceKernelDebugOutText(0, exit_line);
#elif REX_PLATFORM_ANDROID
  // SDL_WaitEvent never sleeps on Android here: its lifecycle wait is woken
  // again on every pump, so it re-polls joysticks back to back and burns a
  // whole core (and the event-queue lock the render threads push into).
  // Drain the queue, then sleep briefly; at most ~1 ms of added latency.
  constexpr Uint64 kIdleSleepNs = 1000000;
  while (!HasQuitFromUIThread()) {
    SDL_Event event;
    bool handled = false;
    while (!HasQuitFromUIThread() && SDL_PollEvent(&event)) {
      ProcessEvent(event);
      handled = true;
    }
    if (!handled) {
      SDL_DelayNS(kIdleSleepNs);
    }
  }
#else
  while (!HasQuitFromUIThread()) {
    SDL_Event event;
    if (!SDL_WaitEvent(&event)) {
      REXLOG_ERROR("SDL_WaitEvent failed: {}", SDL_GetError());
      return EXIT_FAILURE;
    }
    ProcessEvent(event);
  }
#endif
  return EXIT_SUCCESS;
}

void SDLWindowedAppContext::ProcessEvent(SDL_Event& event) {
  if (event.type == wakeup_event_type_) {
    ExecutePendingFunctionsFromUIThread();
    return;
  }
  if (event.type == paint_event_type_) {
    if (WindowSDL* window = GetWindow(event.user.windowID)) {
      window->HandlePaintEvent(uint32_t(event.user.code));
    }
    return;
  }
  if (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST) {
    if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
      REXLOG_INFO("app-quit-trace: source=sdl-window-close-requested window={}",
                  event.window.windowID);
    }
    if (WindowSDL* window = GetWindow(event.window.windowID)) {
      window->HandleWindowEvent(event);
    }
    return;
  }
  switch (event.type) {
    case SDL_EVENT_QUIT: {
      REXLOG_INFO("app-quit-trace: source=sdl-quit windows={}", windows_.size());
      if (windows_.empty()) {
        QuitFromUIThread();
        break;
      }
      std::vector<WindowSDL*> windows_to_close;
      windows_to_close.reserve(windows_.size());
      for (const auto& [window_id, window] : windows_) {
        (void)window_id;
        windows_to_close.push_back(window);
      }
      for (WindowSDL* window : windows_to_close) {
        if (window && window->phase() == Window::Phase::kOpen) {
          window->RequestClose();
        }
        if (HasQuitFromUIThread()) {
          break;
        }
      }
      break;
    }
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
      if (WindowSDL* window = GetWindow(event.key.windowID)) {
        window->HandleKeyEvent(event);
      }
      break;
    }
    case SDL_EVENT_TEXT_INPUT: {
      if (WindowSDL* window = GetWindow(event.text.windowID)) {
        window->HandleTextInputEvent(event);
      }
      break;
    }
    case SDL_EVENT_MOUSE_MOTION: {
      if (WindowSDL* window = GetWindow(event.motion.windowID)) {
        window->HandleMouseEvent(event);
      }
      break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      if (WindowSDL* window = GetWindow(event.button.windowID)) {
        window->HandleMouseEvent(event);
      }
      break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
      if (WindowSDL* window = GetWindow(event.wheel.windowID)) {
        window->HandleMouseEvent(event);
      }
      break;
    }
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED: {
      if (WindowSDL* window = GetWindow(event.tfinger.windowID)) {
        window->HandleTouchEvent(event);
      } else if (event.type == SDL_EVENT_FINGER_CANCELED) {
        // Some backends cancel a device after its window association has been
        // cleared. Give every live window the terminal edge; only the owner
        // will recognize the device/finger pair.
        for (const auto& [window_id, window] : windows_) {
          (void)window_id;
          if (window) {
            window->HandleTouchEvent(event);
          }
        }
      }
      break;
    }
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_DID_ENTER_BACKGROUND:
    case SDL_EVENT_DID_ENTER_FOREGROUND: {
#if REX_PLATFORM_PS4
      // Keep the recorder live during startup and the stalled display period.
      // Freeze it only as the title is backgrounded, preserving the recent
      // Vulkan submit/fence history for post-run inspection.
      if (event.type == SDL_EVENT_WILL_ENTER_BACKGROUND &&
          rex::diagnostics::gpu_flight::IsEnabled()) {
        sceKernelDebugOutText(0, "[PS4 GNM lifecycle] dumping GPU flight trace before background\n");
        rex::diagnostics::gpu_flight::Dump("app-will-enter-background");
        sceKernelDebugOutText(0, "[PS4 GNM lifecycle] GPU flight trace dump complete\n");
      }
#endif
      for (const auto& [window_id, window] : windows_) {
        (void)window_id;
        if (window) {
          window->HandleAppLifecycleEvent(event);
        }
      }
      break;
    }
    case SDL_EVENT_GAMEPAD_ADDED: {
      if (!ui_gamepads_.contains(event.gdevice.which)) {
        if (SDL_Gamepad* gamepad = SDL_OpenGamepad(event.gdevice.which)) {
          ui_gamepads_.emplace(event.gdevice.which, gamepad);
        }
      }
      break;
    }
    case SDL_EVENT_GAMEPAD_REMOVED: {
      if (auto it = ui_gamepads_.find(event.gdevice.which); it != ui_gamepads_.end()) {
        SDL_CloseGamepad(it->second);
        ui_gamepads_.erase(it);
      }
      break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
      // Gamepad events carry no window.
      for (const auto& [window_id, window] : windows_) {
        (void)window_id;
        if (window) {
          window->HandleGamepadEvent(event);
        }
      }
      break;
    }
    case SDL_EVENT_DROP_FILE: {
      if (WindowSDL* window = GetWindow(event.drop.windowID)) {
        window->HandleDropEvent(event);
      }
      break;
    }
    default:
      break;
  }
}

}  // namespace rex::ui
