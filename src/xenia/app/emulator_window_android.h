#ifndef XENIA_APP_EMULATOR_WINDOW_H_
#define XENIA_APP_EMULATOR_WINDOW_H_

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "xenia/app/game_library.h"
#include "xenia/emulator.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/ui/presenter.h"
#include "xenia/ui/window.h"
#include "xenia/ui/window_listener.h"
#include "xenia/ui/windowed_app_context.h"
#include "xenia/xbox.h"

namespace xe {
namespace threading {
class Thread;
}
namespace app {

class EmulatorWindow {
 public:
  using steady_clock = std::chrono::steady_clock;

  enum : size_t {
    kZOrderHidInput,
    kZOrderImGui,
    kZOrderProfiler,
    kZOrderEmulatorWindowInput,
  };

  virtual ~EmulatorWindow();

  static std::unique_ptr<EmulatorWindow> Create(
      Emulator* emulator, ui::WindowedAppContext& app_context, uint32_t width,
      uint32_t height);

  std::unique_ptr<xe::threading::Thread> Gamepad_HotKeys_Listener;
  std::atomic<bool> hotkeys_listener_running_ = {false};

  static constexpr int64_t diff_in_ms(
      const steady_clock::time_point t1,
      const steady_clock::time_point t2) noexcept {
    using ms = std::chrono::milliseconds;
    return std::chrono::duration_cast<ms>(t1 - t2).count();
  }

  steady_clock::time_point last_mouse_up = steady_clock::now();
  steady_clock::time_point last_mouse_down = steady_clock::now();

  Emulator* emulator() const { return emulator_; }
  GameLibrary* game_library() const { return game_library_.get(); }
  ui::WindowedAppContext& app_context() const { return app_context_; }
  ui::Window* window() const { return window_.get(); }

  ui::Presenter* GetGraphicsSystemPresenter() const;
  void SetupGraphicsSystemPresenterPainting();
  void ShutdownGraphicsSystemPresenterPainting();

  void OnEmulatorInitialized();

  void LaunchTitleInNewProcess(const std::filesystem::path& path_to_file);
  xe::X_STATUS RunTitle(const std::filesystem::path& path_to_file);
  void UpdateTitle();

  void SetFullscreen(bool fullscreen);
  void ToggleFullscreen();
  void SetInitializingShaderStorage(bool initializing);

  void TakeScreenshot();
  void ExportScreenshot(const xe::ui::RawImage& image);
  void SaveImage(const std::filesystem::path& path,
                 const xe::ui::RawImage& image);

  void ToggleProfilesConfigDialog();
  void ToggleAudioDialog();
  void ToggleConfigDialog();
  void OpenConfigDialog(const std::string& category = "");
  void ToggleControllerVibration();
  void SetHotkeysState(bool enabled) { disable_hotkeys_ = !enabled; }
  void FileOpen();
  void FileAddGames();

  void UpdateAntiAliasingCvar(gpu::CommandProcessor::SwapPostEffect effect);
  void UpdateScalingAndSharpeningCvar(
      ui::Presenter::GuestOutputPaintConfig::Effect effect);
  void UpdateFsrSharpnessCvar(float value);
  void UpdateFsrMaxUpsamplingPassesCvar(uint32_t value);
  void UpdateCasSharpnessCvar(float value);
  void UpdateDitherCvar(bool value);

  enum class ButtonFunctions {
    ToggleFullscreen,
    CpuTimeScalarSetHalf,
    CpuTimeScalarSetDouble,
    CpuTimeScalarReset,
    ClearGPUCache,
    ToggleControllerVibration,
    ClearMemoryPageState,
    ReadbackResolve,
    ToggleLogging,
    Unknown
  };

  class ControllerHotKey {
   public:
    bool title_passthru;
    bool rumble;
    std::string pretty;
    ButtonFunctions function;

    ControllerHotKey(ButtonFunctions fn = ButtonFunctions::Unknown,
                     std::string pretty = "", bool rumble = false,
                     bool active = true) {
      function = fn;
      this->pretty = pretty;
      title_passthru = active;
      this->rumble = rumble;
    }
  };

  static const char* GetCvarValueForSwapPostEffect(
      gpu::CommandProcessor::SwapPostEffect effect);
  static gpu::CommandProcessor::SwapPostEffect GetSwapPostEffectForCvarValue(
      const std::string& cvar_value);
  static const char* GetCvarValueForGuestOutputPaintEffect(
      ui::Presenter::GuestOutputPaintConfig::Effect effect);
  static ui::Presenter::GuestOutputPaintConfig::Effect
  GetGuestOutputPaintEffectForCvarValue(const std::string& cvar_value);
  static ui::Presenter::GuestOutputPaintConfig
  GetGuestOutputPaintConfigForCvars();
  void ApplyDisplayConfigForCvars();

 private:
  class EmulatorWindowListener final : public ui::WindowListener,
                                       public ui::WindowInputListener {
   public:
    explicit EmulatorWindowListener(EmulatorWindow& emulator_window)
        : emulator_window_(emulator_window) {}

    void OnClosing(ui::UIEvent& e) override;
    void OnFileDrop(ui::FileDropEvent& e) override;
    void OnResize(ui::UISetupEvent& e) override;

    void OnKeyDown(ui::KeyEvent& e) override;

    void OnMouseDown(ui::MouseEvent& e) override;
    void OnMouseUp(ui::MouseEvent& e) override;
    void OnMouseDoubleClick(ui::MouseEvent& e) override;
    void OnUsbDeviceChanged(bool is_arrival) override;

   private:
    EmulatorWindow& emulator_window_;
  };

  explicit EmulatorWindow(Emulator* emulator,
                          ui::WindowedAppContext& app_context, uint32_t width,
                          uint32_t height);

  bool Initialize();

  void InitializeGameLibrary();
  void AddLaunchedTitleToLibrary(uint32_t title_id, const std::string& name);

  void OnKeyDown(ui::KeyEvent& e);
  void OnMouseDown(const ui::MouseEvent& e);
  void OnMouseDoubleClick(const ui::MouseEvent& e);
  void FileDrop(const std::filesystem::path& filename);
  void OnMouseUp(const ui::MouseEvent& e);
  void FileClose();

  void ToggleContextMenu(bool use_cursor_position = true);
  EmulatorWindow::ControllerHotKey ProcessControllerHotkey(int buttons);
  void VibrateController(xe::hid::InputSystem* input_sys, uint32_t user_index,
                         bool vibrate = true);
  void GamepadHotKeys();
  void ToggleGPUSetting(gpu::GPUSetting setting);
  void CycleReadbackResolve();

  static std::string CanonicalizeFileExtension(
      const std::filesystem::path& path);

  std::filesystem::path GetFilePickerInitialDirectory() const;
  void ClearDialogs();

  void ApplyContentVisibility();
  bool StopTitleAndReturnToList();

  Emulator* emulator_;
  ui::WindowedAppContext& app_context_;
  EmulatorWindowListener window_listener_;

  std::unique_ptr<ui::Window> window_;
  ui::Presenter* presenter_painting_ = nullptr;

  bool emulator_initialized_ = false;
  std::atomic<bool> disable_hotkeys_ = false;

  std::string base_title_;
  bool initializing_shader_storage_ = false;
  uint8_t swapped_disc_number_ = 0;

  std::unique_ptr<GameLibrary> game_library_;

  bool target_pending_launch_ = false;
  uint32_t default_logical_width_ = 0;
  uint32_t default_logical_height_ = 0;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_EMULATOR_WINDOW_H_
