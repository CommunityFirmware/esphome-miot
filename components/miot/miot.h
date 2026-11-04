#pragma once

#include <cinttypes>
#include <cstdarg>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/uart/uart.h"

#ifdef USE_OTA_STATE_LISTENER
#include "esphome/components/ota/ota_backend.h"
#endif

namespace esphome::miot {

enum MiotValueType : uint8_t {
  mvtInt,
  mvtUInt,
  mvtFloat,
  mvtString,
};

struct MiotValue {
  explicit MiotValue(int value) : type(mvtInt) { this->as_int = value; }
  explicit MiotValue(uint32_t value) : type(mvtUInt) { this->as_uint = value; }
  explicit MiotValue(float value) : type(mvtFloat) { this->as_float = value; }
  // mvtString values are put on the wire verbatim. Booleans and enums are sent unquoted
  // ("true", "false", numbers); a value for a MIoT *string* property must bring its own
  // double quotes, as the protocol is JSON based and requires them for strings.
  explicit MiotValue(const char *value) : type(mvtString), as_string(value != nullptr ? value : "") {}
  explicit MiotValue(const std::string &value) : type(mvtString), as_string(value) {}

  MiotValueType type;
  union {
    int as_int;
    uint32_t as_uint;
    float as_float;
  };
  std::string as_string;
};

struct MiotListener {
  uint32_t siid;
  uint32_t piid;
  bool poll;
  MiotValueType type;
  std::function<void(const MiotValue &value)> func;
};

#ifdef USE_EVENT
struct MiotEventListener {
  uint32_t siid;
  uint32_t eiid;
  std::function<void()> func;
};
#endif

enum MiotResultFormat {
  mrfNotify,  // properties_changed <siid> <piid> <value> ...
  mrfSet,     // result of set_properties: <siid> <piid> <code> ...
  mrfGet,     // result of get_properties: <siid> <piid> <code> <value> ...
  mrfAction,  // result of action: <siid> <aiid> <code> <piid> <value> ...
  mrfEvent,   // event_occured <siid> <eiid> <piid> <value> ...
};

class Miot : public Component,
#ifdef USE_OTA_STATE_LISTENER
             public ota::OTAGlobalStateListener,
#endif
             public uart::UARTDevice {
 public:
  /// Longest line the MCU can send before it is considered garbage.
  static constexpr size_t MAX_LINE_LENGTH = 512;
  /// Longest downlink command, leaving room for the "down " prefix and the CR terminator.
  static constexpr size_t MAX_COMMAND_LENGTH = MAX_LINE_LENGTH - 6;
  /// Maximum number of commands waiting for the MCU to pick them up.
  static constexpr size_t MAX_QUEUED_COMMANDS = 16;
  /// Maximum number of distinct MCU logs kept for dump_config().
  static constexpr size_t MAX_MCU_LOG_ENTRIES = 32;
  /// The MIoT spec limits the model string to 23 characters.
  static constexpr size_t MAX_MODEL_LENGTH = 23;

  float get_setup_priority() const override { return setup_priority::DATA; }
  void setup() override;
  void loop() override;
  void dump_config() override;

  /// Escape non-printable bytes so MCU supplied strings can be logged and published safely.
  std::string get_printable_string(const uint8_t *data, size_t len) const;
  std::string get_printable_string(const std::string &str) const;

  void set_heartbeat_config(uint32_t siid, uint32_t piid) {
    this->heartbeat_siid_ = siid;
    this->heartbeat_piid_ = piid;
  }
  void set_ota_net_indicator(const std::string &indicator) { this->ota_net_indicator_ = indicator; }
  /// Answer unrecognized commands with "error" instead of acknowledging well-formed ones.
  void set_strict_commands(bool strict) { this->strict_commands_ = strict; }
  /// Require the MCU to answer a downlink with "result" before serving the next one.
  void set_require_result(bool require) { this->require_result_ = require; }

  void register_listener(uint32_t siid, uint32_t piid, bool poll, MiotValueType type,
                         const std::function<void(const MiotValue &value)> &func);
  void queue_command(const std::string &cmd);
  void queue_command(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
  void queue_net_change_command(bool force);
  void set_property(uint32_t siid, uint32_t piid, const MiotValue &value);
  void execute_action(uint32_t siid, uint32_t aiid, const std::string &args);
#ifdef USE_EVENT
  void register_event_listener(uint32_t siid, uint32_t eiid, const std::function<void()> &func);
#endif

 protected:
  void poll_properties_();
  const char *get_net_reply_() const;
  /// The module is in provisioning mode, i.e. it has no configured network to connect to.
  static bool is_provisioning_();
  /// The value the MCU sees for the current state, "updating" may be customised per device.
  const char *get_net_change_payload_() const;
  std::string get_time_reply_(bool posix) const;
  /// The reply to ble_config dump, i.e. the product id and version the MCU set.
  std::string get_ble_config_reply_() const;
  void send_reply_(const char *reply);
  /// The spec requires a four digit version string, used for the MCU and the BLE version.
  static bool is_version_string_(const char *version);
  bool set_mcu_version_(const char *version);
  bool set_ble_config_(const char *pid, const char *version);
  /// A command name the MCU could plausibly have meant, used by the lenient mode.
  static bool is_well_formed_command_(const StringRef &cmd);
  /// Pick the layout of a "result" reply from the number of arguments it carries.
  MiotResultFormat resolve_result_format_(const char *args) const;
  static size_t count_tokens_(const char *args);
  bool set_model_(const char *model);
  void update_property(uint32_t siid, uint32_t piid, const char *value);
  bool update_properties(char **saveptr, MiotResultFormat format);
  void mcu_log(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
  void process_message_();
  void process_event_(uint32_t siid, uint32_t eiid);

  /// Replace the escapes the MCU uses for characters inside string values.
  static std::string unescape_value_(const char *value);

  MiotListener *find_listener_(uint32_t siid, uint32_t piid);
#ifdef USE_EVENT
  MiotEventListener *find_event_listener_(uint32_t siid, uint32_t eiid);
#endif

#ifdef USE_OTA_STATE_LISTENER
  void on_ota_global_state(ota::OTAState state, float progress, uint8_t error, ota::OTAComponent *comp) override;
#endif

  std::string get_printable_rx_message() const {
    return this->get_printable_string(this->rx_message_, this->rx_count_);
  }

  uint8_t rx_message_[MAX_LINE_LENGTH + 1];
  size_t rx_count_{0};
  uint32_t last_rx_char_timestamp_{0};
  const char *last_net_reply_{nullptr};
  std::string model_;
  std::string mcu_version_;
  /// BLE provisioning values from the MCU. This module has no BLE radio, they are only reported back.
  uint16_t ble_pid_{0};
  std::string ble_version_;
  std::vector<std::string> mcu_log_;
  StaticRingBuffer<std::string, MAX_QUEUED_COMMANDS> command_queue_;
  /// A downlink command that requires a result is in flight.
  bool awaiting_result_{false};
  bool expect_action_result_{false};
  /// The last delivered downlink was a get_properties, so a result carries values.
  bool expect_value_result_{false};
  uint32_t heartbeat_siid_{0};
  uint32_t heartbeat_piid_{0};
  std::string ota_net_indicator_;
  /// A firmware update is running, so the module reports the "updating" state.
  bool ota_updating_{false};
  bool strict_commands_{true};
  bool require_result_{true};
  std::vector<MiotListener> listeners_;
#ifdef USE_EVENT
  std::vector<MiotEventListener> event_listeners_;
#endif
};

}  // namespace esphome::miot
