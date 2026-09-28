#include "protocol.h"
#include "recorder.h"

#include <util/base.h>
#include <windows.h>

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fcntl.h>
#include <io.h>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

// Hybrid laptops: run on the discrete GPU, where League renders, so game_capture's shared texture opens.
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

namespace {

std::mutex queue_mutex;
std::condition_variable queue_cv;
std::deque<std::string> queue;
bool input_closed = false;

// stdout is the protocol channel, so every libobs log goes to stderr.
bool debug_logs = false;

void log_to_stderr(int level, const char *format, va_list args, void *) {
  if (level > LOG_INFO && !debug_logs) {
    return;
  }
  const char *prefix = level <= LOG_ERROR ? "error" : level <= LOG_WARNING ? "warning" : level <= LOG_INFO ? "info" : "debug";
  fprintf(stderr, "[%s] ", prefix);
  vfprintf(stderr, format, args);
  fputc('\n', stderr);
  fflush(stderr);
}

void read_stdin() {
  std::string line;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    std::lock_guard lock(queue_mutex);
    queue.push_back(std::move(line));
    queue_cv.notify_one();
  }
  // EOF: the parent app is gone, so is the reason to keep recording.
  std::lock_guard lock(queue_mutex);
  input_closed = true;
  queue_cv.notify_one();
}

// libobs resolves plugins and data relative to the working directory (bin/64bit).
void chdir_to_exe() {
  wchar_t path[MAX_PATH];
  const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (len == 0 || len == MAX_PATH) {
    return;
  }
  if (wchar_t *slash = wcsrchr(path, L'\\')) {
    *slash = L'\0';
    SetCurrentDirectoryW(path);
  }
}

}  // namespace

int main() {
  SetErrorMode(SEM_FAILCRITICALERRORS);
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stdin), _O_BINARY);
  chdir_to_exe();
  debug_logs = getenv("RECORDER_HOST_DEBUG") != nullptr;
  base_set_log_handler(log_to_stderr, nullptr);

  Recorder recorder;
  if (!recorder.init()) {
    protocol::send_error("init", 0, "libobs initialization failed");
    return 1;
  }
  protocol::send_event("ready");

  std::thread reader(read_stdin);
  reader.detach();

  while (true) {
    std::string line;
    {
      std::unique_lock lock(queue_mutex);
      queue_cv.wait(lock, [] { return !queue.empty() || input_closed; });
      if (queue.empty()) {
        break;
      }
      line = std::move(queue.front());
      queue.pop_front();
    }

    OBSDataAutoRelease command = obs_data_create_from_json(line.c_str());
    if (!command) {
      protocol::send_error("", 0, "invalid JSON");
      continue;
    }
    if (strcmp(obs_data_get_string(command, "cmd"), "shutdown") == 0) {
      break;
    }
    recorder.handle(command);
  }

  recorder.shutdown();
  base_set_log_handler(nullptr, nullptr);
  // The detached reader may still be blocked in getline; exit without joining it.
  _exit(0);
}
