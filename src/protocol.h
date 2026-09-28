#pragma once

#include <obs.hpp>

#include <string>

// One JSON object per line: commands on stdin, events on stdout. stdout carries nothing else.
namespace protocol {

void send(obs_data_t *event);
void send_event(const char *name);
void send_error(const char *cmd, long long id, const std::string &message);

}  // namespace protocol
