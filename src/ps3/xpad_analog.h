#ifndef XPAD_ANALOG_H
#define XPAD_ANALOG_H

/*
 * Shared DualShock-3-style stick curve used by both halves of the plugin.
 *
 * The VSH driver applies it to controllers that it owns immediately before
 * cellPadLddDataInsert().  The game SPRX applies the same transform only to a
 * selected, non-conforming native pad (normally a DS4/DualSense over BT).
 * Keeping the arithmetic here makes the two paths bit-for-bit identical.
 */

#include <stdint.h>
#include <string.h>
#include <cell/pad.h>

#define XPAD_ANALOG_MODE_OFF   0
#define XPAD_ANALOG_MODE_AUTO  1
#define XPAD_ANALOG_MODE_FORCE 2

#define XPAD_ANALOG_AXIS_LX 0
#define XPAD_ANALOG_AXIS_LY 1
#define XPAD_ANALOG_AXIS_RX 2
#define XPAD_ANALOG_AXIS_RY 3
#define XPAD_ANALOG_AXIS_COUNT 4

#define XPAD_ANALOG_UNIT 1024

typedef struct xpad_analog_config {
  uint8_t enabled;
  uint8_t saturation;
  uint8_t deadzone;
  uint8_t game_mode;
  uint8_t port_mask;
  uint8_t spoof_pad_info;
  uint8_t minimum[XPAD_ANALOG_AXIS_COUNT];
  uint8_t center[XPAD_ANALOG_AXIS_COUNT];
  uint8_t maximum[XPAD_ANALOG_AXIS_COUNT];
} XPAD_ANALOG_CONFIG_t;

static void xpad_analog_defaults(XPAD_ANALOG_CONFIG_t *cfg) {
  int32_t i;

  memset(cfg, 0, sizeof(*cfg));
  cfg->enabled = 1;
  cfg->saturation = 80;
  cfg->deadzone = 4;
  cfg->game_mode = XPAD_ANALOG_MODE_AUTO;
  cfg->port_mask = 0x01; /* port 1 in the user interface / index 0 in libpad */
  cfg->spoof_pad_info = 1;
  for (i = 0; i < XPAD_ANALOG_AXIS_COUNT; i++) {
    /* Full range is a deliberately safe default.  It cannot make a normal
       DS4/DS5 overly sensitive, while the 80% curve still lets a short Hall
       stick reach the rails.  Per-axis values remain configurable. */
    cfg->minimum[i] = 0;
    cfg->center[i] = 128;
    cfg->maximum[i] = 255;
  }
}

static char *xpad_analog_trim(char *s) {
  char *end;

  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '"') s++;
  end = s + strlen(s);
  while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                     end[-1] == '\r' || end[-1] == '"')) {
    *--end = 0;
  }
  return(s);
}

static int32_t xpad_analog_parse_number(const char *text, int32_t *out) {
  int32_t base = 10, value = 0, digits = 0, d;

  while (*text == ' ' || *text == '\t' || *text == '\r') text++;
  if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    base = 16;
    text += 2;
  }
  while (*text) {
    if (*text >= '0' && *text <= '9') d = *text - '0';
    else if (base == 16 && *text >= 'a' && *text <= 'f') d = *text - 'a' + 10;
    else if (base == 16 && *text >= 'A' && *text <= 'F') d = *text - 'A' + 10;
    else break;
    if (d >= base) break;
    value = value * base + d;
    digits++;
    text++;
  }
  while (*text == ' ' || *text == '\t' || *text == '\r') text++;
  if (!digits || *text) return(-1);
  *out = value;
  return(0);
}

static int32_t xpad_analog_axis_key(const char *key, const char *suffix) {
  static const char *prefix[XPAD_ANALOG_AXIS_COUNT] = {
    "ANALOG_LX_", "ANALOG_LY_", "ANALOG_RX_", "ANALOG_RY_"
  };
  int32_t i;

  for (i = 0; i < XPAD_ANALOG_AXIS_COUNT; i++) {
    char full[24];
    strcpy(full, prefix[i]);
    strcat(full, suffix);
    if (strcasecmp(key, full) == 0) return(i);
  }
  return(-1);
}

/* Parse the same xpad_analog.txt in VSH and game processes. Unknown keys are
   ignored so later versions can extend the file without breaking old builds. */
