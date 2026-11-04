#include "gta4_install_dialog.h"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>
#include <vector>

#include <SDL3/SDL_dialog.h>
#include <imgui.h>

#include <rex/filesystem.h>

namespace gta4::install {
namespace {

struct PickerRequest {
  std::function<void(std::filesystem::path, std::string)> complete;
};

void SDLCALL OnPathPicked(void* userdata, const char* const* file_list, int) {
  std::unique_ptr<PickerRequest> request(static_cast<PickerRequest*>(userdata));
  if (!request || !request->complete) {
    return;
  }
  if (!file_list) {
    request->complete({}, SDL_GetError());
    return;
  }
  if (!file_list[0]) {
    request->complete({}, {});
    return;
  }
  request->complete(rex::to_path(file_list[0]), {});
}

constexpr SDL_DialogFileFilter kGameFilters[] = {
    {"Xbox 360 disc image", "iso"},
    {"All supported files", "*"},
};

constexpr SDL_DialogFileFilter kUpdateFilters[] = {
    {"Xbox title update", "xexp"},
    {"Xbox content package", "*"},
};

constexpr SDL_DialogFileFilter kDlcFilters[] = {
    {"Xbox content package", "*"},
};

}  // namespace

InstallDialog::InstallDialog(rex::ui::ImGuiDrawer* drawer, std::filesystem::path install_root,
                             bool dlc_only, CompleteCallback complete, CancelCallback cancel)
    : ImGuiDialog(drawer),
      install_root_(std::move(install_root)),
      dlc_only_(dlc_only),
      complete_(std::move(complete)),
      cancel_(std::move(cancel)),
      picker_state_(std::make_shared<PickerState>()) {}

void InstallDialog::Preselect(std::filesystem::path game, std::filesystem::path update) {
  if (!dlc_only_ && !game.empty()) {
    AssignPickedPath(PickerTarget::kGame, std::move(game));
  }
  if (!dlc_only_ && !update.empty()) {
    AssignPickedPath(PickerTarget::kUpdate, std::move(update));
  }
  auto_start_ = !dlc_only_ && !PathFor(PickerTarget::kGame).empty() &&
                !PathFor(PickerTarget::kUpdate).empty();
}

void InstallDialog::OnClose() {
  native_picker_open_ = false;
  picker_state_->inspection_worker.Stop();
  progress_.cancel_requested = true;
  if (install_thread_.joinable()) {
    install_thread_.join();
  }
}

void InstallDialog::AssignPickedPath(PickerTarget target, std::filesystem::path path) {
  std::lock_guard lock(picker_state_->mutex);
  switch (target) {
    case PickerTarget::kGame:
      picker_state_->game = std::move(path);
      if (picker_state_->game.empty()) {
        picker_state_->inspection_worker.Clear();
      } else {
        picker_state_->inspection_worker.Request(picker_state_->game);
      }
      break;
    case PickerTarget::kUpdate:
      picker_state_->update = std::move(path);
      break;
    case PickerTarget::kTlad:
      picker_state_->tlad = std::move(path);
      break;
    case PickerTarget::kTbogt:
      picker_state_->tbogt = std::move(path);
      break;
  }
}

std::filesystem::path InstallDialog::PathFor(PickerTarget target) const {
  std::lock_guard lock(picker_state_->mutex);
  switch (target) {
    case PickerTarget::kGame:
      return picker_state_->game;
    case PickerTarget::kUpdate:
      return picker_state_->update;
    case PickerTarget::kTlad:
      return picker_state_->tlad;
    case PickerTarget::kTbogt:
      return picker_state_->tbogt;
  }
  return {};
}

void InstallDialog::ShowFilePicker(PickerTarget target) {
#if REX_PLATFORM_PS4
  OpenNativePicker(target, false);
  return;
#endif
  auto weak_state = std::weak_ptr<PickerState>(picker_state_);
  auto* request =
      new PickerRequest{[weak_state, target](std::filesystem::path path, std::string error) {
        auto state = weak_state.lock();
        if (!state) {
          return;
        }
        std::lock_guard lock(state->mutex);
        if (!error.empty()) {
          state->error = std::move(error);
          return;
        }
        if (path.empty()) {
          return;
        }
        state->error.clear();
        switch (target) {
          case PickerTarget::kGame:
            state->game = std::move(path);
            state->inspection_worker.Request(state->game);
            break;
          case PickerTarget::kUpdate:
            state->update = std::move(path);
            break;
          case PickerTarget::kTlad:
            state->tlad = std::move(path);
            break;
          case PickerTarget::kTbogt:
            state->tbogt = std::move(path);
            break;
        }
      }};

  const SDL_DialogFileFilter* filters = kDlcFilters;
  int filter_count = static_cast<int>(std::size(kDlcFilters));
  if (target == PickerTarget::kGame) {
    filters = kGameFilters;
    filter_count = static_cast<int>(std::size(kGameFilters));
  } else if (target == PickerTarget::kUpdate) {
    filters = kUpdateFilters;
    filter_count = static_cast<int>(std::size(kUpdateFilters));
  }
  SDL_ShowOpenFileDialog(OnPathPicked, request, nullptr, filters, filter_count, nullptr, false);
}

void InstallDialog::ShowFolderPicker(PickerTarget target) {
#if REX_PLATFORM_PS4
  OpenNativePicker(target, true);
  return;
#endif
  auto weak_state = std::weak_ptr<PickerState>(picker_state_);
  auto* request =
      new PickerRequest{[weak_state, target](std::filesystem::path path, std::string error) {
        auto state = weak_state.lock();
        if (!state) {
          return;
        }
        std::lock_guard lock(state->mutex);
        if (!error.empty()) {
          state->error = std::move(error);
          return;
        }
        if (path.empty()) {
          return;
        }
        state->error.clear();
        switch (target) {
          case PickerTarget::kGame:
            state->game = std::move(path);
            state->inspection_worker.Request(state->game);
            break;
          case PickerTarget::kUpdate:
            state->update = std::move(path);
            break;
          case PickerTarget::kTlad:
            state->tlad = std::move(path);
            break;
          case PickerTarget::kTbogt:
            state->tbogt = std::move(path);
            break;
        }
      }};
  SDL_ShowOpenFolderDialog(OnPathPicked, request, nullptr, nullptr, false);
}

void InstallDialog::OpenNativePicker(PickerTarget target, bool folder) {
  native_picker_target_ = target;
  native_picker_folder_ = folder;
  native_picker_selection_ = -1;
  native_picker_error_.clear();
  native_picker_open_ = true;

  // Removable media is the usual source for console installs. Keep these as
  // quick jumps, but allow browsing from / when the mount name differs.
  const std::filesystem::path roots[] = {"/mnt/usb0", "/mnt/usb1", "/data", "/"};
  native_picker_directory_ = "/";
  for (const auto& root : roots) {
    std::error_code error;
    if (std::filesystem::is_directory(root, error)) {
      native_picker_directory_ = root;
      break;
    }
  }
  RefreshNativePickerEntries();
}

void InstallDialog::RefreshNativePickerEntries() {
  native_picker_entries_.clear();
  native_picker_selection_ = -1;
  std::error_code error;
  std::filesystem::directory_iterator iterator(
      native_picker_directory_, std::filesystem::directory_options::skip_permission_denied, error);
  if (error) {
    native_picker_error_ = error.message();
    return;
  }
  for (const auto& entry : iterator) {
    std::error_code entry_error;
    const auto symlink_status = entry.symlink_status(entry_error);
    if (entry_error || std::filesystem::is_symlink(symlink_status)) {
      continue;
    }
    const bool is_directory = std::filesystem::is_directory(symlink_status);
    if (!is_directory && !native_picker_folder_ && native_picker_target_ == PickerTarget::kGame) {
      std::string extension = entry.path().extension().string();
      std::transform(extension.begin(), extension.end(), extension.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (extension != ".iso" && extension != ".xiso") {
        continue;
      }
    }
    native_picker_entries_.push_back({entry.path(), is_directory});
  }
  std::sort(native_picker_entries_.begin(), native_picker_entries_.end(),
            [](const BrowserEntry& a, const BrowserEntry& b) {
              if (a.directory != b.directory) return a.directory;
              return a.path.filename().string() < b.path.filename().string();
            });
  native_picker_error_.clear();
}

void InstallDialog::ChooseNativePath(const std::filesystem::path& path) {
  native_picker_open_ = false;
  {
    std::lock_guard lock(picker_state_->mutex);
    picker_state_->error.clear();
  }
  AssignPickedPath(native_picker_target_, path);
}

void InstallDialog::DrawNativePicker() {
  if (!native_picker_open_) {
    return;
  }

  ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x * 0.72f,
                                  ImGui::GetIO().DisplaySize.y * 0.76f),
                           ImGuiCond_Always);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
  if (!ImGui::Begin(native_picker_folder_ ? "Select Folder##native-picker"
                                         : "Select File##native-picker",
                    nullptr, flags)) {
    ImGui::End();
    return;
  }

