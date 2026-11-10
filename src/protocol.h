#pragma once

#include <obs.hpp>

#include <initializer_list>
#include <string>

// One JSON object per line: commands on stdin, events on stdout. stdout carries nothing else.
namespace protocol {

void send(obs_data_t *event);
// obs_data arrays only hold objects: a list of plain strings (identifiers, not escaped) is added to the JSON text.
void send(obs_data_t *event, const char *key, std::initializer_list<const char *> strings);
void send_event(const char *name);
void send_error(const char *cmd, long long id, const std::string &message);

}  // namespace protocol
