#include "hook_signature.h"

#include <obs.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <windows.h>
#include <shlobj.h>
#include <softpub.h>
#include <wintrust.h>

#include <cstdio>
#include <filesystem>

namespace {

// Same folder as patches/0001-dpm.patch gives win-capture's hook copy (get_hook_path in game-capture-file-init.c).
constexpr wchar_t kHookCopyFolder[] = L"dpm-recorder-hook";

std::wstring to_wide(const char *utf8) {
  wchar_t *wide = nullptr;
  os_utf8_to_wcs_ptr(utf8, 0, &wide);
  std::wstring result = wide ? wide : L"";
  bfree(wide);
  return result;
}

// A module file's absolute path, empty when win-capture does not have it.
std::wstring module_file(const char *file) {
  obs_module_t *module = obs_get_module("win-capture");
  char *path = module ? obs_find_module_file(module, file) : nullptr;
  if (!path) {
    return L"";
  }
  std::error_code error;
  const std::filesystem::path absolute = std::filesystem::absolute(to_wide(path), error);
  bfree(path);
  return error ? L"" : absolute.wstring();
}

std::wstring hook_copy() {
  wchar_t folder[MAX_PATH];
  if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, SHGFP_TYPE_CURRENT, folder))) {
    return L"";
  }
  const std::filesystem::path path = std::filesystem::path(folder) / kHookCopyFolder / L"graphics-hook64.dll";
  std::error_code error;
  return std::filesystem::is_regular_file(path, error) ? path.wstring() : L"";
}

// Only a valid signature chaining to a trusted root counts: no signature, a bad one, an untrusted or expired
// certificate are all "not trusted". Revocation is not checked and nothing is fetched (cached URLs only), so this
// never waits on the network. Returns why the file is not trusted, empty when it is.
std::string verify(const std::wstring &path, const char *name) {
  if (path.empty()) {
    return std::string(name) + " is missing";
  }
  WINTRUST_FILE_INFO file{};
  file.cbStruct = sizeof(file);
  file.pcwszFilePath = path.c_str();

  WINTRUST_DATA data{};
  data.cbStruct = sizeof(data);
  data.dwUIChoice = WTD_UI_NONE;
  data.fdwRevocationChecks = WTD_REVOKE_NONE;
  data.dwUnionChoice = WTD_CHOICE_FILE;
  data.pFile = &file;
  data.dwStateAction = WTD_STATEACTION_VERIFY;
  data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

  GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  const LONG status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
  data.dwStateAction = WTD_STATEACTION_CLOSE;
  WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);

  if (status == ERROR_SUCCESS) {
    return "";
  }
  switch (status) {
    case TRUST_E_NOSIGNATURE:
      return std::string(name) + " is not signed";
    case TRUST_E_BAD_DIGEST:
      return std::string(name) + " was modified after signing";
    default: {
      char code[16];
      snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(status));
      return std::string(name) + " has an untrusted signature (" + code + ")";
    }
  }
}

}  // namespace

HookSignature check_hook_signature() {
  std::wstring hook = hook_copy();
  if (hook.empty()) {
    hook = module_file("graphics-hook64.dll");
  }
  HookSignature result;
  for (const std::string &reason :
       {verify(module_file("inject-helper64.exe"), "inject-helper64.exe"), verify(hook, "graphics-hook64.dll")}) {
    if (!reason.empty()) {
      result.reason += (result.reason.empty() ? "" : ", ") + reason;
    }
  }
  result.trusted = result.reason.empty();
  blog(result.trusted ? LOG_INFO : LOG_WARNING, "game capture hook: %s",
       result.trusted ? "signed" : result.reason.c_str());
  return result;
}
