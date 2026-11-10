#include "protocol.h"

#include <cstdio>
#include <mutex>

namespace protocol {

namespace {
std::mutex write_mutex;

void write_line(const char *json) {
  std::lock_guard lock(write_mutex);
  fputs(json, stdout);
  fputc('\n', stdout);
  fflush(stdout);
}
}  // namespace

void send(obs_data_t *event) {
  // obs_data_get_json_pretty would break the one-line framing.
  write_line(obs_data_get_json(event));
}

void send(obs_data_t *event, const char *key, std::initializer_list<const char *> strings) {
  std::string list = std::string("\"") + key + "\":[";
  for (const char *value : strings) {
    if (list.back() != '[') {
      list += ',';
    }
    list += std::string("\"") + value + "\"";
  }
  list += ']';
  // The event is never empty, so its closing brace follows a member.
  std::string json = obs_data_get_json(event);
  json.insert(json.size() - 1, "," + list);
  write_line(json.c_str());
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
