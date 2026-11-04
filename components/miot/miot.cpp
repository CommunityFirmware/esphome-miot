#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "miot.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#include "esphome/core/string_ref.h"
#include "esphome/core/util.h"
#include "esphome/core/version.h"

#ifdef USE_NETWORK
#include "esphome/components/network/util.h"
#endif

#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif

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
constexpr size_t MAX_POLL_LENGTH = 60;
/// Drop a partially received message if the MCU stalls for this long.
constexpr uint32_t RECEIVE_TIMEOUT_MS = 300;

const char *const NET_OFFLINE = "offline";
const char *const NET_UNPROV = "unprov";
const char *const NET_UAP = "uap";
const char *const NET_LOCAL = "local";
const char *const NET_CLOUD = "cloud";
const char *const NET_UPDATING = "updating";

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
      this->ota_updating_ = true;
      // directly send this to indicate a firmware update, as loop() won't get called anymore
      this->send_reply_((std::string("down MIIO_net_change ") + this->ota_net_indicator_).c_str());
      // the reply above already reported the new state, so do not queue it again
      this->last_net_reply_ = this->get_net_reply_();
      break;
    case ota::OTA_COMPLETED:
    case ota::OTA_ABORT:
    case ota::OTA_ERROR:
      this->ota_updating_ = false;
      this->queue_net_change_command(true);
      break;
    default:
      break;
  }
}
#endif