static void xpad_analog_parse_buffer(XPAD_ANALOG_CONFIG_t *cfg, char *buffer) {
  char *line = buffer, *next, *comment, *equals, *key, *value;
  int32_t n, axis, i;

  while (line != NULL && *line != 0) {
    if ((next = strchr(line, '\n')) != NULL) *next++ = 0;
    if ((comment = strchr(line, '#')) != NULL) *comment = 0;
    key = xpad_analog_trim(line);
    if ((equals = strchr(key, '=')) != NULL) {
      *equals = 0;
      value = xpad_analog_trim(equals + 1);
      key = xpad_analog_trim(key);
      if (strcasecmp(key, "ANALOG_ENABLED") == 0 &&
          xpad_analog_parse_number(value, &n) == 0 && (n == 0 || n == 1)) {
        cfg->enabled = (uint8_t)n;
      } else if (strcasecmp(key, "ANALOG_SATURATION") == 0 &&
                 xpad_analog_parse_number(value, &n) == 0 && n >= 50 && n <= 100) {
        cfg->saturation = (uint8_t)n;
      } else if (strcasecmp(key, "ANALOG_DEADZONE") == 0 &&
                 xpad_analog_parse_number(value, &n) == 0 && n >= 0 && n <= 30) {
        cfg->deadzone = (uint8_t)n;
      } else if (strcasecmp(key, "ANALOG_PORT_MASK") == 0 &&
                 xpad_analog_parse_number(value, &n) == 0 && n >= 0 && n <= 0x7f) {
        cfg->port_mask = (uint8_t)n;
      } else if (strcasecmp(key, "ANALOG_SPOOF_PAD_INFO") == 0 &&
                 xpad_analog_parse_number(value, &n) == 0 && (n == 0 || n == 1)) {
        cfg->spoof_pad_info = (uint8_t)n;
      } else if (strcasecmp(key, "ANALOG_GAME_MODE") == 0) {
        if (strcasecmp(value, "OFF") == 0) cfg->game_mode = XPAD_ANALOG_MODE_OFF;
        else if (strcasecmp(value, "FORCE") == 0) cfg->game_mode = XPAD_ANALOG_MODE_FORCE;
        else if (strcasecmp(value, "AUTO") == 0) cfg->game_mode = XPAD_ANALOG_MODE_AUTO;
      } else if ((axis = xpad_analog_axis_key(key, "MIN")) >= 0 &&
                 xpad_analog_parse_number(value, &n) == 0 && n >= 0 && n <= 255) {
        cfg->minimum[axis] = (uint8_t)n;
      } else if ((axis = xpad_analog_axis_key(key, "CENTER")) >= 0 &&
                 xpad_analog_parse_number(value, &n) == 0 && n >= 0 && n <= 255) {
        cfg->center[axis] = (uint8_t)n;
      } else if ((axis = xpad_analog_axis_key(key, "MAX")) >= 0 &&
                 xpad_analog_parse_number(value, &n) == 0 && n >= 0 && n <= 255) {
        cfg->maximum[axis] = (uint8_t)n;
      }
    }
    line = next;
  }

  /* Reject bad manual ranges as a unit. Partial/broken values must never
     create an axis with near-zero span. */
  for (i = 0; i < XPAD_ANALOG_AXIS_COUNT; i++) {
    if ((int32_t)cfg->center[i] - (int32_t)cfg->minimum[i] < 16 ||
        (int32_t)cfg->maximum[i] - (int32_t)cfg->center[i] < 16) {
      cfg->minimum[i] = 0;
      cfg->center[i] = 128;
      cfg->maximum[i] = 255;
    }
  }
}

static int32_t xpad_analog_clamp(int32_t v, int32_t lo, int32_t hi) {
  if (v < lo) return(lo);
  if (v > hi) return(hi);
  return(v);
}

static uint32_t xpad_analog_isqrt(uint32_t n) {
  uint32_t rem = 0, root = 0;
  int32_t i;

  for (i = 0; i < 16; i++) {
    root <<= 1;
    rem = (rem << 2) | (n >> 30);
    n <<= 2;
    if (root < rem) {
      root++;
      rem -= root;
      root++;
    }
  }
  return(root >> 1);
}