  ImGui::TextWrapped("%s", native_picker_directory_.string().c_str());
  if (ImGui::Button("Up") && native_picker_directory_ != native_picker_directory_.root_path()) {
    native_picker_directory_ = native_picker_directory_.parent_path();
    RefreshNativePickerEntries();
  }
  ImGui::SameLine();
  const std::filesystem::path quick_roots[] = {"/mnt/usb0", "/mnt/usb1", "/data", "/"};
  for (size_t i = 0; i < std::size(quick_roots); ++i) {
    std::error_code error;
    if (!std::filesystem::is_directory(quick_roots[i], error)) continue;
    ImGui::SameLine();
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::SmallButton(quick_roots[i] == "/" ? "/" : quick_roots[i].filename().string().c_str())) {
      native_picker_directory_ = quick_roots[i];
      RefreshNativePickerEntries();
    }
    ImGui::PopID();
  }

  ImGui::Separator();
  if (ImGui::BeginChild("##native-picker-entries", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() * 2.2f),
                        ImGuiChildFlags_Borders)) {
    for (size_t i = 0; i < native_picker_entries_.size(); ++i) {
      const auto& entry = native_picker_entries_[i];
      const std::string name = (entry.directory ? "[DIR] " : "      ") +
                               entry.path.filename().string();
      if (ImGui::Selectable(name.c_str(), native_picker_selection_ == static_cast<int>(i))) {
        native_picker_selection_ = static_cast<int>(i);
      }
    }
  }
  ImGui::EndChild();

  if (!native_picker_error_.empty()) {
    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", native_picker_error_.c_str());
  }
  bool can_open = native_picker_selection_ >= 0 &&
                  native_picker_entries_[native_picker_selection_].directory;
  bool can_choose_file = !native_picker_folder_ && native_picker_selection_ >= 0 &&
                         !native_picker_entries_[native_picker_selection_].directory;
  if (!can_open && !can_choose_file) ImGui::BeginDisabled();
  if (ImGui::Button(native_picker_folder_ ? "Open Folder" : "Open Folder") && can_open) {
    native_picker_directory_ = native_picker_entries_[native_picker_selection_].path;
    RefreshNativePickerEntries();
  }
  if (!can_open && !can_choose_file) ImGui::EndDisabled();
  if (can_choose_file) {
    ImGui::SameLine();
    if (ImGui::Button("Choose File")) {
      ChooseNativePath(native_picker_entries_[native_picker_selection_].path);
    }
  }
  if (native_picker_folder_) {
    ImGui::SameLine();
    if (ImGui::Button("Choose This Folder")) {
      ChooseNativePath(native_picker_directory_);
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) {
    native_picker_open_ = false;
  }
  ImGui::End();
}

