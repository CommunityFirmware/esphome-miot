#include "esphome/core/log.h"
#include "miot_button.h"

namespace esphome::miot {

ESPHOME_LOG_TAG(TAG, "miot.button");

void MiotButton::dump_config() {
  LOG_BUTTON("", "MIoT Button", this);
  ESP_LOGCONFIG(TAG, "  SIID: %" PRIu32 "\n  AIID: %" PRIu32, this->siid_, this->aiid_);
  if (!this->action_args_.empty()) {
    ESP_LOGCONFIG(TAG, "  Args: %s", this->action_args_.c_str());
  }
}

void MiotButton::press_action() { this->parent_->execute_action(this->siid_, this->aiid_, this->action_args_); }

}  // namespace esphome::miot
