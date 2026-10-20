# ESPHome DTMF Tone (external component)

External ESPHome component that generates DTMF as sine waves by modulating LEDC PWM duty (ESP32-C3, ESP-IDF).

Two pins match a hardware summer: `pin_low` is the low group (697/770/852/941 Hz), `pin_high` is the high group (1209/1336/1477/1633 Hz). Omit `pin_high` to mix both tones on one pin (quieter).

## Installation

```yaml
external_components:
  - source: github://pit3k/esphome-dtmf-tone
    components: [dtmf_tone]
```

## Example

```yaml
dtmf_tone:
  id: dtmf_gen
  pin_low: GPIO5
  pin_high: GPIO6
  carrier_frequency: 200000
  sample_rate: 32000
  amplitude: 100%
  high_tone_level: 80%

button:
  - platform: template
    name: "Play DTMF"
    on_press:
      - dtmf_tone.play:
          id: dtmf_gen
          sequence: "1234#"
          tone_length: 200ms
          inter_digit_gap: 80ms
```

`sequence` accepts `0-9`, `*`, `#`, `A-D` (case insensitive). Spaces are ignored. A comma is a pause the length of one tone.

Stop playback with `dtmf_tone.stop: dtmf_gen`.

See [`examples/basic.yaml`](examples/basic.yaml).

## Files

- `components/dtmf_tone/__init__.py`
- `components/dtmf_tone/dtmf_tone.h`
- `components/dtmf_tone/dtmf_tone.cpp`
