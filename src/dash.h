#pragma once
// Landscape dashboard (240x135): big, glanceable numbers for the home screen
// and the approval prompt. Included once from main.cpp after its globals.
// Built-in fonts have no Vietnamese diacritics, so labels are short ASCII.

static M5Canvas dash(&M5.Lcd);
static bool     dashReady = false;
static uint8_t  dashRot = 1;          // 1 or 3, picked from gravity
static bool     dashShown = false;    // LCD currently in landscape for dash

static void dashInit() {
  dash.setPsram(true);
  dash.setColorDepth(16);
  dashReady = dash.createSprite(240, 135) != nullptr;
}

static void dashUpdateRot() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  if (ax > 0.45f)  dashRot = 1;       // hysteresis: hold orientation near flat
  if (ax < -0.45f) dashRot = 3;
}

static uint16_t _pctColor(uint8_t pct) {
  if (pct >= 90) return 0xF800;       // red
  if (pct >= 75) return 0xFD20;       // orange
  if (pct >= 50) return 0xFFE0;       // yellow
  return 0x07E0;                      // green
}

// One usage window: label, big %, bar, reset time.
static void _dashLimit(int y, const char* label, uint8_t pct, const char* reset, const Palette& p) {
  const int x0 = 124, x1 = 236;
  dash.setTextDatum(top_left);
  dash.setFont(&fonts::Font2);
  dash.setTextColor(p.textDim, p.bg);
  dash.drawString(label, x0, y + 2);
  dash.setTextDatum(top_right);
  dash.setFont(&fonts::FreeSansBold18pt7b);
  if (pct == 0xFF) {
    dash.setTextColor(p.textDim, p.bg);
    dash.drawString("--", x1, y - 2);
  } else {
    char b[6]; snprintf(b, sizeof(b), "%u%%", pct);
    dash.setTextColor(_pctColor(pct), p.bg);
    dash.drawString(b, x1, y - 2);
    int w = (x1 - x0) * (pct > 100 ? 100 : pct) / 100;
    dash.fillRoundRect(x0, y + 30, x1 - x0, 7, 3, 0x2104);
    if (w > 2) dash.fillRoundRect(x0, y + 30, w, 7, 3, _pctColor(pct));
  }
  if (reset && reset[0]) {
    dash.setTextDatum(top_right);
    dash.setFont(&fonts::Font2);
    dash.setTextColor(p.textDim, p.bg);
    char b[20]; snprintf(b, sizeof(b), "reset %s", reset);
    dash.drawString(b, x1, y + 39);
  }
}

static void _dashApproval(const Palette& p) {
  dash.fillSprite(p.bg);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  dash.setFont(&fonts::Font2);
  dash.setTextDatum(top_left);
  dash.setTextColor(waited >= 10 ? HOT : p.textDim, p.bg);
  char b[24]; snprintf(b, sizeof(b), "APPROVE?  %lus", (unsigned long)waited);
  dash.drawString(b, 6, 4);

  dash.setTextDatum(top_center);
  dash.setTextColor(p.text, p.bg);
  dash.setFont(strlen(tama.promptTool) <= 10 ? &fonts::FreeSansBold18pt7b : &fonts::FreeSansBold12pt7b);
  dash.drawString(tama.promptTool, 120, 22);

  // hint: two lines of Font2 (~8 px/char -> 28 chars)
  dash.setFont(&fonts::Font2);
  dash.setTextColor(p.textDim, p.bg);
  dash.setTextDatum(top_left);
  char l1[29], l2[29];
  snprintf(l1, sizeof(l1), "%.28s", tama.promptHint);
  snprintf(l2, sizeof(l2), "%.28s", strlen(tama.promptHint) > 28 ? tama.promptHint + 28 : "");
  dash.drawString(l1, 6, 60);
  dash.drawString(l2, 6, 77);

  if (responseSent) {
    dash.setTextDatum(middle_center);
    dash.drawString("sent...", 120, 116);
    return;
  }
  dash.fillRoundRect(4, 98, 112, 34, 8, 0x0400);
  dash.fillRoundRect(124, 98, 112, 34, 8, 0x8000);
  dash.setFont(&fonts::FreeSansBold12pt7b);
  dash.setTextDatum(middle_center);
  dash.setTextColor(TFT_WHITE, 0x0400); dash.drawString("A  OK", 60, 116);
  dash.setTextColor(TFT_WHITE, 0x8000); dash.drawString("B  NO", 180, 116);
}

// ---- Clawd: the Claude Code mascot, from its terminal logo ----
//    ▐▛███▜▌      quadrant block chars -> 18x5 pixel grid; terminal cells
//   ▝▜█████▛▘     are ~1:2, so each grid pixel is drawn 5 wide x 10 tall.
//     ▘▘ ▝▝
static const char* CLAWD[5] = {
  "000111111111111000",
  "000110111111011000",   // the two 0s inside are the eyes
  "011111111111111110",
  "000111111111111000",
  "000010100001010000",   // legs
};
static const uint16_t CLAWD_ORANGE = 0xD3AA;   // #D77757
static const int CPX = 5, CPY = 10;

