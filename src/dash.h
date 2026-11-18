#pragma once
// Big-type home screens, picked by how the stick is held:
//   lying / sideways  -> landscape dashboard 240x135 (Clawd + usage limits)
//   standing upright  -> portrait list 135x240 (Clawd + recent commands)
//   face down         -> do-not-disturb (handled in main.cpp: napping)
// Included once from main.cpp after its globals. Built-in fonts have no
// Vietnamese diacritics, so on-screen labels are short ASCII.

static M5Canvas dash(&M5.Lcd);
static bool     dashReady = false;
static uint8_t  dashRot = 1;          // LCD rotation in use: 1/3 landscape, 0/2 portrait
static bool     dashShown = false;
static bool     dashPortrait = false;

// Gravity thresholds (g). Calibrated on the StickS3 with the `imu` command;
// see TASKS.md. Upright = gravity along the long (Y) axis.
static const float UPRIGHT_G = 0.75f, SIDE_G = 0.45f;

static void dashInit() {
  dash.setPsram(true);
  dash.setColorDepth(16);
  dashReady = dash.createSprite(240, 135) != nullptr;
}

static void dashUpdateRot() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  static int8_t upFrames = 0;             // debounce portrait <-> landscape
  bool upright = fabsf(ay) > UPRIGHT_G && fabsf(ay) > fabsf(ax);
  if (upright) { if (upFrames < 8) upFrames++; } else { if (upFrames > -8) upFrames--; }
  if (!dashPortrait && upFrames >= 6)  dashPortrait = true;
  if (dashPortrait && upFrames <= -6)  dashPortrait = false;
  if (dashPortrait) {
    if (ay > UPRIGHT_G)  dashRot = 0;     // which way up: flip to stay readable
    if (ay < -UPRIGHT_G) dashRot = 2;     // (verified on StickS3 by W, 2026-11-18)
  } else {
    if (ax > SIDE_G)  dashRot = 1;
    if (ax < -SIDE_G) dashRot = 3;
    if (dashRot != 1 && dashRot != 3) dashRot = 1;
  }
}

static uint16_t _pctColor(uint8_t pct) {
  if (pct >= 90) return 0xF800;
  if (pct >= 75) return 0xFD20;
  if (pct >= 50) return 0xFFE0;
  return 0x07E0;
}

// ---- Clawd: the Claude Code mascot, from its terminal logo ----
//    ▐▛███▜▌      quadrant block chars -> 18x5 pixel grid; terminal cells
//   ▝▜█████▛▘     are ~1:2, so a grid pixel is drawn twice as tall as wide.
//     ▘▘ ▝▝
static const char* CLAWD[5] = {
  "000111111111111000",
  "000110111111011000",   // the two 0s inside are the eyes
  "011111111111111110",
  "000111111111111000",
  "000010100001010000",   // legs
};
static const uint16_t CLAWD_ORANGE = 0xD3AA;   // #D77757

static void _clawd(LovyanGFX& g, int cx, int baseY, int px, bool eyesShut, bool legsUp,
                   int shear, uint16_t col, uint16_t bg) {
  int py = px * 2;
  int rows = legsUp ? 4 : 5;
  int y0 = baseY - rows * py;
  for (int r = 0; r < rows; r++) {
    int x0 = cx - 9 * px + shear * (2 - r);           // shear: dizzy wobble
    for (int c = 0; c < 18; c++) {
      char v = CLAWD[r][c];
      bool eye = (r == 1 && v == '0' && c > 3 && c < 14);
      if (v == '1' || (eye && eyesShut)) g.fillRect(x0 + c * px, y0 + r * py, px, py, col);
      else if (eye)                      g.fillRect(x0 + c * px, y0 + r * py, px, py, bg);
    }
  }
}

