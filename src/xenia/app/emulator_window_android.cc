/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Hello
 ******************************************************************************
 */

#include "xenia/app/emulator_window_android.h"

#include "xenia/app/emulator_window.h"
#include "xenia/emulator.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/ui/window.h"

namespace xe {
namespace app {

EmulatorWindow::EmulatorWindow(Emulator* emulator,
                               ui::WindowedAppContext& app_context,
                               uint32_t width, uint32_t height)
    : emulator_(emulator), app_context_(app_context), window_listener_(*this) {}

EmulatorWindow::~EmulatorWindow() = default;

std::unique_ptr<EmulatorWindow> EmulatorWindow::Create(
    Emulator* emulator, ui::WindowedAppContext& app_context, uint32_t width,
    uint32_t height) {
  return std::unique_ptr<EmulatorWindow>(
      new EmulatorWindow(emulator, app_context, width, height));
}

bool EmulatorWindow::Initialize() { return true; }

ui::Presenter* EmulatorWindow::GetGraphicsSystemPresenter() const {
  gpu::GraphicsSystem* graphics_system = emulator_->graphics_system();
  return graphics_system ? graphics_system->presenter() : nullptr;
}

void EmulatorWindow::SetupGraphicsSystemPresenterPainting() {
  ui::Presenter* presenter = GetGraphicsSystemPresenter();
  if (!presenter) {
    return;
  }

  if (window_) {
    window_->SetPresenter(presenter);
  }
  presenter_painting_ = presenter;
}

void EmulatorWindow::ShutdownGraphicsSystemPresenterPainting() {
  if (window_) {
    window_->SetPresenter(nullptr);
  }
  presenter_painting_ = nullptr;
}

void EmulatorWindow::OnEmulatorInitialized() {}
void EmulatorWindow::UpdateTitle() {}

void EmulatorWindow::SetInitializingShaderStorage(bool initializing) {
  initializing_shader_storage_ = initializing;
}

void EmulatorWindow::LaunchTitleInNewProcess(
    const std::filesystem::path& path_to_file) {}

xe::X_STATUS EmulatorWindow::RunTitle(
    const std::filesystem::path& path_to_file) {
  return X_STATUS_UNSUCCESSFUL;
}

void EmulatorWindow::SetFullscreen(bool fullscreen) {}
void EmulatorWindow::ToggleFullscreen() {}
void EmulatorWindow::TakeScreenshot() {}
void EmulatorWindow::ExportScreenshot(const xe::ui::RawImage& image) {}

void EmulatorWindow::SaveImage(const std::filesystem::path& path,
                               const xe::ui::RawImage& image) {}

void EmulatorWindow::ToggleProfilesConfigDialog() {}
void EmulatorWindow::ToggleAudioDialog() {}
void EmulatorWindow::ToggleConfigDialog() {}
void EmulatorWindow::OpenConfigDialog(const std::string& category) {}
void EmulatorWindow::ToggleControllerVibration() {}
void EmulatorWindow::FileOpen() {}
void EmulatorWindow::FileAddGames() {}

void EmulatorWindow::UpdateAntiAliasingCvar(
    gpu::CommandProcessor::SwapPostEffect effect) {}

void EmulatorWindow::UpdateScalingAndSharpeningCvar(
    ui::Presenter::GuestOutputPaintConfig::Effect effect) {}

void EmulatorWindow::UpdateFsrSharpnessCvar(float value) {}
void EmulatorWindow::UpdateFsrMaxUpsamplingPassesCvar(uint32_t value) {}
void EmulatorWindow::UpdateCasSharpnessCvar(float value) {}
void EmulatorWindow::UpdateDitherCvar(bool value) {}

const char* EmulatorWindow::GetCvarValueForSwapPostEffect(
    gpu::CommandProcessor::SwapPostEffect effect) {
  return "";
}

gpu::CommandProcessor::SwapPostEffect
EmulatorWindow::GetSwapPostEffectForCvarValue(const std::string& cvar_value) {
  return gpu::CommandProcessor::SwapPostEffect::kNone;
}

const char* EmulatorWindow::GetCvarValueForGuestOutputPaintEffect(
    ui::Presenter::GuestOutputPaintConfig::Effect effect) {
  return "";
}

ui::Presenter::GuestOutputPaintConfig::Effect
EmulatorWindow::GetGuestOutputPaintEffectForCvarValue(
    const std::string& cvar_value) {
  return ui::Presenter::GuestOutputPaintConfig::Effect::kBilinear;
}

ui::Presenter::GuestOutputPaintConfig
EmulatorWindow::GetGuestOutputPaintConfigForCvars() {
  return ui::Presenter::GuestOutputPaintConfig();
}

void EmulatorWindow::ApplyDisplayConfigForCvars() {}
void EmulatorWindow::ApplyContentVisibility() {}

bool EmulatorWindow::StopTitleAndReturnToList() { return false; }

void EmulatorWindow::InitializeGameLibrary() {}
void EmulatorWindow::AddLaunchedTitleToLibrary(uint32_t title_id,
                                               const std::string& name) {}

void EmulatorWindow::OnKeyDown(ui::KeyEvent& e) {}
void EmulatorWindow::OnMouseDown(const ui::MouseEvent& e) {}
void EmulatorWindow::OnMouseUp(const ui::MouseEvent& e) {}
void EmulatorWindow::OnMouseDoubleClick(const ui::MouseEvent& e) {}
void EmulatorWindow::FileDrop(const std::filesystem::path& filename) {}
void EmulatorWindow::FileClose() {}
void EmulatorWindow::ToggleContextMenu(bool use_cursor_position) {}

EmulatorWindow::ControllerHotKey EmulatorWindow::ProcessControllerHotkey(
    int buttons) {
  return ControllerHotKey();
}

void EmulatorWindow::VibrateController(xe::hid::InputSystem* input_sys,
                                       uint32_t user_index, bool vibrate) {}

void EmulatorWindow::GamepadHotKeys() {}
void EmulatorWindow::ToggleGPUSetting(gpu::GPUSetting setting) {}
void EmulatorWindow::CycleReadbackResolve() {}

std::filesystem::path EmulatorWindow::GetFilePickerInitialDirectory() const {
  return {};
}

void EmulatorWindow::ClearDialogs() {}

void EmulatorWindow::EmulatorWindowListener::OnClosing(ui::UIEvent& e) {}
void EmulatorWindow::EmulatorWindowListener::OnFileDrop(ui::FileDropEvent& e) {}
void EmulatorWindow::EmulatorWindowListener::OnResize(ui::UISetupEvent& e) {}
void EmulatorWindow::EmulatorWindowListener::OnKeyDown(ui::KeyEvent& e) {}
void EmulatorWindow::EmulatorWindowListener::OnMouseDown(ui::MouseEvent& e) {}
void EmulatorWindow::EmulatorWindowListener::OnMouseUp(ui::MouseEvent& e) {}
void EmulatorWindow::EmulatorWindowListener::OnMouseDoubleClick(
    ui::MouseEvent& e) {}
void EmulatorWindow::EmulatorWindowListener::OnUsbDeviceChanged(
    bool is_arrival) {}

}  // namespace app
}  // namespace xe