static void _clawd(int cx, int baseY, bool eyesShut, bool legsUp, uint16_t col, uint16_t bg) {
  int x0 = cx - 9 * CPX;
  int rows = legsUp ? 4 : 5;
  int y0 = baseY - rows * CPY;
  for (int r = 0; r < rows; r++)
    for (int c = 0; c < 18; c++) {
      char v = CLAWD[r][c];
      bool eye = (r == 1 && v == '0' && c > 3 && c < 14);
      if (v == '1' || (eye && eyesShut))
        dash.fillRect(x0 + c * CPX, y0 + r * CPY, CPX, CPY, col);
      else if (eye)
        dash.fillRect(x0 + c * CPX, y0 + r * CPY, CPX, CPY, bg);
    }
}

// Few, bouncy moods. Left panel is x 0..117, y 0..80.
static void _dashPet(const Palette& p) {
  uint32_t t = millis();
  int cx = 58, base = 76, dx = 0, dy = 0;
  bool shut = false, legsUp = false;
  dash.setFont(&fonts::FreeSansBold12pt7b);
  dash.setTextDatum(middle_center);
  switch (activeState) {
    case P_SLEEP: {                                   // sit low, eyes shut, z's drift up
      shut = true; dy = 4;
      int z = (t / 120) % 30;
      dash.setTextColor(0x8410, p.bg);
      dash.drawString("z", cx + 40 + z / 6, 20 - z / 2);
      break;
    }
    case P_BUSY: {                                    // hop hop hop
      float ph = (t % 520) / 520.0f;
      dy = -(int)(22 * sinf(ph * 3.14159f));
      legsUp = dy < -6;
      break;
    }
    case P_ATTENTION: {                               // wiggle + "!"
      dx = ((t / 90) % 2) ? 4 : -4;
      dash.setTextColor(0xF800, p.bg);
      dash.drawString("!", cx + 52, 16);
      break;
    }
    case P_CELEBRATE: {                               // big jumps + sparkles
      float ph = (t % 700) / 700.0f;
      dy = -(int)(30 * sinf(ph * 3.14159f));
      legsUp = dy < -6;
      for (int i = 0; i < 5; i++) {
        int sx = (int)(cx + 50 * cosf(t / 300.0f + i * 1.26f));
        int sy = (int)(30 + 20 * sinf(t / 300.0f + i * 1.26f));
        dash.fillRect(sx, sy, 3, 3, 0xFFE0);
      }
      break;
    }
    default: {                                        // idle: gentle bob + blink
      dy = ((t / 400) % 2) ? -3 : 0;
      shut = (t % 3200) < 140;
      break;
    }
  }
  _clawd(cx + dx, base + dy, shut, legsUp, CLAWD_ORANGE, p.bg);
}

static void _dashHome(const Palette& p) {
  dash.fillSprite(p.bg);
  _dashPet(p);

  // status word under the pet
  static const char* WORD[] = {"zzz", "idle", "BUSY", "WAIT", "yay!", "@_@", "<3"};
  static const uint16_t COL[] = {0x8410, 0x07FF, 0xFFE0, 0xF800, 0x07E0, 0xA01F, 0xF810};
  uint8_t st = activeState < 7 ? activeState : 1;
  dash.setTextDatum(top_center);
  dash.setFont(&fonts::FreeSansBold12pt7b);
  dash.setTextColor(COL[st], p.bg);
  dash.drawString(WORD[st], 58, 82);

  // session count + link
  dash.setFont(&fonts::Font2);
  dash.setTextColor(p.textDim, p.bg);
  char b[24];
  if (tama.connected) snprintf(b, sizeof(b), "%u run / %u  %s", tama.sessionsRunning, tama.sessionsTotal, dataScenarioName());
  else snprintf(b, sizeof(b), "no Claude");
  dash.drawString(b, 58, 104);

  dash.drawFastVLine(118, 4, 104, 0x2104);
  _dashLimit(4,  "5h", tama.lim5Pct, tama.lim5Reset, p);
  _dashLimit(58, "7d", tama.lim7Pct, tama.lim7Reset, p);

  // bottom strip: newest transcript line, else msg
  dash.fillRect(0, 120, 240, 15, 0x10A2);
  dash.setTextDatum(top_left);
  dash.setFont(&fonts::Font2);
  dash.setTextColor(p.text, 0x10A2);
  const char* line = tama.nLines ? tama.lines[0] : tama.msg;
  char ln[31]; snprintf(ln, sizeof(ln), "%.30s", line);
  dash.drawString(ln, 3, 120);
}

// Returns true when it drew (caller then skips the portrait sprite path).
static bool dashTick() {
  if (!dashReady) return false;
  static uint32_t last = 0;
  if (millis() - last < 66) return true;          // ~15 fps
  last = millis();
  dashUpdateRot();
  const Palette& p = characterPalette();
  if (tama.promptId[0]) _dashApproval(p); else _dashHome(p);
  M5.Lcd.setRotation(dashRot);
  dash.pushSprite(0, 0);
  dashShown = true;
  return true;
}

// Call when leaving the dashboard for a portrait screen.
static void dashLeave() {
  if (!dashShown) return;
  dashShown = false;
  M5.Lcd.setRotation(0);
  M5.Lcd.fillScreen(TFT_BLACK);
  applyDisplayMode();
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}
