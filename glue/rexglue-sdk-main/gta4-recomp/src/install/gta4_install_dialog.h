#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/ui/imgui_dialog.h>

#include "gta4_installer.h"

namespace gta4::install {

class InstallDialog final : public rex::ui::ImGuiDialog {
 public:
  using CompleteCallback = std::function<void()>;
  using CancelCallback = std::function<void()>;

  InstallDialog(rex::ui::ImGuiDrawer* drawer, std::filesystem::path install_root, bool dlc_only,
                CompleteCallback complete, CancelCallback cancel);

  // Fills the base-game and title-update rows from host paths, for hosts
  // without a usable file picker. The installation starts on its own once the
  // base game has passed inspection.
  void Preselect(std::filesystem::path game, std::filesystem::path update);

 protected:
  void OnClose() override;
  void OnDraw(ImGuiIO& io) override;

 private:
  enum class State {
    kSelecting,
    kInstalling,
    kInstalled,
    kFailed,
  };

  enum class PickerTarget {
    kGame,
    kUpdate,
    kTlad,
    kTbogt,
  };

  struct PickerState {
    std::mutex mutex;
    std::filesystem::path game;
    std::filesystem::path update;
    std::filesystem::path tlad;
    std::filesystem::path tbogt;
    std::string error;
    GameSourceInspectionWorker inspection_worker;
  };

  struct BrowserEntry {
    std::filesystem::path path;
    bool directory = false;
  };

  void ShowFilePicker(PickerTarget target);
  void ShowFolderPicker(PickerTarget target);
  void OpenNativePicker(PickerTarget target, bool folder);
  void RefreshNativePickerEntries();
  void DrawNativePicker();
  void ChooseNativePath(const std::filesystem::path& path);
  void StartInstall();
  void FinishInstallIfNeeded();
  void DrawBaseInspection();
  void DrawSourceRow(const char* label, PickerTarget target, const std::filesystem::path& value,
                     bool required);
  void AssignPickedPath(PickerTarget target, std::filesystem::path path);
  std::filesystem::path PathFor(PickerTarget target) const;

  std::filesystem::path install_root_;
  bool dlc_only_ = false;
  CompleteCallback complete_;
  CancelCallback cancel_;
  std::shared_ptr<PickerState> picker_state_;
  Progress progress_;
  Result result_;
  std::thread install_thread_;
  std::atomic<bool> install_done_{false};
  State state_ = State::kSelecting;
  bool auto_start_ = false;
  int completion_frames_ = -1;
  bool native_picker_open_ = false;
  bool native_picker_folder_ = false;
  PickerTarget native_picker_target_ = PickerTarget::kGame;
  std::filesystem::path native_picker_directory_;
  std::vector<BrowserEntry> native_picker_entries_;
  int native_picker_selection_ = -1;
  std::string native_picker_error_;
};

}  // namespace gta4::install
