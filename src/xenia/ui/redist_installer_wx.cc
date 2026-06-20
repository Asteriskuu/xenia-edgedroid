/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/redist_installer_wx.h"

#include "xenia/base/cvar.h"
#include "xenia/base/platform.h"

#if XE_PLATFORM_WIN32

#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

// wx headers manage their own windows.h inclusion; include them before it.
#include <wx/intl.h>
#include <wx/mstream.h>
#include <wx/string.h>
#include <wx/zipstrm.h>

#include "xenia/base/platform_win.h"

#include <bcrypt.h>
#include <winhttp.h>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/config.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

#endif  // XE_PLATFORM_WIN32

DEFINE_bool(
    d3d12_install_missing_runtime, true,
    "When a required Direct3D 12 runtime DLL (the DXIL shader compiler, the "
    "Agility SDK, or the debug layer) is missing, offer to download it from "
    "Microsoft. Disable to manage the DLLs manually.",
    "D3D12");

namespace xe {
namespace ui {

#if XE_PLATFORM_WIN32

namespace {

// Pinned DirectX Shader Compiler redistributable. Update all of these together
// when bumping: the SHA-256 is the GitHub release asset's "digest" field.
constexpr wchar_t kDxcUrl[] =
    L"https://github.com/microsoft/DirectXShaderCompiler/releases/download/"
    L"v1.9.2602.24/dxc_2026_05_27.zip";
constexpr char kDxcSha256[] =
    "cf658aacf070d3045e31b8f1f8a696c2945f37c1095019481ef7c513368db3b4";

// Pinned DirectX 12 Agility SDK redistributable (a NuGet .nupkg = zip). Version
// must match SetSDKVersion in d3d12_provider (1.619.3 maps to 619).
constexpr wchar_t kAgilityUrl[] =
    L"https://www.nuget.org/api/v2/package/Microsoft.Direct3D.D3D12/1.619.3";
constexpr char kAgilitySha256[] =
    "43a7d5a3973812eb4b42623fae5275c790a005b8e48b8d7f5bb43cef39e073c5";

// Both archives ship per-arch binaries. Pick the one matching the build target.
#if XE_ARCH_ARM64
#define XE_D3D12_REDIST_ARCH "arm64"
#else
#define XE_D3D12_REDIST_ARCH "x64"
#endif

// Entry inside the archive -> destination filename in the D3D12 folder.
struct RedistEntry {
  const char* zip_path;
  const wchar_t* dest_name;
};
constexpr RedistEntry kDxcEntries[] = {
    {"bin/" XE_D3D12_REDIST_ARCH "/dxcompiler.dll", L"dxcompiler.dll"},
    {"bin/" XE_D3D12_REDIST_ARCH "/dxil.dll", L"dxil.dll"},
};
constexpr RedistEntry kAgilityEntries[] = {
    {"build/native/bin/" XE_D3D12_REDIST_ARCH "/D3D12Core.dll",
     L"D3D12Core.dll"},
};
// The debug layer (D3D12SDKLayers.dll) loads only when the Agility runtime is
// active, so it ships alongside D3D12Core.dll.
constexpr RedistEntry kDebugLayerEntries[] = {
    {"build/native/bin/" XE_D3D12_REDIST_ARCH "/D3D12Core.dll",
     L"D3D12Core.dll"},
    {"build/native/bin/" XE_D3D12_REDIST_ARCH "/d3d12SDKLayers.dll",
     L"D3D12SDKLayers.dll"},
};

#undef XE_D3D12_REDIST_ARCH

// True if both dxcompiler.dll and dxil.dll resolve, either from the D3D12
// folder or anywhere on the default DLL search path (system-wide or next to the
// exe).
bool ShaderCompilerPresent(const std::filesystem::path& d3d12_dir) {
  std::error_code ec;
  if (std::filesystem::exists(d3d12_dir / "dxcompiler.dll", ec) &&
      std::filesystem::exists(d3d12_dir / "dxil.dll", ec)) {
    return true;
  }
  HMODULE dxcompiler = LoadLibraryW(L"dxcompiler.dll");
  HMODULE dxil = LoadLibraryW(L"dxil.dll");
  bool present = dxcompiler != nullptr && dxil != nullptr;
  if (dxcompiler) {
    FreeLibrary(dxcompiler);
  }
  if (dxil) {
    FreeLibrary(dxil);
  }
  return present;
}

// Native MessageBox (not wxMessageBox) since the provider may initialize off
// the GUI thread; only the translated string comes from wx.
bool AskYesNo(const wxString& message) {
  return MessageBoxW(nullptr, message.wc_str(), L"Xenia",
                     MB_YESNO | MB_ICONWARNING | MB_TASKMODAL) == IDYES;
}

void ShowError(const wxString& message) {
  MessageBoxW(nullptr, message.wc_str(), L"Xenia",
              MB_OK | MB_ICONERROR | MB_TASKMODAL);
}

// Persists the user's "no" so the prompt doesn't reappear after a restart.
void RememberDecline() {
  OVERRIDE_PERSIST_bool(d3d12_install_missing_runtime, false);
  config::SaveConfig();
}

bool DownloadToMemory(const wchar_t* url, std::vector<uint8_t>& out) {
  out.clear();

  URL_COMPONENTS components = {};
  components.dwStructSize = sizeof(components);
  wchar_t host[256] = {};
  wchar_t path[1024] = {};
  components.lpszHostName = host;
  components.dwHostNameLength = DWORD(xe::countof(host));
  components.lpszUrlPath = path;
  components.dwUrlPathLength = DWORD(xe::countof(path));
  if (!WinHttpCrackUrl(url, 0, 0, &components)) {
    XELOGE("Download: failed to parse URL (error {})", GetLastError());
    return false;
  }

  HINTERNET session =
      WinHttpOpen(L"xenia-edge", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    XELOGE("Download: WinHttpOpen failed (error {})", GetLastError());
    return false;
  }
  WinHttpSetTimeouts(session, 15000, 15000, 30000, 60000);

  bool success = false;
  HINTERNET connect = WinHttpConnect(session, host, components.nPort, 0);
  if (connect) {
    DWORD flags =
        components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request =
        WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (request) {
      if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                             WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
          WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0;
        DWORD status_size = sizeof(status);
        WinHttpQueryHeaders(
            request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
            WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
          success = true;
          for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available)) {
              XELOGE("Download: WinHttpQueryDataAvailable failed (error {})",
                     GetLastError());
              success = false;
              break;
            }
            if (!available) {
              break;
            }
            size_t offset = out.size();
            out.resize(offset + available);
            DWORD read = 0;
            if (!WinHttpReadData(request, out.data() + offset, available,
                                 &read)) {
              XELOGE("Download: WinHttpReadData failed (error {})",
                     GetLastError());
              success = false;
              break;
            }
            out.resize(offset + read);
            if (!read) {
              break;
            }
          }
        } else {
          XELOGE("Download: HTTP status {}", status);
        }
      } else {
        XELOGE("Download: request failed (error {})", GetLastError());
      }
      WinHttpCloseHandle(request);
    }
    WinHttpCloseHandle(connect);
  } else {
    XELOGE("Download: WinHttpConnect failed (error {})", GetLastError());
  }
  WinHttpCloseHandle(session);

  if (!success) {
    out.clear();
  }
  return success;
}

