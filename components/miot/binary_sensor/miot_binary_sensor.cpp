#include "esphome/core/log.h"
#include "miot_binary_sensor.h"

namespace esphome::miot {

ESPHOME_LOG_TAG(TAG, "miot.binary_sensor");

void MiotBinarySensor::setup() {
  this->parent_->register_listener(this->siid_, this->piid_, this->poll_, mvtString, [this](const MiotValue &value) {
    this->publish_state(value.as_string == this->true_value_);
  });
}

void MiotBinarySensor::dump_config() {
  LOG_BINARY_SENSOR("", "MIoT Binary Sensor", this);
  ESP_LOGCONFIG(TAG,
                "  SIID: %" PRIu32 "\n"
                "  PIID: %" PRIu32 "\n"
                "  Poll: %s\n"
                "  True/False: %s/%s",
                this->siid_, this->piid_, YESNO(this->poll_), this->true_value_.c_str(), this->false_value_.c_str());
}

}  // namespace esphome::miot
