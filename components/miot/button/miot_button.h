#pragma once

#include <string>

#include "esphome/core/component.h"
#include "esphome/components/button/button.h"
#include "../miot.h"

namespace esphome::miot {

class MiotButton : public button::Button, public Component {
 public:
  void dump_config() override;

  void set_miot_config(Miot *parent, uint32_t siid, uint32_t aiid) {
    this->parent_ = parent;
    this->siid_ = siid;
    this->aiid_ = aiid;
  }

  void set_action_args(const std::string &action_args) { this->action_args_ = action_args; }

 protected:
  void press_action() override;

  Miot *parent_{nullptr};
  uint32_t siid_{0};
  uint32_t aiid_{0};
  std::string action_args_;
};

}  // namespace esphome::miot
