#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/light/light_output.h"
#include "esphome/components/light/addressable_light.h"

#ifdef ARDUINO_ESP8266_RELEASE_2_3_0
#error The NeoPixelBus library requires at least arduino_core_version 2.4.x
#endif

#ifdef USE_POWER_SUPPLY
#include "esphome/components/power_supply/power_supply.h"
#endif

#include "NeoPixelBus.h"

namespace esphome {
namespace neopixelbus {

enum class ESPNeoPixelOrder {
  GBWR = 0b11000110,
  GBRW = 0b10000111,
  GBR = 0b10000111,
  GWBR = 0b11001001,
  GRBW = 0b01001011,
  GRB = 0b01001011,
  GWRB = 0b10001101,
  GRWB = 0b01001110,
  BGWR = 0b11010010,
  BGRW = 0b10010011,
  BGR = 0b10010011,
  WGBR = 0b11011000,
  RGBW = 0b00011011,
  RGB = 0b00011011,
  WGRB = 0b10011100,
  RGWB = 0b00011110,
  BWGR = 0b11100001,
  BRGW = 0b01100011,
  BRG = 0b01100011,
  WBGR = 0b11100100,
  RBGW = 0b00100111,
  RBG = 0b00100111,
  WRGB = 0b01101100,
  RWGB = 0b00101101,
  BWRG = 0b10110001,
  BRWG = 0b01110010,
  WBRG = 0b10110100,
  RBWG = 0b00110110,
  WRBG = 0b01111000,
  RWBG = 0b00111001,
};

template<typename T_METHOD, typename T_COLOR_FEATURE>
class NeoPixelBusLightOutputBase : public Component, public light::AddressableLight {
 public:
#ifdef USE_POWER_SUPPLY
  void set_power_supply(power_supply::PowerSupply *power_supply) { this->power_supply_ = power_supply; }
#endif

  NeoPixelBus<T_COLOR_FEATURE, T_METHOD> *get_controller() const { return this->controller_; }

  void clear_effect_data() override {
    for (int i = 0; i < this->size(); i++)
      this->effect_data_[i] = 0;
  }

  /// Add some LEDS, can only be called once.
  void add_leds(uint16_t count_pixels, uint8_t pin) {
    this->add_leds(new NeoPixelBus<T_COLOR_FEATURE, T_METHOD>(count_pixels, pin));
  }
  void add_leds(uint16_t count_pixels, uint8_t pin_clock, uint8_t pin_data) {
    this->add_leds(new NeoPixelBus<T_COLOR_FEATURE, T_METHOD>(count_pixels, pin_clock, pin_data));
  }
  void add_leds(uint16_t count_pixels) { this->add_leds(new NeoPixelBus<T_COLOR_FEATURE, T_METHOD>(count_pixels)); }
  void add_leds(NeoPixelBus<T_COLOR_FEATURE, T_METHOD> *controller) {
    this->controller_ = controller;
    this->controller_->Begin();
  }

  // ========== INTERNAL METHODS ==========
  void setup() override {
    for (int i = 0; i < this->size(); i++) {
      (*this)[i] = light::ESPColor(0, 0, 0, 0);
    }

    this->effect_data_ = new uint8_t[this->size()];
    this->controller_->Begin();
  }

  void loop() override {
    if (!this->should_show_())
      return;

    this->mark_shown_();
    this->controller_->Dirty();

#ifdef USE_POWER_SUPPLY
    if (this->power_supply_ != nullptr) {
      bool is_light_on = false;
      for (int i = 0; i < this->size(); i++) {
        if ((*this)[i].get().is_on()) {
          is_light_on = true;
          break;
        }
      }

      if (is_light_on && !this->has_requested_high_power_) {
        this->power_supply_->request_high_power();
        this->has_requested_high_power_ = true;
      }
      if (!is_light_on && this->has_requested_high_power_) {
        this->power_supply_->unrequest_high_power();
        this->has_requested_high_power_ = false;
      }
    }
#endif

    this->controller_->Show();
  }

  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  int32_t size() const override { return this->controller_->PixelCount(); }

  void set_pixel_order(ESPNeoPixelOrder order) {
    uint8_t u_order = static_cast<uint8_t>(order);
    this->rgb_offsets_[0] = (u_order >> 6) & 0b11;
    this->rgb_offsets_[1] = (u_order >> 4) & 0b11;
    this->rgb_offsets_[2] = (u_order >> 2) & 0b11;
    this->rgb_offsets_[3] = (u_order >> 0) & 0b11;
  }

 protected:
  NeoPixelBus<T_COLOR_FEATURE, T_METHOD> *controller_{nullptr};
  uint8_t *effect_data_{nullptr};
  uint8_t rgb_offsets_[4]{0, 1, 2, 3};
#ifdef USE_POWER_SUPPLY
  power_supply::PowerSupply *power_supply_{nullptr};
  bool has_requested_high_power_{false};
#endif
};

template<typename T_METHOD, typename T_COLOR_FEATURE = NeoRgbFeature>
class NeoPixelRGBLightOutput : public NeoPixelBusLightOutputBase<T_METHOD, T_COLOR_FEATURE> {
 public:
  inline light::ESPColorView operator[](int32_t index) const override {
    uint8_t *base = this->controller_->Pixels() + 3ULL * index;
    return light::ESPColorView(base + this->rgb_offsets_[0], base + this->rgb_offsets_[1], base + this->rgb_offsets_[2],
                               nullptr, this->effect_data_ + index, &this->correction_);
  }

  light::LightTraits get_traits() override {
    auto traits = light::LightTraits();
    traits.set_supports_brightness(true);
    traits.set_supports_rgb(true);
    return traits;
  }
};

template<typename T_METHOD, typename T_COLOR_FEATURE = NeoRgbwFeature>
class NeoPixelRGBWLightOutput : public NeoPixelBusLightOutputBase<T_METHOD, T_COLOR_FEATURE> {
 public:
  inline light::ESPColorView operator[](int32_t index) const override {
    uint8_t *base = this->controller_->Pixels() + 4ULL * index;
    return light::ESPColorView(base + this->rgb_offsets_[0], base + this->rgb_offsets_[1], base + this->rgb_offsets_[2],
                               base + this->rgb_offsets_[3], this->effect_data_ + index, &this->correction_);
  }

  light::LightTraits get_traits() override {
    auto traits = light::LightTraits();
    traits.set_supports_brightness(true);
    traits.set_supports_rgb(true);
    traits.set_supports_rgb_white_value(true);
    return traits;
  }
};

}  // namespace neopixelbus
}  // namespace esphome