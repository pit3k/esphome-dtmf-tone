#include "dtmf_tone.h"

#include "driver/gptimer.h"
#include "driver/ledc.h"
#include "esp_attr.h"
#include "soc/ledc_struct.h"

#include "esphome/core/log.h"

namespace esphome {
namespace dtmf_tone {

static const char *const TAG = "dtmf_tone";

// 64 probek, zakres -127..127. W DRAM, zeby ISR nie czytal flasha.
DRAM_ATTR static const int16_t SINE[64] = {
    0,    12,   25,   37,   49,   60,   71,  81,  90,  98,   106,  112, 117, 122, 125, 126,
    127,  126,  125,  122,  117,  112,  106, 98,  90,  81,   71,   60,  49,  37,  25,  12,
    0,    -12,  -25,  -37,  -49,  -60,  -71, -81, -90, -98,  -106, -112, -117, -122, -125, -126,
    -127, -126, -125, -122, -117, -112, -106, -98, -90, -81, -71,  -60, -49, -37, -25, -12,
};

struct Pair {
  char symbol;
  uint16_t lo;
  uint16_t hi;
};

DRAM_ATTR static const Pair PAIRS[] = {
    {'1', 697, 1209}, {'2', 697, 1336}, {'3', 697, 1477}, {'A', 697, 1633},
    {'4', 770, 1209}, {'5', 770, 1336}, {'6', 770, 1477}, {'B', 770, 1633},
    {'7', 852, 1209}, {'8', 852, 1336}, {'9', 852, 1477}, {'C', 852, 1633},
    {'*', 941, 1209}, {'0', 941, 1336}, {'#', 941, 1477}, {'D', 941, 1633},
};

static bool IRAM_ATTR lookup_freq(char c, uint16_t &lo, uint16_t &hi) {
  if (c >= 'a' && c <= 'd') {
    c = static_cast<char>(c - 'a' + 'A');
  }
  for (const auto &pair : PAIRS) {
    if (pair.symbol == c) {
      lo = pair.lo;
      hi = pair.hi;
      return true;
    }
  }
  return false;
}

static bool IRAM_ATTR on_alarm(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user) {
  (void) timer;
  (void) edata;
  static_cast<DtmfTone *>(user)->on_sample();
  return false;
}

static int IRAM_ATTR clamp_duty(int duty) {
  if (duty < 0) {
    return 0;
  }
  if (duty > 255) {
    return 255;
  }
  return duty;
}

void DtmfTone::setup() {
  uint32_t carrier = this->carrier_hz_;
  const uint32_t fallbacks[] = {carrier, 150000, 100000, 80000};
  esp_err_t err = ESP_FAIL;
  for (uint32_t hz : fallbacks) {
    if (hz < 20000 || hz > 300000) {
      continue;
    }
    ledc_timer_config_t timer = {};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = LEDC_TIMER_8_BIT;
    timer.timer_num = LEDC_TIMER_0;
    timer.freq_hz = hz;
    timer.clk_cfg = LEDC_AUTO_CLK;
    err = ledc_timer_config(&timer);
    if (err == ESP_OK) {
      carrier = hz;
      break;
    }
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "LEDC timer config failed: %d", err);
    return;
  }
  this->carrier_hz_ = carrier;

  ledc_channel_config_t low = {};
  low.gpio_num = this->pin_low_;
  low.speed_mode = LEDC_LOW_SPEED_MODE;
  low.channel = LEDC_CHANNEL_0;
  low.intr_type = LEDC_INTR_DISABLE;
  low.timer_sel = LEDC_TIMER_0;
  low.duty = 128;
  low.hpoint = 0;
  err = ledc_channel_config(&low);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "LEDC channel low failed: %d", err);
    return;
  }
  if (this->dual_) {
    ledc_channel_config_t high = low;
    high.gpio_num = this->pin_high_;
    high.channel = LEDC_CHANNEL_1;
    err = ledc_channel_config(&high);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "LEDC channel high failed: %d", err);
      return;
    }
  }
  this->write_pair(128, 128);

  const uint32_t res_hz = 10000000;
  gptimer_config_t cfg = {};
  cfg.clk_src = GPTIMER_CLK_SRC_DEFAULT;
  cfg.direction = GPTIMER_COUNT_UP;
  cfg.resolution_hz = res_hz;
  gptimer_handle_t handle = nullptr;
  err = gptimer_new_timer(&cfg, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "gptimer create failed: %d", err);
    return;
  }
  uint32_t alarm_ticks = (res_hz + this->sample_rate_ / 2) / this->sample_rate_;
  if (alarm_ticks < 1) {
    alarm_ticks = 1;
  }
  this->actual_rate_ = res_hz / alarm_ticks;

  gptimer_alarm_config_t alarm = {};
  alarm.alarm_count = alarm_ticks;
  alarm.reload_count = 0;
  alarm.flags.auto_reload_on_alarm = true;
  err = gptimer_set_alarm_action(handle, &alarm);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "gptimer alarm failed: %d", err);
    return;
  }
  gptimer_event_callbacks_t cbs = {};
  cbs.on_alarm = on_alarm;
  err = gptimer_register_event_callbacks(handle, &cbs, this);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "gptimer callback failed: %d", err);
    return;
  }
  err = gptimer_enable(handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "gptimer enable failed: %d", err);
    return;
  }
  this->timer_ = handle;
  this->ledc_ready_ = true;
  ESP_LOGI(TAG, "carrier %u Hz, sample rate %u Hz", this->carrier_hz_, this->actual_rate_);
}