void InstallDialog::DrawSourceRow(const char* label, PickerTarget target,
                                  const std::filesystem::path& value, bool required) {
  ImGui::PushID(label);
  ImGui::Text("%s%s", label, required ? " *" : "");
  ImGui::SameLine();
  ImGui::TextDisabled("%s", value.empty() ? "Not selected" : value.string().c_str());
  if (ImGui::Button("Select File")) {
    ShowFilePicker(target);
  }
  ImGui::SameLine();
  if (ImGui::Button("Select Folder")) {
    ShowFolderPicker(target);
  }
  if (!value.empty()) {
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
      AssignPickedPath(target, {});
    }
  }
  ImGui::PopID();
}

void InstallDialog::StartInstall() {
  if (install_thread_.joinable()) {
    install_thread_.join();
  }

  Selection selection;
  {
    std::lock_guard lock(picker_state_->mutex);
    if (!dlc_only_) {
      const GameSourceInspectionSnapshot inspection = picker_state_->inspection_worker.Snapshot();
      if (!inspection.result || !inspection.result->supported() || picker_state_->update.empty()) {
        return;
      }
      selection.game_source = picker_state_->game;
      selection.update_source = picker_state_->update;
    }
    if (!picker_state_->tlad.empty()) {
      selection.dlc_sources.push_back({Episode::kTlad, picker_state_->tlad});
    }
    if (!picker_state_->tbogt.empty()) {
      selection.dlc_sources.push_back({Episode::kTbogt, picker_state_->tbogt});
    }
    if (dlc_only_ && selection.dlc_sources.empty()) {
      return;
    }
  }

  progress_.copied_bytes = 0;
  progress_.total_bytes = 0;
  progress_.cancel_requested = false;
  result_ = {};
  install_done_ = false;
  state_ = State::kInstalling;
  install_thread_ = std::thread([this, selection = std::move(selection)]() {
    result_ = Install(selection, install_root_, progress_);
    install_done_.store(true, std::memory_order_release);
  });
}

