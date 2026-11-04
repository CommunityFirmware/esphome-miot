#include "esphome/core/log.h"
#include "miot_select.h"

namespace esphome::miot {

ESPHOME_LOG_TAG(TAG, "miot.select");

void MiotSelect::setup() {
  this->parent_->register_listener(this->siid_, this->piid_, this->poll_, mvtUInt, [this](const MiotValue &value) {
    const auto &mappings = this->mappings_;
    auto it = std::find(mappings.cbegin(), mappings.cend(), value.as_uint);
    if (it == mappings.cend()) {
      ESP_LOGW(TAG, "Invalid value %" PRIu32, value.as_uint);
      return;
    }

    const size_t mapping_idx = std::distance(mappings.cbegin(), it);
    const auto mapping_value = this->at(mapping_idx);
    if (!mapping_value.has_value()) {
      ESP_LOGW(TAG, "Invalid value %" PRIu32, value.as_uint);
      return;
    }
    this->publish_state(*mapping_value);
  });
}

void MiotSelect::control(const std::string &value) {
  auto idx = this->index_of(value);
  if (!idx.has_value()) {
    ESP_LOGW(TAG, "Invalid value %s", value.c_str());
    return;
  }

  const uint32_t mapping = this->mappings_.at(idx.value());
  ESP_LOGV(TAG, "Setting select %" PRIu32 ":%" PRIu32 " value to %" PRIu32 ":%s", this->siid_, this->piid_, mapping,
           value.c_str());
  this->parent_->set_property(this->siid_, this->piid_, MiotValue(mapping));
}

void MiotSelect::dump_config() {
  LOG_SELECT("", "MIoT Select", this);
  ESP_LOGCONFIG(TAG, "  SIID: %" PRIu32 "\n  PIID: %" PRIu32 "\n  Poll: %s", this->siid_, this->piid_,
                YESNO(this->poll_));

  const auto &options = this->traits.get_options();
  if (options.empty()) {
    return;
  }
  ESP_LOGCONFIG(TAG, "  Options are:");
  for (size_t i = 0; i < this->mappings_.size() && i < options.size(); i++) {
    ESP_LOGCONFIG(TAG, "    %" PRIu32 ": %s", this->mappings_[i], options[i]);
  }
}

}  // namespace esphome::miot