void DtmfTone::dump_config() {
  ESP_LOGCONFIG(TAG, "DTMF sine:");
  ESP_LOGCONFIG(TAG, "  Pin low: GPIO%u", this->pin_low_);
  if (this->dual_) {
    ESP_LOGCONFIG(TAG, "  Pin high: GPIO%d", this->pin_high_);
  } else {
    ESP_LOGCONFIG(TAG, "  Tryb: jeden pin (suma obu tonow)");
  }
  ESP_LOGCONFIG(TAG, "  Carrier: %u Hz", this->carrier_hz_);
  ESP_LOGCONFIG(TAG, "  Sample rate: %u Hz", this->actual_rate_);
  ESP_LOGCONFIG(TAG, "  Amplitude: %u%%, high tone: %u%%", this->amplitude_pct_, this->high_pct_);
}

void IRAM_ATTR DtmfTone::write_pair(int duty_low, int duty_high) {
  auto &low = LEDC.channel_group[0].channel[0];
  low.duty.duty = static_cast<uint32_t>(clamp_duty(duty_low)) << 4;
  low.conf1.duty_scale = 0;
  low.conf1.duty_cycle = 1;
  low.conf1.duty_num = 1;
  low.conf1.duty_inc = 1;
  low.conf1.duty_start = 1;
  low.conf0.low_speed_update = 1;
  if (!this->dual_) {
    return;
  }
  auto &high = LEDC.channel_group[0].channel[1];
  high.duty.duty = static_cast<uint32_t>(clamp_duty(duty_high)) << 4;
  high.conf1.duty_scale = 0;
  high.conf1.duty_cycle = 1;
  high.conf1.duty_num = 1;
  high.conf1.duty_inc = 1;
  high.conf1.duty_start = 1;
  high.conf0.low_speed_update = 1;
}

bool IRAM_ATTR DtmfTone::select_symbol(uint8_t pos) {
  const char c = this->seq_[pos];
  uint16_t lo = 0;
  uint16_t hi = 0;
  if (c == ',') {
    lo = 0;
    hi = 0;
  } else if (!lookup_freq(c, lo, hi)) {
    return false;
  }
  this->freq_lo_ = lo;
  this->freq_hi_ = hi;
  this->phase_lo_ = 0;
  this->phase_hi_ = 0;
  this->step_lo_ = lo == 0 ? 0 : static_cast<uint32_t>((static_cast<uint64_t>(lo) << 32) / this->actual_rate_);
  this->step_hi_ = hi == 0 ? 0 : static_cast<uint32_t>((static_cast<uint64_t>(hi) << 32) / this->actual_rate_);
  this->sample_pos_ = 0;
  this->limit_ = this->tone_samples_ == 0 ? 1 : this->tone_samples_;
  this->seq_pos_ = pos;
  this->state_ = ST_TONE;
  return true;
}

int IRAM_ATTR DtmfTone::next_symbol(uint8_t from) const {
  for (uint8_t i = from; i < this->seq_len_; i++) {
    const char c = this->seq_[i];
    uint16_t lo;
    uint16_t hi;
    if (c == ',' || lookup_freq(c, lo, hi)) {
      return i;
    }
  }
  return -1;
}

