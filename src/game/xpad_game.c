/*
 * xpad_game.sprx - game-process companion for PS3xPAD v4.0.5.
 *
 * One module now provides both jobs that require visibility inside a game:
 *   1. forward cellPadSetActDirect rumble to the VSH USB driver;
 *   2. correct native DS4/DualSense Bluetooth stick values and selectively
 *      report those generic pads as a conforming standard controller.
 *
 * USB/Direwolf input is already corrected by the VSH half. AUTO mode sees
 * that virtual pad as LDD/conforming and skips it, preventing a double curve.
 * A genuine DualShock 3 is skipped for the same reason. FORCE remains an
 * explicit escape hatch for unusual firmware/adapters.
 */

#include <stdint.h>
#include <string.h>
#include <sys/prx.h>
#include <sys/process.h>
#include <sys/event.h>
#include <sys/synchronization.h>
#include <cell/pad.h>
#include <cell/cell_fs.h>
#include "../src/xpad_analog.h"
#include "../src/xpad_rumble.h"

#define XPAD_ANALOG_PATH "/dev_hdd0/plugins/ps3xpad/xpad_analog.txt"
#define XPAD_GAME_LOG_PATH "/dev_hdd0/plugins/ps3xpad/xpad_game.log"

#define RUMBLE_MAX_SEND_FAILS 4
#define OPD_ADDR_MIN 0x00010000u
#define OPD_ADDR_MAX 0x10000000u

#define SPOOF_CAPABILITY (CELL_PAD_CAPABILITY_PS3_CONFORMITY | \
                          CELL_PAD_CAPABILITY_PRESS_MODE | \
                          CELL_PAD_CAPABILITY_SENSOR_MODE | \
                          CELL_PAD_CAPABILITY_HP_ANALOG_STICK | \
                          CELL_PAD_CAPABILITY_ACTUATOR)

SYS_MODULE_INFO(xpad_game, 0, 1, 0);
SYS_MODULE_START(xpad_game_start);
SYS_MODULE_STOP(xpad_game_stop);

int xpad_game_start(uint64_t arg);
int xpad_game_stop(void);

/* SDK 4.75 declares the syscall but omits this convenience wrapper. */
static inline int32_t xpad_event_port_connect_ipc(sys_event_port_t event_port_id,
                                                   sys_ipc_key_t event_queue_key) {
  system_call_2(SYS_EVENT_PORT_CONNECT_IPC, event_port_id, event_queue_key);
  return_to_user_prog(int32_t);
}

/* -------------------------------------------------------------------------
 * Tiny runtime log. It is intentionally append-only and best effort: a log
 * failure must never affect input or game startup.
 * ------------------------------------------------------------------------- */

static void game_log(const char *text, int32_t truncate) {
  int32_t fd;
  uint64_t written;
  int32_t flags = CELL_FS_O_WRONLY | CELL_FS_O_CREAT;

  flags |= truncate ? CELL_FS_O_TRUNC : CELL_FS_O_APPEND;
  if (cellFsOpen(XPAD_GAME_LOG_PATH, flags, &fd, NULL, 0) != CELL_FS_SUCCEEDED) return;
  cellFsWrite(fd, text, (uint64_t)strlen(text), &written);
  cellFsWrite(fd, "\n", 1, &written);
  cellFsClose(fd);
}

static void game_log_code(const char *text, int32_t code) {
  static const char hex[] = "0123456789ABCDEF";
  char line[160];
  uint32_t value = (uint32_t)code;
  uint32_t i, n = 0;

  while (text[n] != 0 && n < sizeof(line) - 14) {
    line[n] = text[n];
    n++;
  }
  line[n++] = ' ';
  line[n++] = '(';
  line[n++] = '0';
  line[n++] = 'x';
  for (i = 0; i < 8; i++) {
    line[n++] = hex[(value >> (28 - i * 4)) & 0x0f];
  }
  line[n++] = ')';
  line[n] = 0;
  game_log(line, 0);
}

