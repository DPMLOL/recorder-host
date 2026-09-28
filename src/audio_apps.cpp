#include "audio_apps.h"

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <windows.h>
#include <wrl/client.h>

#include <map>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

std::string executable_of(DWORD pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) {
    return {};
  }
  wchar_t path[MAX_PATH];
  DWORD size = MAX_PATH;
  std::string name;
  if (QueryFullProcessImageNameW(process, 0, path, &size)) {
    const wchar_t *base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, base, -1, nullptr, 0, nullptr, nullptr);
    name.resize(bytes > 0 ? bytes - 1 : 0);
    WideCharToMultiByte(CP_UTF8, 0, base, -1, name.data(), bytes, nullptr, nullptr);
  }
  CloseHandle(process);
  return name;
}

}  // namespace

void list_audio_apps(obs_data_array_t *apps) {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  struct App {
    DWORD pid;
    bool active;
  };
  std::map<std::string, App> found;

  ComPtr<IMMDeviceEnumerator> enumerator;
  ComPtr<IMMDeviceCollection> devices;
  UINT device_count = 0;
  if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) &&
      SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) {
    devices->GetCount(&device_count);
  }

  for (UINT d = 0; d < device_count; d++) {
    ComPtr<IMMDevice> device;
    ComPtr<IAudioSessionManager2> manager;
    ComPtr<IAudioSessionEnumerator> sessions;
    int session_count = 0;
    if (FAILED(devices->Item(d, &device)) ||
        FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &manager)) ||
        FAILED(manager->GetSessionEnumerator(&sessions)) || FAILED(sessions->GetCount(&session_count))) {
      continue;
    }
    for (int s = 0; s < session_count; s++) {
      ComPtr<IAudioSessionControl> control;
      ComPtr<IAudioSessionControl2> control2;
      DWORD pid = 0;
      AudioSessionState state = AudioSessionStateInactive;
      if (FAILED(sessions->GetSession(s, &control)) || FAILED(control.As(&control2)) ||
          FAILED(control2->GetProcessId(&pid)) || pid == 0) {
        continue;
      }
      control->GetState(&state);
      const std::string executable = executable_of(pid);
      if (executable.empty()) {
        continue;
      }
      App &app = found.try_emplace(executable, App{pid, false}).first->second;
      app.active = app.active || state == AudioSessionStateActive;
    }
  }

  for (const auto &[executable, app] : found) {
    OBSDataAutoRelease entry = obs_data_create();
    obs_data_set_string(entry, "executable", executable.c_str());
    obs_data_set_int(entry, "pid", app.pid);
    obs_data_set_bool(entry, "active", app.active);
    obs_data_array_push_back(apps, entry);
  }

  if (SUCCEEDED(com)) {
    CoUninitialize();
  }
}