void IRAM_ATTR DtmfTone::on_sample() {
  if (this->state_ == ST_IDLE) {
    this->write_pair(128, 128);
    return;
  }

  if (this->sample_pos_ >= this->limit_) {
    if (this->state_ == ST_TONE) {
      const int nxt = this->next_symbol(this->seq_pos_ + 1);
      if (nxt >= 0 && this->gap_samples_ > 0) {
        this->state_ = ST_GAP;
        this->sample_pos_ = 0;
        this->limit_ = this->gap_samples_;
        this->write_pair(128, 128);
        return;
      }
      if (nxt >= 0 && this->select_symbol(static_cast<uint8_t>(nxt))) {
        // wpadamy w render ponizej
      } else {
        this->state_ = ST_IDLE;
        this->write_pair(128, 128);
        return;
      }
    } else {
      const int nxt = this->next_symbol(this->seq_pos_ + 1);
      if (nxt < 0 || !this->select_symbol(static_cast<uint8_t>(nxt))) {
        this->state_ = ST_IDLE;
        this->write_pair(128, 128);
        return;
      }
    }
  }

  if (this->state_ == ST_GAP || (this->freq_lo_ == 0 && this->freq_hi_ == 0)) {
    this->sample_pos_++;
    this->write_pair(128, 128);
    return;
  }

  int32_t env = 256;
  if (this->ramp_samples_ > 0) {
    const uint32_t age = this->sample_pos_;
    const uint32_t left = this->limit_ > this->sample_pos_ ? this->limit_ - this->sample_pos_ : 0;
    const uint32_t edge = age < left ? age : left;
    if (edge < this->ramp_samples_) {
      env = static_cast<int32_t>((edge * 256) / this->ramp_samples_);
    }
  }

  const int32_t slo = SINE[this->phase_lo_ >> 26];
  const int32_t shi = SINE[this->phase_hi_ >> 26];
  this->phase_lo_ += this->step_lo_;
  this->phase_hi_ += this->step_hi_;
  this->sample_pos_++;

  int32_t alo = slo * this->amplitude_pct_ * env / (100 * 256);
  int32_t ahi = shi * this->amplitude_pct_ * this->high_pct_ * env / (100 * 100 * 256);
  if (!this->dual_) {
    const int mixed = clamp_duty(128 + static_cast<int>((alo + ahi) / 2));
    this->write_pair(mixed, 128);
    return;
  }
  this->write_pair(128 + static_cast<int>(alo), 128 + static_cast<int>(ahi));
}

void DtmfTone::loop() {
  if (this->timer_running_ && this->state_ == ST_IDLE && this->timer_ != nullptr) {
    gptimer_stop(static_cast<gptimer_handle_t>(this->timer_));
    this->timer_running_ = false;
    this->write_pair(128, 128);
  }
}

void DtmfTone::stop() {
  if (this->timer_ != nullptr && this->timer_running_) {
    gptimer_stop(static_cast<gptimer_handle_t>(this->timer_));
  }
  this->timer_running_ = false;
  this->state_ = ST_IDLE;
  if (this->ledc_ready_) {
    this->write_pair(128, 128);
  }
}

void DtmfTone::play(const std::string &sequence, uint32_t tone_ms, uint32_t gap_ms) {
  if (!this->ledc_ready_ || this->timer_ == nullptr) {
    ESP_LOGW(TAG, "generator nie gotowy");
    return;
  }
  this->stop();

  uint8_t len = 0;
  for (char raw : sequence) {
    if (len >= sizeof(this->seq_) - 1) {
      break;
    }
    if (raw == ' ' || raw == '\t') {
      continue;
    }
    char c = raw;
    if (c >= 'a' && c <= 'd') {
      c = static_cast<char>(c - 'a' + 'A');
    }
    uint16_t lo;
    uint16_t hi;
    if (c == ',' || lookup_freq(c, lo, hi)) {
      this->seq_[len++] = c;
    } else {
      ESP_LOGW(TAG, "pomijam znak '%c'", raw);
    }
  }
  this->seq_[len] = '\0';
  this->seq_len_ = len;
  if (len == 0) {
    ESP_LOGW(TAG, "pusta sekwencja");
    return;
  }

  const uint32_t rate = this->actual_rate_ == 0 ? this->sample_rate_ : this->actual_rate_;
  this->tone_samples_ = static_cast<uint32_t>((static_cast<uint64_t>(tone_ms) * rate) / 1000);
  if (this->tone_samples_ < 1) {
    this->tone_samples_ = 1;
  }
  this->gap_samples_ = static_cast<uint32_t>((static_cast<uint64_t>(gap_ms) * rate) / 1000);
  uint32_t ramp = (4 * rate) / 1000;
  if (ramp * 2 >= this->tone_samples_) {
    ramp = this->tone_samples_ / 4;
  }
  this->ramp_samples_ = ramp;

  const int first = this->next_symbol(0);
  if (first < 0 || !this->select_symbol(static_cast<uint8_t>(first))) {
    return;
  }
  ESP_LOGI(TAG, "play \"%s\" tone=%ums gap=%ums", this->seq_, tone_ms, gap_ms);
  esp_err_t err = gptimer_start(static_cast<gptimer_handle_t>(this->timer_));
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "gptimer start failed: %d", err);
    this->state_ = ST_IDLE;
    return;
  }
  this->timer_running_ = true;
}

}  // namespace dtmf_tone
}  // namespace esphome