static int32_t xpad_analog_normalize(uint8_t raw, uint8_t minimum,
                                     uint8_t center, uint8_t maximum) {
  int32_t v = (int32_t)raw - (int32_t)center;
  int32_t span;

  if (v >= 0) {
    span = (int32_t)maximum - (int32_t)center;
  } else {
    span = (int32_t)center - (int32_t)minimum;
  }
  if (span < 16) {
    /* Reject a malformed/degenerate calibration instead of turning the axis
       into a switch.  The fallback is the standard 0/128/255 range. */
    span = v >= 0 ? 127 : 128;
  }
  return(xpad_analog_clamp((v * XPAD_ANALOG_UNIT) / span,
                           -XPAD_ANALOG_UNIT, XPAD_ANALOG_UNIT));
}

static void xpad_analog_deadzone(int32_t *x, int32_t *y, int32_t dz) {
  uint32_t mag;
  int32_t scaled;

  if (dz <= 0) return;
  mag = xpad_analog_isqrt((uint32_t)((*x) * (*x) + (*y) * (*y)));
  if ((int32_t)mag <= dz) {
    *x = 0;
    *y = 0;
    return;
  }
  scaled = (int32_t)(((mag - (uint32_t)dz) * XPAD_ANALOG_UNIT) /
                     (uint32_t)(XPAD_ANALOG_UNIT - dz));
  *x = (int32_t)(((int64_t)*x * scaled) / (int64_t)mag);
  *y = (int32_t)(((int64_t)*y * scaled) / (int64_t)mag);
}

static uint8_t xpad_analog_to_byte(int32_t v) {
  int32_t out;

  if (v < 0) out = 128 + (v * 128) / XPAD_ANALOG_UNIT;
  else out = 128 + (v * 127) / XPAD_ANALOG_UNIT;
  return((uint8_t)xpad_analog_clamp(out, 0, 255));
}

static void xpad_analog_one_stick(const XPAD_ANALOG_CONFIG_t *cfg,
                                  int32_t ax, int32_t ay,
                                  uint8_t raw_x, uint8_t raw_y,
                                  uint8_t *out_x, uint8_t *out_y) {
  int32_t x, y, dz, sat;

  x = xpad_analog_normalize(raw_x, cfg->minimum[ax], cfg->center[ax],
                            cfg->maximum[ax]);
  y = xpad_analog_normalize(raw_y, cfg->minimum[ay], cfg->center[ay],
                            cfg->maximum[ay]);

  dz = (XPAD_ANALOG_UNIT * (int32_t)cfg->deadzone) / 100;
  xpad_analog_deadzone(&x, &y, dz);

  /* Saturation is per axis, just like a real DualShock 3.  Radial scaling
     would shrink diagonals and recreate the exact problem being fixed. */
  sat = cfg->saturation;
  if (sat >= 50 && sat < 100) {
    x = xpad_analog_clamp((x * 100) / sat,
                          -XPAD_ANALOG_UNIT, XPAD_ANALOG_UNIT);
    y = xpad_analog_clamp((y * 100) / sat,
                          -XPAD_ANALOG_UNIT, XPAD_ANALOG_UNIT);
  }
  *out_x = xpad_analog_to_byte(x);
  *out_y = xpad_analog_to_byte(y);
}

static void xpad_analog_apply(const XPAD_ANALOG_CONFIG_t *cfg,
                              CellPadData *data) {
  uint8_t lx, ly, rx, ry;

  if (cfg == NULL || !cfg->enabled || data == NULL ||
      data->len <= CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y) {
    return;
  }
  lx = (uint8_t)data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X];
  ly = (uint8_t)data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y];
  rx = (uint8_t)data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X];
  ry = (uint8_t)data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y];

  /* An all-zero buffer is how some games expose an unpopulated read, not a
     physically possible resting report.  Leave it untouched. */
  if (!lx && !ly && !rx && !ry) return;

  xpad_analog_one_stick(cfg, XPAD_ANALOG_AXIS_LX, XPAD_ANALOG_AXIS_LY,
                        lx, ly, &lx, &ly);
  xpad_analog_one_stick(cfg, XPAD_ANALOG_AXIS_RX, XPAD_ANALOG_AXIS_RY,
                        rx, ry, &rx, &ry);
  data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X] = lx;
  data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y] = ly;
  data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] = rx;
  data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] = ry;
}

#endif /* XPAD_ANALOG_H */
