// Probe v3: count edges on G42 per second while toggling EXT_5V every 5 s.
#include <M5Unified.h>
static volatile uint32_t edges = 0;
static void IRAM_ATTR isr() { edges++; }
void setup() {
  auto cfg = M5.config(); cfg.serial_baudrate = 115200;
  M5.begin(cfg);
  M5.Speaker.end();
  pinMode(42, INPUT);
  attachInterrupt(42, isr, CHANGE);
  M5.Display.setTextSize(2);
}
void loop() {
  static uint32_t sec = 0; static bool ext = false;
  if (sec % 5 == 0) {
    ext = !ext;
    bool ok = M5.Power.setExtOutput(ext);
    Serial.printf("[p3] setExtOutput(%d) -> %d, getExtOutput=%d\n", ext, ok, M5.Power.getExtOutput());
  }
  edges = 0; delay(1000);
  uint32_t e = edges;
  Serial.printf("[p3] t=%lus ext=%d level=%d edges/s=%lu\n", (unsigned long)sec, ext, digitalRead(42), (unsigned long)e);
  M5.Display.fillScreen(TFT_BLACK); M5.Display.setCursor(0,0);
  M5.Display.printf("ext %d\nedges\n%lu", ext, (unsigned long)e);
  sec++;
}
