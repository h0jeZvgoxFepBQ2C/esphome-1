#pragma once

#include "esphome/core/component.h"
#include "esphome/core/esphal.h"
#include "esphome/components/sensor/sensor.h"

namespace esphome {
namespace rotary_encoder {

/// All possible resolutions for the rotary encoder
enum RotaryEncoderResolution {
  ROTARY_ENCODER_1_PULSE_PER_CYCLE =
      0x4400,  /// increment counter by 1 with every A-B cycle, slow response but accurate
  ROTARY_ENCODER_2_PULSES_PER_CYCLE = 0x2200,  /// increment counter by 2 with every A-B cycle
  ROTARY_ENCODER_4_PULSES_PER_CYCLE = 0x1100,  /// increment counter by 4 with every A-B cycle, most inaccurate
};

struct RotaryEncoderSensorStore {
  ISRInternalGPIOPin *pin_a;
  ISRInternalGPIOPin *pin_b;

  volatile int32_t counter{0};
  RotaryEncoderResolution resolution{ROTARY_ENCODER_1_PULSE_PER_CYCLE};
  int32_t min_value{INT32_MIN};
  int32_t max_value{INT32_MAX};
  int32_t last_read{0};
  uint8_t state{0};

  static void gpio_intr(RotaryEncoderSensorStore *arg);
};

class RotaryEncoderSensor : public sensor::Sensor, public Component {
 public:
  void set_pin_a(GPIOPin *pin_a) { pin_a_ = pin_a; }
  void set_pin_b(GPIOPin *pin_b) { pin_b_ = pin_b; }

  /** Set the resolution of the rotary encoder.
   *
   * By default, this component will increment the counter by 1 with every A-B input cycle.
   * You can however change this behavior to have more coarse resolutions like 4 counter increases per A-B cycle.
   *
   * @param mode The new mode of the encoder.
   */
  void set_resolution(RotaryEncoderResolution mode);

  void set_reset_pin(GPIOPin *pin_i) { this->pin_i_ = pin_i; }
  void set_min_value(int32_t min_value);
  void set_max_value(int32_t max_value);

  // ========== INTERNAL METHODS ==========
  // (In most use cases you won't need these)
  void setup() override;
  void dump_config() override;
  void loop() override;

  float get_setup_priority() const override;

 protected:
  GPIOPin *pin_a_;
  GPIOPin *pin_b_;
  GPIOPin *pin_i_{nullptr};  /// Index pin, if this is not nullptr, the counter will reset to 0 once this pin is HIGH.

  RotaryEncoderSensorStore store_{};
};

}  // namespace rotary_encoder
}  // namespace esphome