void InstallDialog::FinishInstallIfNeeded() {
  if (state_ != State::kInstalling || !install_done_.load(std::memory_order_acquire)) {
    return;
  }
  if (install_thread_.joinable()) {
    install_thread_.join();
  }
  state_ = result_.success ? State::kInstalled : State::kFailed;
}

void InstallDialog::DrawBaseInspection() {
  const GameSourceInspectionSnapshot snapshot = picker_state_->inspection_worker.Snapshot();
  if (snapshot.checking) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.25f, 1.0f));
    ImGui::TextUnformatted("Checking…");
    ImGui::PopStyleColor();
    return;
  }
  if (!snapshot.result) {
    return;
  }

  const ImVec4 color = snapshot.result->supported() ? ImVec4(0.35f, 0.90f, 0.45f, 1.0f)
                                                    : ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
  const std::string summary = FormatGameSourceInspection(*snapshot.result);
  ImGui::PushStyleColor(ImGuiCol_Text, color);
  ImGui::TextWrapped("%s", summary.c_str());
  ImGui::PopStyleColor();

  const std::string diagnostics = FormatGameSourceDiagnostics(*snapshot.result);
  if (!diagnostics.empty()) {
    ImGui::TextDisabled("%s", diagnostics.c_str());
  }
}

void InstallDialog::OnDraw(ImGuiIO& io) {
  FinishInstallIfNeeded();

  if (completion_frames_ >= 0) {
    if (completion_frames_ == 0) {
      completion_frames_ = -1;
      auto complete = std::move(complete_);
      Close();
      if (complete) {
        complete();
      }
      return;
    }
    --completion_frames_;
  }

  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.10f, io.DisplaySize.y * 0.08f),
                          ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.80f, io.DisplaySize.y * 0.84f),
                           ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.98f);
  constexpr ImGuiWindowFlags kWindowFlags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
  if (!ImGui::Begin("Liberty Recompiled Setup##installer", nullptr, kWindowFlags)) {
    ImGui::End();
    return;
  }

  ImGui::SetWindowFontScale(1.45f);
  ImGui::TextUnformatted(dlc_only_ ? "INSTALL EPISODES" : "INSTALL LIBERTY RECOMPILED");
  ImGui::SetWindowFontScale(1.0f);
  ImGui::Separator();
  ImGui::Spacing();

  if (state_ == State::kSelecting || state_ == State::kFailed) {
    if (!dlc_only_) {
      ImGui::TextWrapped(
          "Select your legally obtained Xbox 360 GTA IV source and the v8 (0.0.8.5) "
          "title update. The update may be an STFS package or raw default.xexp.");
      ImGui::Spacing();
      DrawSourceRow("Base game", PickerTarget::kGame, PathFor(PickerTarget::kGame), true);
      DrawBaseInspection();
      ImGui::Spacing();
      DrawSourceRow("Title update v8", PickerTarget::kUpdate, PathFor(PickerTarget::kUpdate), true);
      ImGui::Spacing();
      ImGui::Separator();
    } else {
      ImGui::TextWrapped(
          "Add either or both installed episodes to the existing GTA IV installation.");
    }

    ImGui::Spacing();
    DrawSourceRow("The Lost and Damned", PickerTarget::kTlad, PathFor(PickerTarget::kTlad), false);
    ImGui::Spacing();
    DrawSourceRow("The Ballad of Gay Tony", PickerTarget::kTbogt, PathFor(PickerTarget::kTbogt),
                  false);
    ImGui::Spacing();
    ImGui::TextDisabled("Install directory: %s", install_root_.string().c_str());

    std::string picker_error;
    {
      std::lock_guard lock(picker_state_->mutex);
      picker_error = picker_state_->error;
    }
    if (!picker_error.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
      ImGui::TextWrapped("File picker error: %s", picker_error.c_str());
      ImGui::PopStyleColor();
    }
    if (state_ == State::kFailed && !result_.error.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
      ImGui::TextWrapped("Installation failed: %s", result_.error.c_str());
      ImGui::PopStyleColor();
    }

    bool has_supported_game = dlc_only_;
    bool has_update = dlc_only_;
    bool has_dlc = false;
    {
      std::lock_guard lock(picker_state_->mutex);
      const GameSourceInspectionSnapshot inspection = picker_state_->inspection_worker.Snapshot();
      has_supported_game = dlc_only_ || (inspection.result && inspection.result->supported());
      has_update = dlc_only_ || !picker_state_->update.empty();
      has_dlc = !picker_state_->tlad.empty() || !picker_state_->tbogt.empty();
    }
    const bool may_install = has_supported_game && has_update && (!dlc_only_ || has_dlc);
    if (auto_start_ && state_ == State::kSelecting && may_install) {
      auto_start_ = false;
      StartInstall();
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(!may_install);
    if (ImGui::Button(state_ == State::kFailed ? "Retry Installation" : "Install",
                      ImVec2(190, 0))) {
      StartInstall();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
      auto cancel = std::move(cancel_);
      Close();
      if (cancel) {
        cancel();
      }
    }
  } else if (state_ == State::kInstalling) {
    ImGui::TextWrapped("Validating, extracting, and publishing the installation...");
    const uint64_t copied = progress_.copied_bytes.load(std::memory_order_relaxed);
    const uint64_t total = progress_.total_bytes.load(std::memory_order_relaxed);
    const float fraction =
        total == 0 ? 0.0f
                   : std::clamp(static_cast<float>(copied) / static_cast<float>(total), 0.0f, 1.0f);
    ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f));
    ImGui::Text("Copied %llu of %llu bytes", static_cast<unsigned long long>(copied),
                static_cast<unsigned long long>(total));
    ImGui::Spacing();
    if (ImGui::Button("Cancel Installation")) {
      progress_.cancel_requested = true;
    }
  } else {
    ImGui::TextWrapped(
        "Installation and integrity validation completed successfully. The game can now start.");
    ImGui::Spacing();
    if (completion_frames_ < 0 && ImGui::Button("Start Game", ImVec2(190, 0))) {
      completion_frames_ = 1;
    }
  }

  ImGui::End();
  DrawNativePicker();
}

}  // namespace gta4::install
