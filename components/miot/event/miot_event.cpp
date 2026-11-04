#include "esphome/core/log.h"
#include "miot_event.h"

namespace esphome::miot {

ESPHOME_LOG_TAG(TAG, "miot.event");

void MiotEvent::setup() {
  this->parent_->register_event_listener(this->siid_, this->eiid_, [this]() { this->trigger(this->type_); });
}

void MiotEvent::dump_config() {
  LOG_EVENT("", "MIoT Event", this);
  ESP_LOGCONFIG(TAG, "  SIID: %" PRIu32 "\n  EIID: %" PRIu32 "\n  Type: %s", this->siid_, this->eiid_,
                this->type_.c_str());
}

}  // namespace esphome::miot
