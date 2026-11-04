#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "miot.h"
#include "esphome/components/network/util.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#include "esphome/core/string_ref.h"
#include "esphome/core/util.h"

#ifdef USE_CAPTIVE_PORTAL
#include "esphome/components/captive_portal/captive_portal.h"
#endif

#ifdef USE_TIME
#include "esphome/core/time.h"
#endif

/*
 * TODO
 * add "access" to components? read/write/notify
 * automations?
 * reboot mcu on ota reboot
 */

namespace esphome::miot {

ESPHOME_LOG_TAG(TAG, "miot");

namespace {

/// Longest accumulated property list in a single get_properties command.
constexpr size_t MAX_COMMAND_LENGTH = 60;
/// Drop a partially received message if the MCU stalls for this long.
constexpr uint32_t RECEIVE_TIMEOUT_MS = 300;

const char *const NET_OFFLINE = "offline";
const char *const NET_UNPROV = "unprov";
const char *const NET_UAP = "uap";
const char *const NET_LOCAL = "local";
const char *const NET_CLOUD = "cloud";

}  // namespace

void Miot::setup() {
  // discard initial incoming serial buffer
  uint8_t c;
  while (this->available()) {
    if (this->read_byte(&c) && this->rx_count_ < MAX_LINE_LENGTH) {
      this->rx_message_[this->rx_count_++] = c;
    }
  }

  if (this->rx_count_ > 0) {
    this->mcu_log("Discarding initial MCU data: %s", this->get_printable_rx_message().c_str());
    this->rx_count_ = 0;
  }

  // these two commands have to be the first two in that order to get
  // the deerma.humidifier.jsq5 mcu into its main app loop
  this->queue_net_change_command(true);
  this->queue_command("none");

  this->queue_command("MIIO_mcu_version_req");

  this->set_interval("poll", 60000, [this]() { this->poll_properties_(); });

  if (this->heartbeat_siid_ != 0 && this->heartbeat_piid_ != 0) {
    this->set_interval("heartbeat", 60000,
                       [this]() { this->set_property(this->heartbeat_siid_, this->heartbeat_piid_, MiotValue(60)); });
  }

#ifdef USE_OTA_STATE_LISTENER
  ota::get_global_ota_callback()->add_global_state_listener(this);
#endif
}

#ifdef USE_OTA_STATE_LISTENER
void Miot::on_ota_global_state(ota::OTAState state, float progress, uint8_t error, ota::OTAComponent *comp) {
  switch (state) {
    case ota::OTA_STARTED:
      // directly send this to indicate a firmware update, as loop() won't get called anymore
      this->send_reply_((std::string("down MIIO_net_change ") + this->ota_net_indicator_).c_str());
      break;
    case ota::OTA_ERROR:
      this->queue_net_change_command(true);
      break;
    default:
      break;
  }
}
#endif

void Miot::poll_properties_() {
  std::string cmd;
  cmd.reserve(MAX_COMMAND_LENGTH);

  for (const auto &listener : this->listeners_) {
    if (!listener.poll) {
      continue;
    }

    char part[MAX_COMMAND_LENGTH + 1];
    snprintf(part, sizeof(part), " %" PRIu32 " %" PRIu32, listener.siid, listener.piid);

    if (cmd.size() + std::strlen(part) > MAX_COMMAND_LENGTH) {
      this->queue_command("get_properties" + cmd);
      cmd.clear();
    }

    cmd += part;
  }

  if (!cmd.empty()) {
    this->queue_command("get_properties" + cmd);
  }
}

void Miot::loop() {
  // The cached loop start time is accurate enough here and avoids a hardware read per byte.
  const uint32_t now = App.get_loop_component_start_time();

  if (this->rx_count_ > 0 && now - this->last_rx_char_timestamp_ > RECEIVE_TIMEOUT_MS) {
    ESP_LOGE(TAG, "Timeout while receiving from MCU, dropping message '%s'", this->get_printable_rx_message().c_str());
    this->rx_count_ = 0;
  }

  uint8_t c;
  while (this->available()) {
    if (!this->read_byte(&c)) {
      break;
    }

    if (c == '\r') {
      ESP_LOGV(TAG, "Received MCU message '%s'", this->get_printable_rx_message().c_str());
      this->process_message_();
      this->rx_count_ = 0;
      break;
    }

    if (this->rx_count_ >= MAX_LINE_LENGTH) {
      ESP_LOGE(TAG, "MCU message too long, dropping '%s'", this->get_printable_rx_message().c_str());
      this->rx_count_ = 0;
    }

    this->rx_message_[this->rx_count_++] = c;
    this->last_rx_char_timestamp_ = now;
  }
}

void Miot::dump_config() {
  std::string model = this->get_printable_string(this->model_);
  if (model.empty()) {
    model = "(unknown)";
  }
  std::string mcu_version = this->get_printable_string(this->mcu_version_);
  if (mcu_version.empty()) {
    mcu_version = "(unknown)";
  }

  ESP_LOGCONFIG(TAG,
                "MIoT:\n"
                "  Model: %s\n"
                "  MCU version: %s\n"
                "  Heartbeat: %" PRIu32 ":%" PRIu32 "\n"
                "  OTA net indicator: %s\n"
                "  Time: %s",
                model.c_str(), mcu_version.c_str(), this->heartbeat_siid_, this->heartbeat_piid_,
                this->ota_net_indicator_.c_str(),
#ifdef USE_TIME
                "available"
#else
                "unavailable"
#endif
  );

  if (!this->mcu_log_.empty()) {
    ESP_LOGCONFIG(TAG, "  MCU log:");
    for (const auto &entry : this->mcu_log_) {
      ESP_LOGCONFIG(TAG, "    %s", entry.c_str());
    }
  }
}

std::string Miot::get_printable_string(const uint8_t *data, size_t len) const {
  std::string s;
  // Worst case is four characters per escaped byte; reserving for mostly printable data keeps the
  // common case to a single allocation without oversizing the buffer.
  s.reserve(len + len / 4);

  size_t run_start = 0;
  for (size_t i = 0; i < len; i++) {
    if (std::isprint(data[i])) {
      continue;
    }
    // copy the printable run in front of the escape sequence at once
    s.append(reinterpret_cast<const char *>(data + run_start), i - run_start);
    char escaped[5];
    snprintf(escaped, sizeof(escaped), "\\x%02X", static_cast<unsigned>(data[i]));
    s.append(escaped, 4);
    run_start = i + 1;
  }
  s.append(reinterpret_cast<const char *>(data + run_start), len - run_start);

  return s;
}

std::string Miot::get_printable_string(const std::string &str) const {
  return this->get_printable_string(reinterpret_cast<const uint8_t *>(str.c_str()), str.size());
}

MiotListener *Miot::find_listener_(uint32_t siid, uint32_t piid) {
  for (auto &listener : this->listeners_) {
    if (listener.siid == siid && listener.piid == piid) {
      return &listener;
    }
  }
  return nullptr;
}

#ifdef USE_EVENT
MiotEventListener *Miot::find_event_listener_(uint32_t siid, uint32_t eiid) {
  for (auto &listener : this->event_listeners_) {
    if (listener.siid == siid && listener.eiid == eiid) {
      return &listener;
    }
  }
  return nullptr;
}
#endif

void Miot::register_listener(uint32_t siid, uint32_t piid, bool poll, MiotValueType type,
                             const std::function<void(const MiotValue &value)> &func) {
  if (this->find_listener_(siid, piid) != nullptr) {
    ESP_LOGE(TAG, "Property already has a listener: %" PRIu32 ":%" PRIu32, siid, piid);
    return;
  }
  this->listeners_.push_back(MiotListener{siid, piid, poll, type, func});
}

#ifdef USE_EVENT
void Miot::register_event_listener(uint32_t siid, uint32_t eiid, const std::function<void()> &func) {
  if (this->find_event_listener_(siid, eiid) != nullptr) {
    ESP_LOGE(TAG, "Event already has a listener: %" PRIu32 ":%" PRIu32, siid, eiid);
    return;
  }
  this->event_listeners_.push_back(MiotEventListener{siid, eiid, func});
}
#endif

void Miot::queue_command(const std::string &cmd) {
  if (!this->command_queue_.push(cmd)) {
    ESP_LOGE(TAG, "Command queue full, dropping MCU command '%s'", cmd.c_str());
    return;
  }
  ESP_LOGD(TAG, "Queuing MCU command '%s'", cmd.c_str());
}

void Miot::queue_command(const char *fmt, ...) {
  char buffer[MAX_LINE_LENGTH + 1];

  va_list args;
  va_start(args, fmt);
  const int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  if (len < 0) {
    ESP_LOGE(TAG, "Failed to format MCU command '%s'", fmt);
    return;
  }
  if (static_cast<size_t>(len) > MAX_LINE_LENGTH) {
    ESP_LOGE(TAG, "MCU command too long (%d > %zu), dropping", len, MAX_LINE_LENGTH);
    return;
  }

  this->queue_command(std::string(buffer, static_cast<size_t>(len)));
}

void Miot::queue_net_change_command(bool force) {
  const char *reply = this->get_net_reply_();
  if (!force && reply == this->last_net_reply_) {
    return;
  }
  ESP_LOGI(TAG, "Network status changed to '%s'", reply);
  this->queue_command("MIIO_net_change %s", reply);
  this->last_net_reply_ = reply;
}

void Miot::set_property(uint32_t siid, uint32_t piid, const MiotValue &value) {
  switch (value.type) {
    case mvtInt:
      this->queue_command("set_properties %" PRIu32 " %" PRIu32 " %d", siid, piid, value.as_int);
      break;
    case mvtUInt:
      this->queue_command("set_properties %" PRIu32 " %" PRIu32 " %" PRIu32, siid, piid, value.as_uint);
      break;
    case mvtFloat:
      this->queue_command("set_properties %" PRIu32 " %" PRIu32 " %f", siid, piid, value.as_float);
      break;
    case mvtString:
      this->queue_command("set_properties %" PRIu32 " %" PRIu32 " %s", siid, piid, value.as_string.c_str());
      break;
  }
}

void Miot::execute_action(uint32_t siid, uint32_t aiid, const std::string &args) {
  if (args.empty()) {
    this->queue_command("action %" PRIu32 " %" PRIu32, siid, aiid);
  } else {
    this->queue_command("action %" PRIu32 " %" PRIu32 " %s", siid, aiid, args.c_str());
  }
}

void Miot::send_reply_(const char *reply) {
  ESP_LOGV(TAG, "Sending reply '%s' to MCU", reply);
  this->write_str(reply);
  this->write_byte('\r');
  this->flush();
}

void Miot::update_property(uint32_t siid, uint32_t piid, const char *value) {
  if (this->heartbeat_siid_ != 0 && this->heartbeat_piid_ != 0 && siid == this->heartbeat_siid_ &&
      piid == this->heartbeat_piid_) {
    return;
  }

  MiotListener *listener = this->find_listener_(siid, piid);
  if (listener == nullptr) {
    ESP_LOGW(TAG, "Received property value without component: %" PRIu32 ":%" PRIu32 " %s", siid, piid,
             this->get_printable_string(value).c_str());
    return;
  }

  switch (listener->type) {
    case mvtInt:
      listener->func(MiotValue(parse_number<int>(value).value_or(0)));
      break;
    case mvtUInt:
      listener->func(MiotValue(parse_number<uint32_t>(value).value_or(0u)));
      break;
    case mvtFloat:
      listener->func(MiotValue(parse_number<float>(value).value_or(0.0f)));
      break;
    case mvtString:
      listener->func(MiotValue(value));
      break;
  }
}

void Miot::process_event_(uint32_t siid, uint32_t eiid) {
#ifdef USE_EVENT
  MiotEventListener *listener = this->find_event_listener_(siid, eiid);
  if (listener == nullptr) {
    ESP_LOGW(TAG, "Received event without component: %" PRIu32 ":%" PRIu32, siid, eiid);
    return;
  }

  listener->func();
#else
  ESP_LOGW(TAG, "Received event without component: %" PRIu32 ":%" PRIu32, siid, eiid);
#endif
}

// mrfNotify: properties_changed <siid> <piid> <value> ... <siid> <piid> <value>
// mrfSet: result <siid> <piid> <code> ... <siid> <piid> <code>
// mrfAction: result <siid> <aiid> <code> <piid> <value> ... <piid> <value>
// mrfEvent: event_occurred <siid> <eiid> <piid> <value> ... <piid> <value>
void Miot::update_properties(char **saveptr, MiotResultFormat format) {
  const char *siid = nullptr, *piid = nullptr, *aiid = nullptr, *eiid = nullptr, *code = nullptr, *value = nullptr;
  const char *delim = " ";

  if (format == mrfAction) {
    if (!(siid = strtok_r(nullptr, delim, saveptr))) {
      return;
    }
    if (!(aiid = strtok_r(nullptr, delim, saveptr))) {
      return;
    }
    if (!(code = strtok_r(nullptr, delim, saveptr))) {
      return;
    }
    if (std::strcmp(code, "0")) {
      ESP_LOGE(TAG, "Result error on action %s:%s: %s", siid, aiid, code);
      return;
    }
  }

  if (format == mrfEvent) {
    if (!(siid = strtok_r(nullptr, delim, saveptr))) {
      return;
    }
    if (!(eiid = strtok_r(nullptr, delim, saveptr))) {
      return;
    }
  }

  while (true) {
    delim = " ";

    if (format != mrfAction && format != mrfEvent) {
      if (!(siid = strtok_r(nullptr, delim, saveptr))) {
        break;
      }
    }
    if (!(piid = strtok_r(nullptr, delim, saveptr))) {
      break;
    }
    if (format == mrfSet) {
      if (!(code = strtok_r(nullptr, delim, saveptr))) {
        break;
      }
      if (std::strcmp(code, "0")) {
        ESP_LOGE(TAG, "Result error on property %s:%s: %s", siid, piid, code);
        continue;
      }
    }
    // peek if the next token starts with a " to properly parse string results containing spaces
    if (*saveptr && **saveptr == '"') {
      delim = "\"";
    }
    if (!(value = strtok_r(nullptr, delim, saveptr))) {
      break;
    }

    this->update_property(parse_number<uint32_t>(siid).value_or(0u), parse_number<uint32_t>(piid).value_or(0u), value);
  }

  if (format == mrfEvent) {
    // send event after all properties have been updated
    this->process_event_(parse_number<uint32_t>(siid).value_or(0u), parse_number<uint32_t>(eiid).value_or(0u));
  }
}

const char *Miot::get_net_reply_() {
  if (remote_is_connected()) {
    return NET_CLOUD;
  }
  if (network::is_connected()) {
    return NET_LOCAL;
  }
#ifdef USE_CAPTIVE_PORTAL
  if (captive_portal::global_captive_portal != nullptr && captive_portal::global_captive_portal->is_active()) {
    return NET_UAP;
  }
#endif
  if (network::is_disabled()) {
    return NET_UNPROV;
  }
  return NET_OFFLINE;
}

std::string Miot::get_time_reply_(bool posix) const {
#ifdef USE_TIME
  auto now = ESPTime::from_epoch_local(::time(nullptr));

  if (!now.is_valid()) {
    ESP_LOGW(TAG, "MCU time request: time source not ready yet");
    return "0";
  }

  if (posix) {
    std::array<char, 21> buffer;  // int64_t in decimal
    snprintf(buffer.data(), buffer.size(), "%" PRId64, static_cast<int64_t>(now.timestamp));
    return std::string(buffer.data());
  }

  std::array<char, ESPTime::STRFTIME_BUFFER_SIZE> buffer;
  now.strftime_to(buffer, "%Y-%m-%d %H:%M:%S");
  return std::string(buffer.data());
#else
  ESP_LOGW(TAG, "MCU time request: no time source available");
  return "0";
#endif
}

void Miot::mcu_log(const char *fmt, ...) {
  char buffer[MAX_LINE_LENGTH + 1];

  va_list args;
  va_start(args, fmt);
  const int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  if (len < 0) {
    return;
  }
  const size_t size = std::min(static_cast<size_t>(len), MAX_LINE_LENGTH);

  bool known = false;
  for (const auto &entry : this->mcu_log_) {
    if (entry.size() == size && std::memcmp(entry.data(), buffer, size) == 0) {
      known = true;
      break;
    }
  }

  if (!known) {
    if (this->mcu_log_.size() >= MAX_MCU_LOG_ENTRIES) {
      this->mcu_log_.erase(this->mcu_log_.begin());
    }
    if (this->mcu_log_.capacity() < MAX_MCU_LOG_ENTRIES) {
      // grow the list once, on the first MCU log, instead of reallocating for every entry
      this->mcu_log_.reserve(MAX_MCU_LOG_ENTRIES);
    }
    this->mcu_log_.emplace_back(buffer, size);
  }

  ESP_LOGW(TAG, "%s", buffer);
}

void Miot::process_message_() {
  if (this->rx_count_ == 0) {
    // a lone CR or UART noise, there is nothing to parse
    return;
  }

  this->rx_message_[this->rx_count_] = 0;
  char *saveptr = nullptr;
  const StringRef cmd =
      StringRef::from_maybe_nullptr(strtok_r(reinterpret_cast<char *>(this->rx_message_), " ", &saveptr));

  this->queue_net_change_command(false);

  if (cmd == "get_down") {
    if (this->command_queue_.empty()) {
      this->send_reply_("down none");
    } else {
      const std::string &next_cmd = this->command_queue_.front();
      this->expect_action_result_ = next_cmd.rfind("action ", 0, 7) == 0;
      char reply[MAX_LINE_LENGTH + 6];  // "down " + command
      snprintf(reply, sizeof(reply), "down %s", next_cmd.c_str());
      this->send_reply_(reply);
      this->command_queue_.pop();
    }
  } else if (cmd == "properties_changed") {
    this->update_properties(&saveptr, mrfNotify);
    this->send_reply_("ok");
  } else if (cmd == "event_occured") {
    this->update_properties(&saveptr, mrfEvent);
    this->send_reply_("ok");
  } else if (cmd == "result") {
    this->update_properties(&saveptr, this->expect_action_result_ ? mrfAction : mrfSet);
    this->send_reply_("ok");
  } else if (cmd == "net") {
    this->send_reply_(this->get_net_reply_());
  } else if (cmd == "time") {
    const char *arg = strtok_r(nullptr, " ", &saveptr);
    const bool posix = arg != nullptr && *arg != 0 && std::strcmp(arg, "posix") == 0;
    auto reply = this->get_time_reply_(posix);
    ESP_LOGD(TAG, "MCU time request: %s", reply.c_str());
    this->send_reply_(reply.c_str());
  } else if (cmd == "mac") {
    uint8_t mac[MAC_ADDRESS_SIZE];
    get_mac_address_raw(mac);
    char mac_str[MAC_ADDRESS_BUFFER_SIZE];
    format_mac_addr_lower_no_sep(mac, mac_str);
    this->send_reply_(mac_str);
  } else if (cmd == "model") {
    const char *model = strtok_r(nullptr, " ", &saveptr);
    if (model != nullptr && *model != 0) {
      this->model_ = model;
      ESP_LOGI(TAG, "Model: %s", this->get_printable_string(this->model_).c_str());
    }
    this->send_reply_("ok");
  } else if (cmd == "mcu_version") {
    const char *version = strtok_r(nullptr, " ", &saveptr);
    if (version != nullptr && *version != 0) {
      this->mcu_version_ = version;
      ESP_LOGI(TAG, "MCU version: %s", this->get_printable_string(this->mcu_version_).c_str());
    }
    this->send_reply_("ok");
  } else if (cmd == "error") {
    const char *message = strtok_r(nullptr, "\"", &saveptr);
    const char *code = "";
    if (message != nullptr) {
      if (const char *next = strtok_r(nullptr, " ", &saveptr); next != nullptr) {
        code = next;
      }
    }
    std::string printable;
    if (message != nullptr) {
      printable = this->get_printable_string(message);
    }
    ESP_LOGE(TAG, "MCU command error %s: %s", code, printable.c_str());
    this->send_reply_("ok");
  } else if (cmd == "restore") {
    ESP_LOGI(TAG, "Resetting to factory defaults...");
    this->send_reply_("ok");
    global_preferences->reset();
    App.safe_reboot();
  } else if (cmd.size() < 3 || !std::islower(cmd[0]) || !std::islower(cmd[1]) || !std::islower(cmd[2])) {
    // required for deerma.humidifier.jsq5 and other devices with picky MCUs
    // that enforce MIoT spec compliance, and print e.g. debug info over UART.
    this->mcu_log("Invalid command '%s'", this->get_printable_rx_message().c_str());
    this->send_reply_("error");
  } else {
    this->mcu_log("Unknown command '%s'", this->get_printable_rx_message().c_str());
    this->send_reply_("ok");
  }
}

}  // namespace esphome::miot