// Few, bouncy moods. Draws Clawd with feet at (cx, base), pixel size px.
static void _pet(LovyanGFX& g, int cx, int base, int px, const Palette& p) {
  uint32_t t = millis();
  int dx = 0, dy = 0, shear = 0, k = px;              // k scales motion with size
  bool shut = false, legsUp = false;
  if (px >= 5) g.setFont(&fonts::FreeSansBold12pt7b); else g.setFont(&fonts::Font2);
  g.setTextDatum(middle_center);
  switch (activeState) {
    case P_SLEEP: {
      shut = true; dy = k;
      int z = (t / 120) % 30;
      g.setTextColor(0x8410, p.bg);
      g.drawString("z", cx + 8 * k + z / 6, base - 11 * k - z / 2);
      break;
    }
    case P_BUSY: {                                     // hop hop hop
      float ph = (t % 520) / 520.0f;
      dy = -(int)(4.4f * k * sinf(ph * 3.14159f));
      legsUp = dy < -k;
      break;
    }
    case P_ATTENTION: {                                // wiggle + "!"
      dx = ((t / 90) % 2) ? k : -k;
      g.setTextColor(0xF800, p.bg);
      g.drawString("!", cx + 10 * k, base - 12 * k);
      break;
    }
    case P_CELEBRATE: {                                // big jumps + sparkles
      float ph = (t % 700) / 700.0f;
      dy = -(int)(6 * k * sinf(ph * 3.14159f));
      legsUp = dy < -k;
      for (int i = 0; i < 5; i++) {
        int sx = (int)(cx + 10 * k * cosf(t / 300.0f + i * 1.26f));
        int sy = (int)(base - 9 * k + 4 * k * sinf(t / 300.0f + i * 1.26f));
        g.fillRect(sx, sy, 3, 3, 0xFFE0);
      }
      break;
    }
    case P_DIZZY: {                                    // shaken: wobble + stars
      shear = (int)(k * 0.8f * sinf(t / 70.0f));
      shut = true;
      for (int i = 0; i < 3; i++) {
        float a = t / 160.0f + i * 2.09f;
        g.fillRect(cx + (int)(7 * k * cosf(a)), base - 12 * k + (int)(2 * k * sinf(a)), 3, 3, 0xFFE0);
      }
      break;
    }
    default: {                                         // idle: bob + blink
      dy = ((t / 400) % 2) ? -k / 2 - 1 : 0;
      shut = (t % 3200) < 140;
      break;
    }
  }
  _clawd(g, cx + dx, base + dy, px, shut, legsUp, shear, CLAWD_ORANGE, p.bg);
}

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

static const char* _stateWord(uint8_t st, uint16_t* col) {
  static const char* WORD[] = {"zzz", "idle", "BUSY", "WAIT", "yay!", "@_@", "<3"};
  static const uint16_t COL[] = {0x8410, 0x07FF, 0xFFE0, 0xF800, 0x07E0, 0xA01F, 0xF810};
  if (st >= 7) st = 1;
  *col = COL[st];
  return WORD[st];
}

// Battery icon 22x10 (+2px tip) at (x,y). Fill colour: green >50, yellow >20,
// red below; cyan when charging (voltage-% reads high while charging).
static void _batt(LovyanGFX& g, int x, int y, const Palette& p) {
  const PwrSample& s = power();
  g.drawRect(x, y, 22, 10, p.textDim);
  g.fillRect(x + 22, y + 3, 2, 4, p.textDim);
  if (s.pct >= 0) {
    uint16_t c = s.chg ? 0x07FF : s.pct > 50 ? 0x07E0 : s.pct > 20 ? 0xFFE0 : 0xF800;
    int w = (18 * s.pct + 50) / 100;
    if (w > 0) g.fillRect(x + 2, y + 2, w, 6, c);
  }
}

// "87%" (or "--%") right-aligned at rx, vertically centred on cy, Font0.
static void _battPct(LovyanGFX& g, int rx, int cy, const Palette& p) {
  const PwrSample& s = power();
  char b[8];
  if (s.pct >= 0) snprintf(b, sizeof(b), "%s%d%%", s.chg ? "+" : "", s.pct);
  else snprintf(b, sizeof(b), "--%%");
  g.setFont(&fonts::Font0); g.setTextSize(1);
  g.setTextDatum(middle_right);
  g.setTextColor(p.textDim, p.bg);
  g.drawString(b, rx, cy);
}

static void _dashHome(const Palette& p) {
  dash.fillSprite(p.bg);
  _batt(dash, 4, 4, p);
  _battPct(dash, 56, 9, p);
  _pet(dash, 58, 76, 5, p);

  uint16_t c; const char* w = _stateWord(activeState, &c);
  dash.setTextDatum(top_center);
  dash.setFont(&fonts::FreeSansBold12pt7b);
  dash.setTextColor(c, p.bg);
  dash.drawString(w, 58, 82);

  dash.setFont(&fonts::Font2);
  dash.setTextColor(p.textDim, p.bg);
  char b[24];
  if (tama.connected) snprintf(b, sizeof(b), "%u run / %u  %s", tama.sessionsRunning, tama.sessionsTotal, dataScenarioName());
  else snprintf(b, sizeof(b), "no Claude");
  dash.drawString(b, 58, 104);

  dash.drawFastVLine(118, 4, 104, 0x2104);
  _dashLimit(4,  "5h", tama.lim5Pct, tama.lim5Reset, p);
  _dashLimit(58, "7d", tama.lim7Pct, tama.lim7Reset, p);

  dash.fillRect(0, 120, 240, 15, 0x10A2);
  dash.setTextDatum(top_left);
  dash.setFont(&fonts::Font2);
  dash.setTextColor(p.text, 0x10A2);
  const char* line = tama.nLines ? tama.lines[0] : tama.msg;
  char ln[31]; snprintf(ln, sizeof(ln), "%.30s", line);
  dash.drawString(ln, 3, 120);
}