static void write_ready_marker(int32_t installed_hooks) {
  XPAD_GAME_READY_FILE_t ready;
  int32_t fd, r;
  uint64_t written = 0;

  ready.magic = XPAD_GAME_READY_MAGIC;
  ready.hook_count = (uint32_t)installed_hooks;
  ready.process_id = (uint32_t)sys_process_getpid();
  r = cellFsOpen(XPAD_GAME_READY_PATH,
                 CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
                 &fd, NULL, 0);
  if (r != CELL_FS_SUCCEEDED) {
    game_log_code("ready marker: open failed", r);
    return;
  }
  r = cellFsWrite(fd, &ready, sizeof(ready), &written);
  cellFsClose(fd);
  if (r != CELL_FS_SUCCEEDED || written != sizeof(ready)) {
    game_log_code("ready marker: write failed", r);
  } else {
    game_log("ready marker: written for VSH toast", 0);
  }
}

/* -------------------------------------------------------------------------
 * Shared-OPD detours. On PS3 ELFv1 a function pointer addresses the pair
 * {entry, TOC}. System-library importers share that pair, so replacing both
 * words redirects every caller in the process without patching executable
 * instructions or relying on firmware-specific prologues.
 * ------------------------------------------------------------------------- */

typedef struct xpad_opd_detour {
  uint32_t *opd;
  uint32_t saved[2];
} XPAD_OPD_DETOUR_t;

static int32_t opd_plausible(uint32_t addr) {
  return(addr >= OPD_ADDR_MIN && addr < OPD_ADDR_MAX);
}

static int32_t opd_install(void *target, void *replacement,
                           XPAD_OPD_DETOUR_t *detour) {
  uint32_t *opd, *rep;

  if (target == NULL || replacement == NULL || detour == NULL) return(-1);
  opd = (uint32_t *)target;
  rep = (uint32_t *)replacement;
  if (!opd_plausible((uint32_t)(uintptr_t)opd) ||
      !opd_plausible((uint32_t)(uintptr_t)rep)) return(-1);

  detour->opd = opd;
  detour->saved[0] = opd[0];
  detour->saved[1] = opd[1];
  if ((((uintptr_t)opd | (uintptr_t)rep) & 7) == 0) {
    *(volatile uint64_t *)opd = *(const uint64_t *)rep;
  } else {
    opd[0] = rep[0];
    opd[1] = rep[1];
  }
  __asm__ __volatile__("sync" ::: "memory");
  return(0);
}

static void opd_remove(XPAD_OPD_DETOUR_t *detour) {
  if (detour == NULL || detour->opd == NULL) return;
  if (((uintptr_t)detour->opd & 7) == 0) {
    uint64_t saved;
    memcpy(&saved, detour->saved, sizeof(saved));
    *(volatile uint64_t *)detour->opd = saved;
  } else {
    detour->opd[0] = detour->saved[0];
    detour->opd[1] = detour->saved[1];
  }
  __asm__ __volatile__("sync" ::: "memory");
  detour->opd = NULL;
}

/* -------------------------------------------------------------------------
 * Configuration and original pad identity cache.
 * ------------------------------------------------------------------------- */

static XPAD_ANALOG_CONFIG_t g_analog;
static CellPadInfo2 g_original_info;
static uint8_t g_info_valid;

static XPAD_OPD_DETOUR_t g_act_detour;
static XPAD_OPD_DETOUR_t g_data_detour;
static XPAD_OPD_DETOUR_t g_extra_detour;
static XPAD_OPD_DETOUR_t g_info_detour;

