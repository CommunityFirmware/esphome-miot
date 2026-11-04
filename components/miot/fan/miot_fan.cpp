#include "esphome/core/log.h"
#include "miot_fan.h"

namespace esphome::miot {

ESPHOME_LOG_TAG(TAG, "miot.fan");

// preset modes are implemented as described here:
// https://developers.home-assistant.io/docs/core/entity/fan/#preset-modes
// -> setting a speed percentage unsets the preset
// -> setting a preset sets the speed percentage to zero
// -> mcu speed values are not reported if the device is off or in preset mode

// all properties are currently always polled
// "state" and "oscillating" are currently true booleans ("true" and "false" on the wire)
// "direction" is a true boolean as well, where "true" means REVERSE
// "speed" values are 0=off [1:]=(max-min)/step
// an empty preset mode string declares the corresponding value as "manual mode", which is set before setting a manual
// speed

// TODO what about "restore_mode", untested, does it require anything here?

const MiotFan::PresetMode *MiotFan::find_preset_mode_by_value_(uint32_t value) const {
  for (const auto &mode : this->preset_modes_) {
    if (mode.value == value) {
      return &mode;
    }
  }
  return nullptr;
}

const MiotFan::PresetMode *MiotFan::find_preset_mode_by_name_(const StringRef &name) const {
  for (const auto &mode : this->preset_modes_) {
    if (name == mode.name) {
      return &mode;
    }
  }
  return nullptr;
}

void MiotFan::setup() {
  this->parent_->register_listener(this->state_siid_, this->state_piid_, true, mvtString,
                                   [this](const MiotValue &value) {
                                     this->state = value.as_string == "true";
                                     this->publish_state();
                                   });

  this->parent_->register_listener(this->speed_siid_, this->speed_piid_, true, mvtUInt, [this](const MiotValue &value) {
    if (!this->state) {
      if (value.as_uint != 0) {
        ESP_LOGW(TAG, "Ignoring MCU reported speed, fan is off");
      }
      return;
    }
    if (this->has_preset_mode()) {
      ESP_LOGW(TAG, "Ignoring MCU reported speed, in preset mode");
      return;
    }
    this->speed = (value.as_uint - this->speed_min_) / this->speed_step_ + 1;
    this->publish_state();
  });

  if (this->oscillating_siid_ != 0 && this->oscillating_piid_ != 0) {
    this->parent_->register_listener(this->oscillating_siid_, this->oscillating_piid_, true, mvtString,
                                     [this](const MiotValue &value) {
                                       this->oscillating = value.as_string == "true";
                                       this->publish_state();
                                     });
  }

  if (this->direction_siid_ != 0 && this->direction_piid_ != 0) {
    this->parent_->register_listener(
        this->direction_siid_, this->direction_piid_, true, mvtString, [this](const MiotValue &value) {
          this->direction = value.as_string == "true" ? fan::FanDirection::REVERSE : fan::FanDirection::FORWARD;
          this->publish_state();
        });
  }

  if (this->preset_modes_siid_ != 0 && this->preset_modes_piid_ != 0) {
    this->parent_->register_listener(
        this->preset_modes_siid_, this->preset_modes_piid_, true, mvtUInt, [this](const MiotValue &value) {
          if (this->manual_speed_preset_.has_value() && value.as_uint == *this->manual_speed_preset_) {
            this->clear_preset_mode_();
          } else {
            const PresetMode *mode = this->find_preset_mode_by_value_(value.as_uint);
            if (mode == nullptr) {
              ESP_LOGE(TAG, "Unknown preset mode value %" PRIu32, value.as_uint);
              return;
            }
            this->set_preset_mode_(mode->name);
            this->speed = 0;
          }
          this->publish_state();
        });
  }

  std::vector<const char *> modes;
  modes.reserve(this->preset_modes_.size());
  for (const auto &mode : this->preset_modes_) {
    modes.push_back(mode.name);
  }
  this->set_supported_preset_modes(modes);
}

void MiotFan::dump_config() {
  LOG_FAN("", "MIoT Fan", this);
  ESP_LOGCONFIG(TAG,
                "  State: %" PRIu32 ":%" PRIu32 "\n"
                "  Speed: %" PRIu32 ":%" PRIu32 " (min/max/step %" PRIu32 "/%" PRIu32 "/%" PRIu32 ")",
                this->state_siid_, this->state_piid_, this->speed_siid_, this->speed_piid_, this->speed_min_,
                this->speed_max_, this->speed_step_);

  if (this->oscillating_siid_ != 0 && this->oscillating_piid_ != 0) {
    ESP_LOGCONFIG(TAG, "  Oscillating: %" PRIu32 ":%" PRIu32, this->oscillating_siid_, this->oscillating_piid_);
  }
  if (this->direction_siid_ != 0 && this->direction_piid_ != 0) {
    ESP_LOGCONFIG(TAG, "  Direction: %" PRIu32 ":%" PRIu32, this->direction_siid_, this->direction_piid_);
  }
  if (this->preset_modes_siid_ != 0 && this->preset_modes_piid_ != 0) {
    ESP_LOGCONFIG(TAG, "  Preset modes: %" PRIu32 ":%" PRIu32, this->preset_modes_siid_, this->preset_modes_piid_);
    for (const auto &mode : this->preset_modes_) {
      ESP_LOGCONFIG(TAG, "    %" PRIu32 ": %s", mode.value, mode.name);
    }
    if (this->manual_speed_preset_.has_value()) {
      ESP_LOGCONFIG(TAG, "  Manual preset mode: %" PRIu32, *this->manual_speed_preset_);
    }
  }
}

fan::FanTraits MiotFan::get_traits() {
  fan::FanTraits traits(this->oscillating_siid_ != 0 && this->oscillating_piid_ != 0,
                        this->speed_siid_ != 0 && this->speed_piid_ != 0,
                        this->direction_siid_ != 0 && this->direction_piid_ != 0,
                        (this->speed_max_ - this->speed_min_) / this->speed_step_ + 1);

  this->wire_preset_modes_(traits);

  return traits;
}

void MiotFan::control(const fan::FanCall &call) {
  const PresetMode *mode = nullptr;

  const StringRef mode_from = this->get_preset_mode();
  const StringRef mode_to = StringRef::from_maybe_nullptr(call.get_preset_mode());

  if (this->preset_modes_siid_ != 0 && this->preset_modes_piid_ != 0 && call.has_preset_mode() && !mode_to.empty() &&
      (!this->has_preset_mode() || mode_from != mode_to)) {
    mode = this->find_preset_mode_by_name_(mode_to);
  }

  if (mode != nullptr) {
    this->speed = 0;
    this->parent_->set_property(this->preset_modes_siid_, this->preset_modes_piid_, MiotValue(mode->value));
  } else if (call.get_speed().has_value()) {
    this->clear_preset_mode_();
    if (this->manual_speed_preset_.has_value()) {
      this->parent_->set_property(this->preset_modes_siid_, this->preset_modes_piid_,
                                  MiotValue(*this->manual_speed_preset_));
    }
    this->parent_->set_property(this->speed_siid_, this->speed_piid_,
                                MiotValue(this->speed_min_ + *call.get_speed() * this->speed_step_ - 1));
  }
  if (this->oscillating_siid_ != 0 && this->oscillating_piid_ != 0 && call.get_oscillating().has_value()) {
    this->parent_->set_property(this->oscillating_siid_, this->oscillating_piid_,
                                MiotValue(*call.get_oscillating() ? "true" : "false"));
  }
  if (this->direction_siid_ != 0 && this->direction_piid_ != 0 && call.get_direction().has_value()) {
    this->parent_->set_property(this->direction_siid_, this->direction_piid_,
                                MiotValue(*call.get_direction() == fan::FanDirection::REVERSE ? "true" : "false"));
  }
  if (call.get_state().has_value()) {
    this->parent_->set_property(this->state_siid_, this->state_piid_, MiotValue(*call.get_state() ? "true" : "false"));
  }
}

}  // namespace esphome::miot
