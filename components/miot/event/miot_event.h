#pragma once

#include <string>

#include "esphome/core/component.h"
#include "esphome/components/event/event.h"
#include "../miot.h"

namespace esphome::miot {

class MiotEvent : public Component, public event::Event {
 public:
  void setup() override;
  void dump_config() override;

  void set_miot_config(Miot *parent, uint32_t siid, uint32_t eiid, const std::string &type) {
    this->parent_ = parent;
    this->siid_ = siid;
    this->eiid_ = eiid;
    this->type_ = type;
  }

 protected:
  Miot *parent_{nullptr};
  uint32_t siid_{0};
  uint32_t eiid_{0};
  std::string type_;
};

}  // namespace esphome::miot
