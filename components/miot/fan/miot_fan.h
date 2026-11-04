#pragma once

#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/string_ref.h"
#include "esphome/components/fan/fan.h"
#include "../miot.h"

namespace esphome::miot {

class MiotFan : public Component, public fan::Fan {
 public:
  /// Maps a MCU property value to a preset mode name.
  struct PresetMode {
    uint32_t value;
    const char *name;
  };

  MiotFan(Miot *parent) : parent_(parent) {}

  void setup() override;
  void dump_config() override;

  void set_state_config(uint32_t siid, uint32_t piid) {
    this->state_siid_ = siid;
    this->state_piid_ = piid;
  }
  void set_speed_config(uint32_t siid, uint32_t piid, uint32_t min, uint32_t max, uint32_t step) {
    this->speed_siid_ = siid;
    this->speed_piid_ = piid;
    this->speed_min_ = min;
    this->speed_max_ = max;
    this->speed_step_ = step;
  }
  void set_oscillating_config(uint32_t siid, uint32_t piid) {
    this->oscillating_siid_ = siid;
    this->oscillating_piid_ = piid;
  }
  void set_direction_config(uint32_t siid, uint32_t piid) {
    this->direction_siid_ = siid;
    this->direction_piid_ = piid;
  }
  void set_preset_modes_config(uint32_t siid, uint32_t piid) {
    this->preset_modes_siid_ = siid;
    this->preset_modes_piid_ = piid;
  }
  void set_preset_mode_name(uint32_t value, const char *name) {
    if (name == nullptr || *name == 0) {
      // an empty name declares the value as "manual mode", set before a manual speed
      this->manual_speed_preset_ = value;
    } else {
      this->preset_modes_.push_back(PresetMode{value, name});
    }
  }

  fan::FanTraits get_traits() override;

 protected:
  void control(const fan::FanCall &call) override;

  const PresetMode *find_preset_mode_by_value_(uint32_t value) const;
  const PresetMode *find_preset_mode_by_name_(const StringRef &name) const;

  Miot *parent_{nullptr};
  uint32_t state_siid_{0};
  uint32_t state_piid_{0};
  uint32_t speed_siid_{0};
  uint32_t speed_piid_{0};
  uint32_t speed_min_{0};
  uint32_t speed_max_{0};
  uint32_t speed_step_{1};
  uint32_t oscillating_siid_{0};
  uint32_t oscillating_piid_{0};
  uint32_t direction_siid_{0};
  uint32_t direction_piid_{0};
  uint32_t preset_modes_siid_{0};
  uint32_t preset_modes_piid_{0};
  std::vector<PresetMode> preset_modes_;
  optional<uint32_t> manual_speed_preset_{};
};

}  // namespace esphome::miot