bool VerifySha256(const std::vector<uint8_t>& data, const char* expected_hex) {
  uint8_t digest[32];
  BCRYPT_ALG_HANDLE alg = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) !=
      0) {
    return false;
  }
  bool hashed = false;
  BCRYPT_HASH_HANDLE hash = nullptr;
  if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
    hashed = BCryptHashData(hash, const_cast<PUCHAR>(data.data()),
                            ULONG(data.size()), 0) == 0 &&
             BCryptFinishHash(hash, digest, ULONG(sizeof(digest)), 0) == 0;
    BCryptDestroyHash(hash);
  }
  BCryptCloseAlgorithmProvider(alg, 0);
  if (!hashed) {
    return false;
  }

  // Both this and the pinned constant are lowercase hex, so a plain compare is
  // enough.
  char hex[sizeof(digest) * 2 + 1];
  static const char kHexDigits[] = "0123456789abcdef";
  for (size_t i = 0; i < sizeof(digest); ++i) {
    hex[i * 2] = kHexDigits[digest[i] >> 4];
    hex[i * 2 + 1] = kHexDigits[digest[i] & 0xF];
  }
  hex[sizeof(digest) * 2] = '\0';
  return std::strcmp(hex, expected_hex) == 0;
}

bool WriteFileBytes(const std::filesystem::path& path, const void* data,
                    size_t size) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    return false;
  }
  file.write(static_cast<const char*>(data), std::streamsize(size));
  return bool(file);
}