void Miot::poll_properties_() {
  std::string cmd;
  cmd.reserve(MAX_POLL_LENGTH);

  for (const auto &listener : this->listeners_) {
    if (!listener.poll) {
      continue;
    }

    char part[MAX_POLL_LENGTH + 1];
    snprintf(part, sizeof(part), " %" PRIu32 " %" PRIu32, listener.siid, listener.piid);

    if (cmd.size() + std::strlen(part) > MAX_POLL_LENGTH) {
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
                "  Strict commands: %s\n"
                "  Require result: %s\n"
                "  Time: %s",
                model.c_str(), mcu_version.c_str(), this->heartbeat_siid_, this->heartbeat_piid_,
                this->ota_net_indicator_.c_str(), YESNO(this->strict_commands_), YESNO(this->require_result_),
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
  if (cmd.size() > MAX_COMMAND_LENGTH) {
    ESP_LOGE(TAG, "MCU command too long (%zu > %zu), dropping '%s'", cmd.size(), MAX_COMMAND_LENGTH, cmd.c_str());
    return;
  }
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
  if (static_cast<size_t>(len) > MAX_COMMAND_LENGTH) {
    ESP_LOGE(TAG, "MCU command too long (%d > %zu), dropping", len, MAX_COMMAND_LENGTH);
    return;
  }

  this->queue_command(std::string(buffer, static_cast<size_t>(len)));
}

bool Miot::is_provisioning_() {
#ifdef USE_CAPTIVE_PORTAL
  if (captive_portal::global_captive_portal == nullptr || !captive_portal::global_captive_portal->is_active()) {
    return false;
  }
#ifdef USE_WIFI
  // with credentials configured the portal is a fallback AP, so the module is offline, not unprovisioned
  return wifi::global_wifi_component == nullptr || !wifi::global_wifi_component->has_sta();
#else
  return true;
#endif
#else
  return false;
#endif
}

const char *Miot::get_net_change_payload_() const {
  // "updating" may be customised per device (e.g. "updating force"), the state stays the same
  return this->ota_updating_ ? this->ota_net_indicator_.c_str() : this->get_net_reply_();
}

void Miot::queue_net_change_command(bool force) {
  const char *state = this->get_net_reply_();
  if (!force && state == this->last_net_reply_) {
    return;
  }
  const char *payload = this->get_net_change_payload_();
  ESP_LOGI(TAG, "Network status changed to '%s'", payload);
  this->queue_command("MIIO_net_change %s", payload);
  this->last_net_reply_ = state;
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

size_t Miot::count_tokens_(const char *args) {
  if (args == nullptr) {
    return 0;
  }
  size_t count = 0;
  bool in_token = false;
  bool in_quotes = false;
  for (const char *p = args; *p != 0; p++) {
    if (*p == '"') {
      in_quotes = !in_quotes;
      in_token = true;
    } else if (*p == ' ' && !in_quotes) {
      in_token = false;
    } else if (!in_token) {
      in_token = true;
      count++;
    }
  }
  return count;
}

MiotResultFormat Miot::resolve_result_format_(const char *args) const {
  const size_t tokens = count_tokens_(args);
  // <siid> <aiid> <code> followed by <piid> <value> pairs
  const bool fits_action = tokens >= 3 && (tokens - 3) % 2 == 0;
  // <siid> <piid> <code> triples
  const bool fits_set = tokens > 0 && tokens % 3 == 0;
  // <siid> <piid> <code> <value> quads
  const bool fits_get = tokens > 0 && tokens % 4 == 0;

  // a shape that only one interpretation fits settles it, whatever was handed out last
  if (fits_set && !fits_get) {
    return mrfSet;
  }
  if (fits_get && !fits_set) {
    return mrfGet;
  }
  // ambiguous counts (12, 24, ...) or mixed successes and failures: the pending command decides
  if (this->expect_action_result_) {
    return mrfAction;
  }
  if (this->expect_value_result_) {
    return mrfGet;
  }
  if (fits_set) {
    return mrfSet;
  }
  if (fits_get) {
    return mrfGet;
  }
  if (fits_action) {
    return mrfAction;
  }
  // nothing fits, let the parser report the error with the set_properties layout
  return mrfSet;
}

std::string Miot::unescape_value_(const char *value) {
  std::string result(value);
  // the spec escapes spaces inside string values as \u0020
  for (size_t pos = result.find("\\u0020"); pos != std::string::npos; pos = result.find("\\u0020", pos + 1)) {
    result.replace(pos, 6, " ");
  }
  return result;
}

bool Miot::is_well_formed_command_(const StringRef &cmd) {
  return cmd.size() >= 3 && std::islower(static_cast<unsigned char>(cmd[0])) &&
         std::islower(static_cast<unsigned char>(cmd[1])) && std::islower(static_cast<unsigned char>(cmd[2]));
}

bool Miot::set_model_(const char *model) {
  const size_t len = std::strlen(model);
  size_t dots = 0;
  bool valid = len > 0 && len <= MAX_MODEL_LENGTH;
  for (size_t i = 0; valid && i < len; i++) {
    const char c = model[i];
    valid = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '_' || c == '-';
    if (c == '.') {
      dots++;
    }
  }
  // a model is <company>.<category>.<version>
  valid = valid && dots >= 2;
  if (!valid) {
    ESP_LOGW(TAG, "Ignoring invalid model '%s'", this->get_printable_string(model).c_str());
    return false;
  }
  this->model_ = model;
  ESP_LOGI(TAG, "Model: %s", model);
  return true;
}

bool Miot::set_mcu_version_(const char *version) {
  if (version == nullptr) {
    ESP_LOGW(TAG, "MCU reported no version");
    return false;
  }
  // the spec requires exactly four digits
  const size_t len = std::strlen(version);
  bool valid = len == 4;
  for (size_t i = 0; valid && i < len; i++) {
    valid = std::isdigit(static_cast<unsigned char>(version[i])) != 0;
  }
  if (!valid) {
    ESP_LOGW(TAG, "Ignoring invalid MCU version '%s'", this->get_printable_string(version).c_str());
    return false;
  }
  this->mcu_version_ = version;
  ESP_LOGI(TAG, "MCU version: %s", version);
  return true;
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
      listener->func(MiotValue(unescape_value_(value)));
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
// mrfSet: result <siid> <piid> <code> ... <siid> <piid> <code>       (set_properties)
// mrfSet: result <siid> <piid> <code> <value> ...                     (get_properties)
// mrfAction: result <siid> <aiid> <code> <piid> <value> ... <piid> <value>
// mrfEvent: event_occured <siid> <eiid> <piid> <value> ... <piid> <value>
// Returns false if the message is structurally invalid, the MCU then receives an "error" reply.
bool Miot::update_properties(char **saveptr, MiotResultFormat format) {
  const char *siid = nullptr, *piid = nullptr, *aiid = nullptr, *eiid = nullptr, *code = nullptr, *value = nullptr;
  const char *delim = " ";
  // get_properties results carry a value, set_properties results do not
  const bool with_value = format == mrfGet || format == mrfAction || format == mrfEvent || format == mrfNotify;
  size_t applied = 0;

  if (format == mrfAction) {
    if (!(siid = strtok_r(nullptr, delim, saveptr))) {
      return false;
    }
    if (!(aiid = strtok_r(nullptr, delim, saveptr))) {
      return false;
    }
    if (!(code = strtok_r(nullptr, delim, saveptr))) {
      return false;
    }
    if (std::strcmp(code, "0")) {
      ESP_LOGE(TAG, "Result error on action %s:%s: %s", siid, aiid, code);
      return true;
    }
  }

  if (format == mrfEvent) {
    if (!(siid = strtok_r(nullptr, delim, saveptr))) {
      return false;
    }
    if (!(eiid = strtok_r(nullptr, delim, saveptr))) {
      return false;
    }
  }

  while (true) {
    delim = " ";

    if (format != mrfAction && format != mrfEvent) {
      siid = strtok_r(nullptr, delim, saveptr);
      if (siid == nullptr) {
        // end of the message, every entry has been consumed
        break;
      }
    }
    if (!(piid = strtok_r(nullptr, delim, saveptr))) {
      if (format == mrfAction || format == mrfEvent) {
        // the siid/eiid header was already consumed, so this is the end of the message
        break;
      }
      return false;
    }
    if (format == mrfSet || format == mrfGet) {
      if (!(code = strtok_r(nullptr, delim, saveptr))) {
        return false;
      }
      if (std::strcmp(code, "0")) {
        // 1 means the request is still in progress, the value follows via properties_changed
        if (std::strcmp(code, "1")) {
          ESP_LOGE(TAG, "Result error on property %s:%s: %s", siid, piid, code);
        } else {
          ESP_LOGD(TAG, "Result pending on property %s:%s", siid, piid);
        }
        continue;
      }
      if (!with_value) {
        applied++;
        continue;
      }
    }
    // peek if the next token starts with a " to properly parse string results containing spaces
    if (*saveptr && **saveptr == '"') {
      delim = "\"";
    }
    if (!(value = strtok_r(nullptr, delim, saveptr))) {
      return false;
    }

    this->update_property(parse_number<uint32_t>(siid).value_or(0u), parse_number<uint32_t>(piid).value_or(0u), value);
    applied++;
  }

  if (format == mrfEvent) {
    // send event after all properties have been updated
    this->process_event_(parse_number<uint32_t>(siid).value_or(0u), parse_number<uint32_t>(eiid).value_or(0u));
  }

  // the spec requires at least one property to report
  return format != mrfNotify || applied > 0;
}

const char *Miot::get_net_reply_() const {
  // an update in progress replaces the network state, as in the MIoT state diagram
  if (this->ota_updating_) {
    return NET_UPDATING;
  }
  if (remote_is_connected()) {
    return NET_CLOUD;
  }
#ifdef USE_NETWORK
  if (network::is_connected()) {
    return NET_LOCAL;
  }
#endif
  if (is_provisioning_()) {
    return NET_UAP;
  }
#ifdef USE_NETWORK
  if (network::is_disabled()) {
    return NET_UNPROV;
  }
#endif
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
    if (this->awaiting_result_ && this->require_result_) {
      // one question, one answer: this get_down should have been a result
      this->mcu_log("Received get_down while a result is pending");
      this->awaiting_result_ = false;
      this->expect_action_result_ = false;
      this->expect_value_result_ = false;
      this->send_reply_("error");
    } else if (this->command_queue_.empty()) {
      // nothing is pending, so a result from here on is unsolicited
      this->expect_action_result_ = false;
      this->expect_value_result_ = false;
      this->send_reply_("down none");
    } else {
      const std::string &next_cmd = this->command_queue_.front();
      this->expect_action_result_ = next_cmd.rfind("action ", 0, 7) == 0;
      this->expect_value_result_ = next_cmd.rfind("get_properties", 0, 14) == 0;
      this->awaiting_result_ = next_cmd.rfind("MIIO_", 0, 5) != 0 && next_cmd != "none";
      char reply[MAX_LINE_LENGTH + 6];  // "down " + command
      snprintf(reply, sizeof(reply), "down %s", next_cmd.c_str());
      this->send_reply_(reply);
      this->command_queue_.pop();
    }
  } else if (cmd == "properties_changed") {
    this->send_reply_(this->update_properties(&saveptr, mrfNotify) ? "ok" : "error");
  } else if (cmd == "event_occured") {
    this->send_reply_(this->update_properties(&saveptr, mrfEvent) ? "ok" : "error");
  } else if (cmd == "result") {
    this->awaiting_result_ = false;
    const MiotResultFormat format = this->resolve_result_format_(saveptr);
    this->send_reply_(this->update_properties(&saveptr, format) ? "ok" : "error");
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
    if (model == nullptr) {
      // without a parameter the MCU asks for the current model
      if (this->model_.empty()) {
        ESP_LOGW(TAG, "MCU queried the model before reporting it");
        this->send_reply_("error");
      } else {
        this->send_reply_(this->model_.c_str());
      }
    } else {
      this->send_reply_(this->set_model_(model) ? "ok" : "error");
    }
  } else if (cmd == "mcu_version") {
    this->send_reply_(this->set_mcu_version_(strtok_r(nullptr, " ", &saveptr)) ? "ok" : "error");
  } else if (cmd == "version") {
    // report the module firmware version, which is ESPHome now
    this->send_reply_(ESPHOME_VERSION);
  } else if (cmd == "reboot") {
    ESP_LOGI(TAG, "MCU requested a reboot");
    this->send_reply_("ok");
    App.safe_reboot();
  } else if (cmd == "error") {
    this->awaiting_result_ = false;
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
  } else if (cmd == "reset_wifi") {
    ESP_LOGI(TAG, "Resetting Wi-Fi configuration...");
#if defined(USE_WIFI)
    if (wifi::global_wifi_component != nullptr) {
      wifi::global_wifi_component->clear_sta();
    }
#endif
    this->send_reply_("ok");
    App.safe_reboot();
  } else if (cmd == "ble_config") {
    // we have no BLE remote to configure, but the command has to be answered
    const char *arg = strtok_r(nullptr, " ", &saveptr);
    if (arg != nullptr && std::strcmp(arg, "set") == 0) {
      const char *pid = strtok_r(nullptr, " ", &saveptr);
      const char *version = strtok_r(nullptr, " ", &saveptr);
      if (pid == nullptr || version == nullptr) {
        this->send_reply_("error");
      } else {
        ESP_LOGW(TAG, "Ignoring ble_config set %s %s, no BLE remote is configured",
                 this->get_printable_string(pid).c_str(), this->get_printable_string(version).c_str());
        this->send_reply_("ok");
      }
    } else if (arg != nullptr && std::strcmp(arg, "dump") == 0) {
      ESP_LOGW(TAG, "Ignoring ble_config dump, no BLE remote is configured");
      this->send_reply_("ok");
    } else {
      this->send_reply_("error");
    }
  } else if (cmd == "factory") {
    // the factory test router cannot be emulated
    ESP_LOGW(TAG, "Ignoring factory command, factory test mode is not supported");
    this->send_reply_("ok");
  } else if (cmd == "set_log_level") {
    // we run our own logger, so the requested level is dropped, but the command has to be answered
    const char *level = strtok_r(nullptr, " ", &saveptr);
    const char *requested = level != nullptr ? level : "-";
    ESP_LOGD(TAG, "Ignoring set_log_level %s, the module keeps its own log level",
             this->get_printable_string(requested).c_str());
    this->send_reply_("ok");
  } else if (!this->strict_commands_ && is_well_formed_command_(cmd)) {
    // lenient mode: acknowledge unrecognized but plausible commands, as older docs implied
    this->mcu_log("Unknown command '%s'", this->get_printable_rx_message().c_str());
    this->send_reply_("ok");
  } else {
    // the spec requires an "error" reply for commands that are not recognized, which also covers
    // MCUs that print e.g. debug info over UART
    this->mcu_log("Unknown command '%s'", this->get_printable_rx_message().c_str());
    this->send_reply_("error");
  }
}

}  // namespace esphome::miot
