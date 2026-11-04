#include "esphome/core/log.h"
#include "miot_switch.h"

namespace esphome::miot {

ESPHOME_LOG_TAG(TAG, "miot.switch");

void MiotSwitch::setup() {
  this->parent_->register_listener(this->siid_, this->piid_, this->poll_, mvtString, [this](const MiotValue &value) {
    this->publish_state(value.as_string == this->true_value_);
  });
}

void MiotSwitch::write_state(bool state) {
  ESP_LOGV(TAG, "Setting switch %" PRIu32 ":%" PRIu32 " %s", this->siid_, this->piid_, ONOFF(state));
  this->parent_->set_property(this->siid_, this->piid_, MiotValue(state ? this->true_value_ : this->false_value_));
  this->publish_state(state);
}

void MiotSwitch::dump_config() {
  LOG_SWITCH("", "MIoT Switch", this);
  ESP_LOGCONFIG(TAG,
                "  SIID: %" PRIu32 "\n"
                "  PIID: %" PRIu32 "\n"
                "  Poll: %s\n"
                "  True/False: %s/%s",
                this->siid_, this->piid_, YESNO(this->poll_), this->true_value_.c_str(), this->false_value_.c_str());
}

}  // namespace esphome::miot
