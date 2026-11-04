/**
 ******************************************************************************
 * ReXGlue : PS4 Audio Backend (sceAudioOut)                                  *
 ******************************************************************************
 */

#include <rex/platform.h>

#if REX_PLATFORM_PS4

#include <rex/audio/orbis/orbis_audio_system.h>
#include <rex/audio/ps5/ps5_audio_driver.h>

#include <rex/audio/flags.h>

namespace rex::audio::orbis {

std::unique_ptr<AudioSystem> OrbisAudioSystem::Create(
    runtime::FunctionDispatcher* function_dispatcher) {
  return std::make_unique<OrbisAudioSystem>(function_dispatcher);
}

OrbisAudioSystem::OrbisAudioSystem(runtime::FunctionDispatcher* function_dispatcher)
    : AudioSystem(function_dispatcher) {}

OrbisAudioSystem::~OrbisAudioSystem() {}

void OrbisAudioSystem::Initialize() {
  AudioSystem::Initialize();
}

X_STATUS OrbisAudioSystem::CreateDriver([[maybe_unused]] size_t index,
                                        rex::thread::Semaphore* semaphore,
                                        AudioDriver** out_driver) {
  assert_not_null(out_driver);
  // The PS5 port's driver: the game queues frames and a thread of the driver's
  // own feeds sceAudioOut. OrbisAudioDriver output each frame on the game's
  // audio thread and blocked there, so nothing was buffered: any hold-up of
  // that thread over ~5 ms ran the speakers dry (slow, robotic sound).
  auto driver = new ps5::Ps5AudioDriver(memory_, semaphore);
  if (!driver->Initialize()) {
    driver->Shutdown();
    delete driver;
    return X_STATUS_UNSUCCESSFUL;
  }

  *out_driver = driver;
  return X_STATUS_SUCCESS;
}

void OrbisAudioSystem::DestroyDriver(AudioDriver* driver) {
  assert_not_null(driver);
  auto ps5_driver = dynamic_cast<ps5::Ps5AudioDriver*>(driver);
  assert_not_null(ps5_driver);
  ps5_driver->Shutdown();
  delete ps5_driver;
}

}  // namespace rex::audio::orbis

#endif  // REX_PLATFORM_PS4