bool ExtractEntries(const std::vector<uint8_t>& zip_data,
                    const RedistEntry* entries, size_t entry_count,
                    const std::filesystem::path& out_dir) {
  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);

  // Skip entries already on disk. They're immutable, and a present one may be
  // loaded and locked (e.g. D3D12Core.dll once Agility is active).
  std::vector<bool> wanted(entry_count);
  size_t remaining = 0;
  for (size_t i = 0; i < entry_count; ++i) {
    wanted[i] = !std::filesystem::exists(out_dir / entries[i].dest_name, ec);
    remaining += wanted[i] ? 1 : 0;
  }
  if (remaining == 0) {
    return true;
  }

  wxMemoryInputStream mem(zip_data.data(), zip_data.size());
  wxZipInputStream zip(mem);
  if (!zip.IsOk()) {
    XELOGE("DLL install: failed to open the downloaded archive");
    return false;
  }
  for (std::unique_ptr<wxZipEntry> entry(zip.GetNextEntry()); entry;
       entry.reset(zip.GetNextEntry())) {
    std::string name(entry->GetInternalName().utf8_str().data());
    for (size_t i = 0; i < entry_count; ++i) {
      if (!wanted[i] || name != entries[i].zip_path) {
        continue;
      }
      std::vector<char> buffer(static_cast<size_t>(entry->GetSize()));
      zip.Read(buffer.data(), buffer.size());
      auto dest_path = out_dir / entries[i].dest_name;
      if (static_cast<size_t>(zip.LastRead()) != buffer.size() ||
          !WriteFileBytes(dest_path, buffer.data(), buffer.size())) {
        XELOGE("DLL install: failed to extract {}", entries[i].zip_path);
        return false;
      }
      wanted[i] = false;
      if (--remaining == 0) {
        return true;
      }
      break;
    }
  }

  XELOGE("DLL install: archive is missing an expected entry");
  return false;
}

// Launches a fresh copy of this executable with the same command line and
// exits. Returns only if the relaunch couldn't be started.
void RelaunchAndExit() {
  wchar_t exe_path[MAX_PATH];
  DWORD length =
      GetModuleFileNameW(nullptr, exe_path, DWORD(xe::countof(exe_path)));
  if (length == 0 || length >= xe::countof(exe_path)) {
    return;
  }
  std::wstring command_line = GetCommandLineW();
  STARTUPINFOW startup_info = {};
  startup_info.cb = sizeof(startup_info);
  PROCESS_INFORMATION process_info = {};
  if (CreateProcessW(exe_path, command_line.data(), nullptr, nullptr, FALSE, 0,
                     nullptr, nullptr, &startup_info, &process_info)) {
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    ExitProcess(0);
  }
}

// Downloads `url`, verifies it against the expected SHA-256, and extracts the
// given entries into out_dir. Shows an error dialog and returns false on any
// failure.
bool DownloadAndExtract(const wchar_t* url, const char* sha256_hex,
                        const RedistEntry* entries, size_t entry_count,
                        const std::filesystem::path& out_dir) {
  std::vector<uint8_t> package;
  if (!DownloadToMemory(url, package)) {
    ShowError(
        _("Download failed. Check your internet connection and try again."));
    return false;
  }
  if (!VerifySha256(package, sha256_hex)) {
    XELOGE("Downloaded package failed SHA-256 verification, discarding");
    ShowError(
        _("The downloaded file failed integrity verification and was "
          "discarded."));
    return false;
  }
  if (!ExtractEntries(package, entries, entry_count, out_dir)) {
    ShowError(_("Failed to install the downloaded files."));
    return false;
  }
  return true;
}

}  // namespace

bool EnsureShaderCompilerRuntime(const std::filesystem::path& d3d12_dir) {
  if (ShaderCompilerPresent(d3d12_dir)) {
    return true;
  }
  if (!cvars::d3d12_install_missing_runtime) {
    XELOGW(
        "DXIL shader compiler runtime (dxcompiler.dll + dxil.dll) is missing "
        "and auto-install is disabled (d3d12_install_missing_runtime=false)");
    return false;
  }

  // Prompt once per process. Don't re-ask on every provider re-init.
  static bool prompted = false;
  if (prompted) {
    return false;
  }
  prompted = true;

  if (!AskYesNo(
          _("dxcompiler.dll and dxil.dll not found, required for Direct3D 12 "
            "shaders. Download now (~26 MB)?"))) {
    XELOGW("User declined the DXIL shader compiler runtime download");
    RememberDecline();
    return false;
  }

  XELOGI("Downloading the DXIL shader compiler runtime from Microsoft...");
  if (!DownloadAndExtract(kDxcUrl, kDxcSha256, kDxcEntries,
                          xe::countof(kDxcEntries), d3d12_dir)) {
    return false;
  }

  XELOGI("DXIL shader compiler runtime installed to {}",
         xe::path_to_utf8(d3d12_dir));
  return true;
}

