#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include <string>

namespace esphome {
namespace dtmf_tone {

class DtmfTone : public Component {
 public:
  void set_pin_low(uint8_t pin) { this->pin_low_ = pin; }
  void set_pin_high(int pin) {
    this->pin_high_ = pin;
    this->dual_ = pin >= 0;
  }
  void set_carrier_frequency(uint32_t hz) { this->carrier_hz_ = hz; }
  void set_sample_rate(uint32_t hz) { this->sample_rate_ = hz; }
  void set_amplitude_pct(uint8_t pct) { this->amplitude_pct_ = pct; }
  void set_high_pct(uint8_t pct) { this->high_pct_ = pct; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  // tone_ms: dlugosc jednego symbolu, gap_ms: cisza miedzy symbolami.
  // Sekwencja: 0-9 * # A-D. Spacja jest ignorowana. Przecinek = pauza o dlugosci tonu.
  void play(const std::string &sequence, uint32_t tone_ms, uint32_t gap_ms);
  void stop();
  bool is_playing() const { return this->state_ != ST_IDLE; }
  void IRAM_ATTR on_sample();

 protected:
  void IRAM_ATTR write_pair(int duty_low, int duty_high);
  bool IRAM_ATTR select_symbol(uint8_t pos);
  int IRAM_ATTR next_symbol(uint8_t from) const;

  uint8_t pin_low_{5};
  int pin_high_{-1};
  bool dual_{false};
  uint32_t carrier_hz_{200000};
  uint32_t sample_rate_{32000};
  uint32_t actual_rate_{32000};
  uint8_t amplitude_pct_{100};
  uint8_t high_pct_{80};

  enum : uint8_t { ST_IDLE = 0, ST_TONE = 1, ST_GAP = 2 };
  volatile uint8_t state_{ST_IDLE};
  volatile bool timer_running_{false};

  char seq_[65]{};
  uint8_t seq_len_{0};
  uint8_t seq_pos_{0};
  uint16_t freq_lo_{0};
  uint16_t freq_hi_{0};
  uint32_t step_lo_{0};
  uint32_t step_hi_{0};
  uint32_t phase_lo_{0};
  uint32_t phase_hi_{0};
  uint32_t sample_pos_{0};
  uint32_t limit_{0};
  uint32_t tone_samples_{1};
  uint32_t gap_samples_{0};
  uint32_t ramp_samples_{0};

  void *timer_{nullptr};
  bool ledc_ready_{false};
};

template<typename... Ts> class PlayAction : public Action<Ts...>, public Parented<DtmfTone> {
 public:
  TEMPLATABLE_VALUE(std::string, sequence)
  TEMPLATABLE_VALUE(uint32_t, tone_length)
  TEMPLATABLE_VALUE(uint32_t, inter_digit_gap)

  void play(Ts... x) override {
    this->parent_->play(this->sequence_.value(x...), this->tone_length_.value(x...), this->inter_digit_gap_.value(x...));
  }
};

template<typename... Ts> class StopAction : public Action<Ts...>, public Parented<DtmfTone> {
 public:
  void play(Ts... x) override { this->parent_->stop(); }
};

}  // namespace dtmf_tone
}  // namespace esphome
