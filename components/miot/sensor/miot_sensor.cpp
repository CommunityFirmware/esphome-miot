#include "esphome/core/log.h"
#include "miot_sensor.h"

namespace esphome::miot {

ESPHOME_LOG_TAG(TAG, "miot.sensor");

void MiotSensor::setup() {
  this->parent_->register_listener(this->siid_, this->piid_, this->poll_, mvtFloat,
                                   [this](const MiotValue &value) { this->publish_state(value.as_float); });
}

void MiotSensor::dump_config() {
  LOG_SENSOR("", "MIoT Sensor", this);
  ESP_LOGCONFIG(TAG, "  SIID: %" PRIu32 "\n  PIID: %" PRIu32 "\n  Poll: %s", this->siid_, this->piid_,
                YESNO(this->poll_));
}

}  // namespace esphome::miot