bool EnsureAgilityRuntime(const std::filesystem::path& d3d12_dir) {
  std::error_code ec;
  if (std::filesystem::exists(d3d12_dir / "D3D12Core.dll", ec)) {
    // Already downloaded, yet the runtime still didn't report Shader Model 6.6.
    // Don't loop on download + restart.
    XELOGE(
        "D3D12Core.dll is present but the Agility SDK runtime could not be "
        "enabled");
    return false;
  }
  if (!cvars::d3d12_install_missing_runtime) {
    XELOGW(
        "In-box Direct3D 12 runtime lacks Shader Model 6.6 and auto-install is "
        "disabled (d3d12_install_missing_runtime=false)");
    return false;
  }

  static bool prompted = false;
  if (prompted) {
    return false;
  }
  prompted = true;

  if (!AskYesNo(
          _("D3D12Core.dll not found, required for Shader Model 6.6. Download "
            "now (~35 MB) and restart?"))) {
    XELOGW("User declined the DirectX 12 Agility SDK runtime download");
    RememberDecline();
    return false;
  }

  XELOGI("Downloading the DirectX 12 Agility SDK runtime from Microsoft...");
  if (!DownloadAndExtract(kAgilityUrl, kAgilitySha256, kAgilityEntries,
                          xe::countof(kAgilityEntries), d3d12_dir)) {
    return false;
  }

  XELOGI("DirectX 12 Agility SDK runtime installed to {}; restarting",
         xe::path_to_utf8(d3d12_dir));
  RelaunchAndExit();
  // Only reached if the relaunch couldn't be started.
  ShowError(
      _("Installed the DirectX 12 Agility SDK runtime, but couldn't "
        "restart automatically. Please restart Xenia."));
  return false;
}

bool EnsureDebugLayer(const std::filesystem::path& d3d12_dir) {
  std::error_code ec;
  if (std::filesystem::exists(d3d12_dir / "D3D12Core.dll", ec) &&
      std::filesystem::exists(d3d12_dir / "D3D12SDKLayers.dll", ec)) {
    // Both present yet the debug layer still couldn't be enabled. Don't loop.
    XELOGE(
        "The Agility debug layer DLLs are present but the Direct3D 12 debug "
        "layer could not be enabled");
    return false;
  }
  if (!cvars::d3d12_install_missing_runtime) {
    XELOGW(
        "Direct3D 12 debug layer is unavailable and auto-install is disabled "
        "(d3d12_install_missing_runtime=false)");
    return false;
  }

  static bool prompted = false;
  if (prompted) {
    return false;
  }
  prompted = true;

  if (!AskYesNo(
          _("D3D12SDKLayers.dll not found, required for the Direct3D 12 debug "
            "layer. Download now (~35 MB) and restart?"))) {
    XELOGW("User declined the Direct3D 12 debug layer download");
    RememberDecline();
    return false;
  }

  XELOGI("Downloading the DirectX 12 debug layer from Microsoft...");
  if (!DownloadAndExtract(kAgilityUrl, kAgilitySha256, kDebugLayerEntries,
                          xe::countof(kDebugLayerEntries), d3d12_dir)) {
    return false;
  }

  XELOGI("DirectX 12 debug layer installed to {}; restarting",
         xe::path_to_utf8(d3d12_dir));
  RelaunchAndExit();
  // Only reached if the relaunch couldn't be started.
  ShowError(
      _("Installed the Direct3D 12 debug layer, but couldn't restart "
        "automatically. Please restart Xenia."));
  return false;
}

#else

bool EnsureShaderCompilerRuntime(const std::filesystem::path&) { return false; }
bool EnsureAgilityRuntime(const std::filesystem::path&) { return false; }
bool EnsureDebugLayer(const std::filesystem::path&) { return false; }

#endif  // XE_PLATFORM_WIN32

}  // namespace ui
}  // namespace xe