// SoftAP setup card (landscape): what to join, where to browse, the PIN.
static void _dashAp(const Palette& p) {
  dash.fillSprite(p.bg);
  dash.setTextDatum(top_left);
  dash.setFont(&fonts::FreeSansBold9pt7b);
  dash.setTextColor(CLAWD_ORANGE, p.bg);
  dash.drawString("WIFI SETUP", 4, 2);
  char b[40];
  snprintf(b, sizeof(b), "%lus", (unsigned long)wifiApLeftSec());
  dash.setTextDatum(top_right); dash.setTextColor(p.textDim, p.bg);
  dash.drawString(b, 236, 2);
  dash.setTextDatum(top_left);
  dash.setFont(&fonts::Font2);
  int y = 26;
  const char* L[][2] = {{"wifi", wifiApSsid()}, {"pass", wifiApPass()},
                        {"open", wifiIpStr()}, {"user", "buddy"}, {"PIN", webPin()}};
  for (auto& r : L) {
    dash.setTextColor(p.textDim, p.bg); dash.drawString(r[0], 4, y);
    dash.setTextColor(p.text, p.bg);    dash.drawString(r[1], 48, y);
    y += 18;
  }
  dash.setTextColor(p.textDim, p.bg);
  dash.drawString("2.4 GHz only", 4, 118);
}

// Portrait: small Clawd, then recent commands in readable type.
static void _dashTall(const Palette& p) {
  spr.fillSprite(p.bg);
  _battPct(spr, 104, 7, p);
  _batt(spr, 107, 2, p);
  _pet(spr, 67, 44, 3, p);
  uint16_t c; const char* w = _stateWord(activeState, &c);
  spr.setFont(&fonts::FreeSansBold9pt7b);
  spr.setTextDatum(top_center);
  spr.setTextColor(c, p.bg);
  spr.drawString(w, 67, 48);

  spr.setFont(&fonts::Font2);
  spr.setTextDatum(top_left);
  int y = 72;
  if (tama.promptId[0]) {
    spr.setTextColor(HOT, p.bg);
    spr.drawString("APPROVE? lay ngang", 4, y);   // approval UI lives in landscape
    y += 18;
  }
  uint8_t n = tama.nLines;
  if (n == 0) {
    spr.setTextColor(p.textDim, p.bg);
    spr.drawString(tama.connected ? tama.msg : "no Claude", 4, y);
  }
  for (uint8_t i = 0; i < n && y < 200; i++) {
    const char* s = tama.lines[i];
    if (strlen(s) > 6 && s[2] == ':' && s[5] == ' ') s += 6;   // drop "HH:MM "
    char a[17], b2[17];
    snprintf(a, sizeof(a), "%.16s", s);
    snprintf(b2, sizeof(b2), "%.16s", strlen(s) > 16 ? s + 16 : "");
    spr.setTextColor(i == 0 ? p.text : p.textDim, p.bg);
    spr.drawString(a, 4, y); y += 16;
    if (b2[0] && i == 0) { spr.drawString(b2, 4, y); y += 16; }
    y += 3;
  }

  spr.fillRect(0, 214, 135, 26, 0x10A2);
  spr.setFont(&fonts::FreeSansBold9pt7b);
  spr.setTextDatum(middle_left);
  char b[10];
  if (tama.lim5Pct != 0xFF) {
    snprintf(b, sizeof(b), "5h %u%%", tama.lim5Pct);
    spr.setTextColor(_pctColor(tama.lim5Pct), 0x10A2); spr.drawString(b, 4, 227);
  }
  if (tama.lim7Pct != 0xFF) {
    snprintf(b, sizeof(b), "7d %u%%", tama.lim7Pct);
    spr.setTextDatum(middle_right);
    spr.setTextColor(_pctColor(tama.lim7Pct), 0x10A2); spr.drawString(b, 131, 227);
  }
  spr.setFont(&fonts::Font0); spr.setTextSize(1);       // restore default for legacy screens
}

// Returns true when it drew (caller then skips the legacy portrait path).
static bool dashTick() {
  if (!dashReady) return false;
  static uint32_t last = 0;
  if (millis() - last < 66) return true;          // ~15 fps
  last = millis();
  dashUpdateRot();
  const Palette& p = characterPalette();
  M5.Lcd.setRotation(dashRot);
  if (wifiApActive() && !tama.promptId[0]) {
    if (dashRot != 1 && dashRot != 3) { dashRot = 1; }
    M5.Lcd.setRotation(dashRot);
    _dashAp(p);
    dash.pushSprite(0, 0);
  } else if (dashPortrait && !tama.promptId[0]) {
    _dashTall(p);
    spr.pushSprite(0, 0);
  } else {
    if (dashPortrait) { dashRot = 1; M5.Lcd.setRotation(1); }   // approve in landscape
    if (tama.promptId[0]) _dashApproval(p); else _dashHome(p);
    dash.pushSprite(0, 0);
  }
  dashShown = true;
  return true;
}

static void dashLeave() {
  if (!dashShown) return;
  dashShown = false;
  M5.Lcd.setRotation(0);
  M5.Lcd.fillScreen(TFT_BLACK);
  spr.setFont(&fonts::Font0); spr.setTextSize(1); spr.setTextDatum(TL_DATUM);
  applyDisplayMode();
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}
