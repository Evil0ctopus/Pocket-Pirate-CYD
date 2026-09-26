#include "audio.h"

#include <ESP_I2S.h>
#include <Preferences.h>
#include <Wire.h>

#include <math.h>

#include "app.h"
#include "board_pins.h"

using namespace CheapBlackDisplay;

namespace audio {
namespace {

constexpr uint8_t kEsAddr = 0x18;  // confirmed by on-device I2C scan
constexpr int kRate = 16000;

I2SClass g_i2s;
bool g_ready = false;
uint8_t g_vol = 80;  // percent

bool esWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(kEsAddr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool esRead(uint8_t reg, uint8_t& val) {
  Wire.beginTransmission(kEsAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)kEsAddr, 1) != 1) return false;
  val = Wire.read();
  return true;
}

// ES8311 init for DAC playback, slave mode, 16-bit I2S, MCLK = 256*fs.
// Register sequence and values per Espressif's es8311 driver (Apache-2.0):
// coeff row for (mclk=256*fs): pre_div=1 pre_multi=0 adc/dac_div=1 fs_mode=0
// lrck_h=0x00 lrck_l=0xFF bclk_div=4 adc_osr=dac_osr=0x10.
bool esInit() {
  uint8_t v;
  if (!esRead(0x00, v)) return false;  // codec present?

  if (!esWrite(0x00, 0x1F)) return false;  // reset
  delay(20);
  esWrite(0x00, 0x00);
  esWrite(0x00, 0x80);  // power-on (CSM)

  esWrite(0x01, 0x3F);  // clocks on, MCLK from MCLK pin, not inverted

  esRead(0x02, v);      // pre_div=1, pre_multi=0 (keep low 3 bits)
  esWrite(0x02, v & 0x07);
  esWrite(0x03, 0x10);  // fs_mode=0 | adc_osr 0x10
  esWrite(0x04, 0x10);  // dac_osr
  esWrite(0x05, 0x00);  // adc_div=1, dac_div=1
  esRead(0x06, v);      // bclk_div=4 -> (4-1)
  esWrite(0x06, (v & 0xE0) | 0x03);
  esRead(0x07, v);      // lrck_h
  esWrite(0x07, v & 0xC0);
  esWrite(0x08, 0xFF);  // lrck_l

  esRead(0x00, v);      // slave mode
  esWrite(0x00, v & 0xBF);
  esWrite(0x09, 0x0C);  // SDP in: I2S, 16-bit
  esWrite(0x0A, 0x0C);  // SDP out: I2S, 16-bit

  esWrite(0x0D, 0x01);  // power up analog
  esWrite(0x0E, 0x02);  // PGA + ADC modulator
  esWrite(0x12, 0x00);  // power up DAC
  esWrite(0x13, 0x10);  // enable output to HP drive
  esWrite(0x1C, 0x6A);  // ADC EQ bypass, DC offset cancel
  esWrite(0x37, 0x08);  // bypass DAC EQ

  esWrite(0x32, (uint8_t)(g_vol == 0 ? 0 : (g_vol * 256 / 100) - 1));
  esWrite(0x31, 0x00);  // unmuted
  return true;
}

// Render one sine note with a quick attack/decay envelope, blocking write.
void note(float freq, int ms, float amp = 0.55f) {
  const int total = kRate * ms / 1000;
  const int attack = kRate / 200;              // 5 ms
  const int decay = total / 3;
  static int16_t buf[512];                     // 256 stereo frames
  int done = 0;
  float phase = 0, dp = 2.0f * PI * freq / kRate;
  while (done < total) {
    int n = min(256, total - done);
    for (int i = 0; i < n; i++) {
      int idx = done + i;
      float env = 1.0f;
      if (idx < attack) env = idx / (float)attack;
      int fromEnd = total - idx;
      if (fromEnd < decay) env *= fromEnd / (float)decay;
      int16_t s = (int16_t)(sinf(phase) * 32767 * amp * env);
      phase += dp;
      buf[2 * i] = s;
      buf[2 * i + 1] = s;
    }
    g_i2s.write((uint8_t*)buf, n * 4);
    done += n;
  }
}

void silence(int ms) {
  static int16_t z[512] = {0};
  int total = kRate * ms / 1000, done = 0;
  while (done < total) {
    int n = min(256, total - done);
    g_i2s.write((uint8_t*)z, n * 4);
    done += n;
  }
}

bool gate() { return g_ready && app::sound(); }

}  // namespace

void begin() {
  Preferences p;
  p.begin("set", true);
  g_vol = p.getUChar("vol", 80);
  p.end();

  pinMode(AUDIO_ENABLE, OUTPUT);
  digitalWrite(AUDIO_ENABLE, LOW);

  g_i2s.setPins(I2S_SCK, I2S_LRC, I2S_SDO, I2S_SDI, I2S_MCK);
  if (!g_i2s.begin(I2S_MODE_STD, kRate, I2S_DATA_BIT_WIDTH_16BIT,
                   I2S_SLOT_MODE_STEREO)) {
    Serial.println("[audio] I2S begin failed");
    return;
  }
  if (!esInit()) {
    Serial.println("[audio] ES8311 not responding at 0x18");
    return;
  }
  digitalWrite(AUDIO_ENABLE, HIGH);  // power amp on
  g_ready = true;
  Serial.println("[audio] ES8311 ready (16kHz, 16-bit)");
}

bool ready() { return g_ready; }

void setVolume(uint8_t pct) {
  if (pct > 100) pct = 100;
  g_vol = pct;
  Preferences p;
  p.begin("set", false);
  p.putUChar("vol", g_vol);
  p.end();
  if (g_ready)
    esWrite(0x32, (uint8_t)(g_vol == 0 ? 0 : (g_vol * 256 / 100) - 1));
}
uint8_t volume() { return g_vol; }

void setAmp(bool on) { digitalWrite(AUDIO_ENABLE, on ? HIGH : LOW); }

void dumpRegs() {
  Serial.print("[audio] regs:");
  const uint8_t regs[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                          0x08, 0x09, 0x0A, 0x0D, 0x0E, 0x12, 0x13, 0x14,
                          0x1C, 0x31, 0x32, 0x37};
  for (uint8_t r : regs) {
    uint8_t v = 0xEE;
    esRead(r, v);
    Serial.printf(" %02X=%02X", r, v);
  }
  Serial.printf(" ready=%d vol=%u\n", (int)g_ready, g_vol);
}

void tapChirp() {
  if (!gate()) return;
  // Soft wood-block / coin tap — keep under ~40ms so taps stay snappy.
  note(988, 18, 0.30f);
  note(740, 20, 0.20f);
}

void chime() {
  if (!gate()) return;
  // Rising "ahoy" — minor-pentatonic pirate whistle.
  note(294, 90, 0.45f);   // D4
  note(349, 90, 0.48f);   // F4
  note(440, 110, 0.52f);  // A4
  note(523, 180, 0.55f);  // C5
  silence(30);
}

void fanfare() {
  if (!gate()) return;
  // Level-up treasure jingle (bright major arpeggio + sparkle).
  note(523, 80, 0.5f);
  note(659, 80, 0.52f);
  note(784, 80, 0.55f);
  note(1047, 140, 0.58f);
  silence(40);
  note(784, 60, 0.4f);
  note(1047, 60, 0.4f);
  note(1319, 160, 0.5f);
  silence(20);
}

}  // namespace audio