static void analog_config_load(void) {
  static char buffer[4096];
  int32_t fd;
  uint64_t nread = 0;

  xpad_analog_defaults(&g_analog);
  if (cellFsOpen(XPAD_ANALOG_PATH, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED) {
    game_log("config: xpad_analog.txt absent, defaults active", 0);
    return;
  }
  if (cellFsRead(fd, buffer, sizeof(buffer) - 1, &nread) == CELL_FS_SUCCEEDED) {
    if (nread >= sizeof(buffer)) nread = sizeof(buffer) - 1;
    buffer[nread] = 0;
    xpad_analog_parse_buffer(&g_analog, buffer);
    game_log("config: xpad_analog.txt loaded", 0);
  }
  cellFsClose(fd);
}

static int32_t port_selected(uint32_t port_no) {
  if (port_no >= CELL_PAD_MAX_PORT_NUM) return(0);
  return((g_analog.port_mask & (1U << port_no)) != 0);
}

static int32_t auto_pad_is_target(uint32_t port_no) {
  uint32_t type, cap;

  if (!g_info_valid || !port_selected(port_no)) return(0);
  if (!(g_original_info.port_status[port_no] & CELL_PAD_STATUS_CONNECTED)) return(0);
  type = g_original_info.device_type[port_no];
  cap = g_original_info.device_capability[port_no];
  if (type == CELL_PAD_DEV_TYPE_BD_REMOCON || type == CELL_PAD_DEV_TYPE_LDD) return(0);
  if (cap & CELL_PAD_CAPABILITY_PS3_CONFORMITY) return(0);
  return(1);
}

static int32_t pad_is_target(uint32_t port_no) {
  if (!g_analog.enabled || !port_selected(port_no) ||
      g_analog.game_mode == XPAD_ANALOG_MODE_OFF) return(0);
  if (g_analog.game_mode == XPAD_ANALOG_MODE_FORCE) {
    if (g_info_valid &&
        g_original_info.device_type[port_no] == CELL_PAD_DEV_TYPE_BD_REMOCON) return(0);
    return(1);
  }
  return(auto_pad_is_target(port_no));
}

/* -------------------------------------------------------------------------
 * Rumble IPC (game -> VSH USB driver).
 * ------------------------------------------------------------------------- */

static sys_event_port_t g_rumble_port;
static int32_t g_rumble_connected;
static int32_t g_rumble_send_fails;
static sys_mutex_t g_rumble_mutex;
static int32_t g_rumble_mutex_ok;
static uint8_t g_last_l[CELL_PAD_MAX_PORT_NUM];
static uint8_t g_last_r[CELL_PAD_MAX_PORT_NUM];
static uint8_t g_have_last[CELL_PAD_MAX_PORT_NUM];

static int32_t rumble_connect(void) {
  int32_t r;

  if (g_rumble_connected) return(CELL_OK);
  r = sys_event_port_create(&g_rumble_port, SYS_EVENT_PORT_LOCAL, SYS_EVENT_PORT_NO_NAME);
  if (r != CELL_OK) {
    game_log_code("rumble IPC: event port create failed", r);
    return(r);
  }
  r = xpad_event_port_connect_ipc(g_rumble_port, RUMBLE_IPC_KEY);
  if (r != CELL_OK) {
    game_log_code("rumble IPC: connect failed", r);
    sys_event_port_destroy(g_rumble_port);
    return(r);
  }
  g_rumble_connected = 1;
  g_rumble_send_fails = 0;
  game_log("rumble IPC: connected to VSH queue", 0);
  return(CELL_OK);
}

static void rumble_disconnect(void) {
  if (!g_rumble_connected) return;
  g_rumble_connected = 0;
  g_rumble_send_fails = 0;
  sys_event_port_disconnect(g_rumble_port);
  sys_event_port_destroy(g_rumble_port);
}

static void rumble_send(uint32_t port_no, uint8_t lval, uint8_t rval) {
  int32_t r;

  if (port_no >= CELL_PAD_MAX_PORT_NUM || !g_rumble_mutex_ok) return;
  if (sys_mutex_lock(g_rumble_mutex, 0) != CELL_OK) return;
  if (g_have_last[port_no] && g_last_l[port_no] == lval && g_last_r[port_no] == rval) {
    sys_mutex_unlock(g_rumble_mutex);
    return;
  }
  if (rumble_connect() == CELL_OK) {
    r = sys_event_port_send(g_rumble_port, (uint64_t)port_no,
                            (uint64_t)lval, (uint64_t)rval);
    if (r == CELL_OK) {
      g_rumble_send_fails = 0;
      g_last_l[port_no] = lval;
      g_last_r[port_no] = rval;
      g_have_last[port_no] = 1;
    } else if (++g_rumble_send_fails >= RUMBLE_MAX_SEND_FAILS) {
      rumble_disconnect();
    }
  }
  sys_mutex_unlock(g_rumble_mutex);
}

/* Acknowledge a real module start to the VSH driver. This is deliberately
   sent after hook installation: unlike a webMAN popup chained to the combo,
   the toast cannot claim success when loading or hook setup actually failed. */
static void notify_vsh_game_ready(int32_t installed_hooks) {
  int32_t r;

  /* The marker is the HEN-safe toast path and also proves start completed
     even when the named event queue is unavailable. */
  write_ready_marker(installed_hooks);
  if (!g_rumble_mutex_ok) {
    game_log("rumble IPC: mutex unavailable", 0);
    return;
  }
  r = sys_mutex_lock(g_rumble_mutex, 0);
  if (r != CELL_OK) {
    game_log_code("rumble IPC: mutex lock failed", r);
    return;
  }
  r = rumble_connect();
  if (r == CELL_OK) {
    r = sys_event_port_send(g_rumble_port, XPAD_EV_GAME_READY,
                            (uint64_t)installed_hooks,
                            (uint64_t)(uint32_t)sys_process_getpid());
    if (r == CELL_OK) game_log("rumble IPC: ready event sent", 0);
    else game_log_code("rumble IPC: ready event send failed", r);
  }
  sys_mutex_unlock(g_rumble_mutex);
}

/* -------------------------------------------------------------------------
 * Pad API hooks.
 * ------------------------------------------------------------------------- */

static int32_t hook_padSetActDirect(uint32_t port_no, CellPadActParam *param) {
  typedef int32_t (*original_t)(uint32_t, CellPadActParam *);
  original_t original = (original_t)(void *)g_act_detour.saved;

  if (param != NULL) {
    rumble_send(port_no, param->motor[1], param->motor[0] ? 0xff : 0x00);
  }
  if (original == NULL) return(CELL_PAD_ERROR_UNINITIALIZED);
  return(original(port_no, param));
}

static int32_t hook_padGetInfo2(CellPadInfo2 *info) {
  typedef int32_t (*original_t)(CellPadInfo2 *);
  original_t original = (original_t)(void *)g_info_detour.saved;
  int32_t r, port;

  if (original == NULL) return(CELL_PAD_ERROR_UNINITIALIZED);
  r = original(info);
  if (r != CELL_PAD_OK || info == NULL) return(r);

  memcpy(&g_original_info, info, sizeof(g_original_info));
  g_info_valid = 1;
  if (!g_analog.spoof_pad_info) return(r);

  for (port = 0; port < CELL_PAD_MAX_PORT_NUM; port++) {
    if (!pad_is_target((uint32_t)port)) continue;
    info->device_type[port] = CELL_PAD_DEV_TYPE_STANDARD;
    info->device_capability[port] |= SPOOF_CAPABILITY;
  }
  return(r);
}

static void refresh_original_info(void) {
  typedef int32_t (*original_t)(CellPadInfo2 *);
  original_t original = (original_t)(void *)g_info_detour.saved;
  CellPadInfo2 info;

  if (g_info_valid || original == NULL) return;
  if (original(&info) == CELL_PAD_OK) {
    memcpy(&g_original_info, &info, sizeof(g_original_info));
    g_info_valid = 1;
  }
}

static void correct_game_sticks(uint32_t port_no, CellPadData *data) {
  if (!g_info_valid) refresh_original_info();
  if (pad_is_target(port_no)) xpad_analog_apply(&g_analog, data);
}

static int32_t hook_padGetData(uint32_t port_no, CellPadData *data) {
  typedef int32_t (*original_t)(uint32_t, CellPadData *);
  original_t original = (original_t)(void *)g_data_detour.saved;
  int32_t r;

  if (original == NULL) return(CELL_PAD_ERROR_UNINITIALIZED);
  r = original(port_no, data);
  if (r == CELL_PAD_OK) correct_game_sticks(port_no, data);
  return(r);
}

static int32_t hook_padGetDataExtra(uint32_t port_no, uint32_t *device_type,
                                    CellPadData *data) {
  typedef int32_t (*original_t)(uint32_t, uint32_t *, CellPadData *);
  original_t original = (original_t)(void *)g_extra_detour.saved;
  int32_t r;

  if (original == NULL) return(CELL_PAD_ERROR_UNINITIALIZED);
  r = original(port_no, device_type, data);
  if (r == CELL_PAD_OK) {
    correct_game_sticks(port_no, data);
    if (device_type != NULL && g_analog.spoof_pad_info && pad_is_target(port_no)) {
      *device_type = CELL_PAD_DEV_TYPE_STANDARD;
    }
  }
  return(r);
}

static int32_t install_hooks(void) {
  int32_t installed = 0;

  if (opd_install((void *)cellPadSetActDirect, (void *)hook_padSetActDirect,
                  &g_act_detour) == 0) {
    game_log("hook: cellPadSetActDirect OK", 0);
    installed++;
  } else game_log("hook: cellPadSetActDirect FAILED", 0);

  if (opd_install((void *)cellPadGetData, (void *)hook_padGetData,
                  &g_data_detour) == 0) {
    game_log("hook: cellPadGetData OK", 0);
    installed++;
  } else game_log("hook: cellPadGetData FAILED", 0);

  if (opd_install((void *)cellPadGetDataExtra, (void *)hook_padGetDataExtra,
                  &g_extra_detour) == 0) {
    game_log("hook: cellPadGetDataExtra OK", 0);
    installed++;
  } else game_log("hook: cellPadGetDataExtra FAILED", 0);

  if (opd_install((void *)cellPadGetInfo2, (void *)hook_padGetInfo2,
                  &g_info_detour) == 0) {
    game_log("hook: cellPadGetInfo2 OK", 0);
    installed++;
  } else game_log("hook: cellPadGetInfo2 FAILED", 0);
  return(installed);
}

static void remove_hooks(void) {
  /* Reverse order: data hooks may consult the info hook's saved descriptor. */
  opd_remove(&g_info_detour);
  opd_remove(&g_extra_detour);
  opd_remove(&g_data_detour);
  opd_remove(&g_act_detour);
}

int xpad_game_start(uint64_t arg) {
  sys_mutex_attribute_t mattr;
  int32_t installed_hooks;
  (void)arg;

  game_log("xpad_game v4.0.5 starting", 1);
  memset(&g_original_info, 0, sizeof(g_original_info));
  memset(g_have_last, 0, sizeof(g_have_last));
  g_info_valid = 0;
  analog_config_load();

  sys_mutex_attribute_initialize(mattr);
  g_rumble_mutex_ok = (sys_mutex_create(&g_rumble_mutex, &mattr) == CELL_OK) ? 1 : 0;
  installed_hooks = install_hooks();
  if (installed_hooks == 4) game_log("result: all hooks installed", 0);
  else game_log("result: one or more hooks unavailable; failed paths stay inactive", 0);
  notify_vsh_game_ready(installed_hooks);
  return(SYS_PRX_RESIDENT);
}

int xpad_game_stop(void) {
  remove_hooks();
  if (g_rumble_mutex_ok) {
    sys_mutex_lock(g_rumble_mutex, 0);
    rumble_disconnect();
    sys_mutex_unlock(g_rumble_mutex);
    sys_mutex_destroy(g_rumble_mutex);
    g_rumble_mutex_ok = 0;
  }
  game_log("xpad_game v4.0 stopped", 0);
  return(SYS_PRX_STOP_OK);
}
