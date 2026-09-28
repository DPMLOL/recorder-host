#include "protocol.h"

#include <cstdio>
#include <mutex>

namespace protocol {

namespace {
std::mutex write_mutex;
}

void send(obs_data_t *event) {
  // obs_data_get_json_pretty would break the one-line framing.
  const char *json = obs_data_get_json(event);
  std::lock_guard lock(write_mutex);
  fputs(json, stdout);
  fputc('\n', stdout);
  fflush(stdout);
}

void send_event(const char *name) {
  OBSDataAutoRelease event = obs_data_create();
  obs_data_set_string(event, "event", name);
  send(event);
}

void send_error(const char *cmd, long long id, const std::string &message) {
  OBSDataAutoRelease event = obs_data_create();
  obs_data_set_string(event, "event", "error");
  obs_data_set_string(event, "cmd", cmd);
  obs_data_set_int(event, "id", id);
  obs_data_set_string(event, "message", message.c_str());
  send(event);
}

}  // namespace protocol
