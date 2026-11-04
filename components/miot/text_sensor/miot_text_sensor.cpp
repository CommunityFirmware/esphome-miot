#include "esphome/core/log.h"
#include "miot_text_sensor.h"

namespace esphome::miot {

ESPHOME_LOG_TAG(TAG, "miot.text_sensor");

void MiotTextSensor::setup() {
  this->parent_->register_listener(this->siid_, this->piid_, this->poll_, mvtString, [this](const MiotValue &value) {
    this->publish_state(this->parent_->get_printable_string(value.as_string));
  });
}

void MiotTextSensor::dump_config() {
  LOG_TEXT_SENSOR("", "MIoT Text Sensor", this);
  ESP_LOGCONFIG(TAG, "  SIID: %" PRIu32 "\n  PIID: %" PRIu32 "\n  Poll: %s", this->siid_, this->piid_,
                YESNO(this->poll_));
}

}  // namespace esphome::miot
