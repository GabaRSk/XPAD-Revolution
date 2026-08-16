#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ppu_thread.h>
#include <sys/process.h>
#include <sys/prx.h>
#include <sys/synchronization.h>
#include <sys/event.h>
#include <sys/timer.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netex/net.h>
#include <cell/sysmodule.h>
#include <cell/pad.h>
#include <cell/pad/libpad_dbg.h>
#include <cell/usbd.h>
#include <cell/cell_fs.h>
#include "ControlStruct.h"
#include "xpad_analog.h"
#include "xpad_rumble.h"

#define THREAD_NAME "xpaddt"
#define STOP_THREAD_NAME "xpadds"
#define INJECT_THREAD_NAME "xpaddi"
#define MANUAL_LOADER_THREAD_NAME "xpadgld"
#define VIEWER_THREAD_NAME "xpadnet"
#define VIEWER_DISCOVERY_PORT 39001
#define VIEWER_DISCOVERY_SIZE 8
#define VIEWER_DISCOVERY_STALE_USEC (5ULL * 1000ULL * 1000ULL)
#define VIEWER_PACKET_SIZE 18
#define MAX_XPAD_DEV_NUM ((int32_t)(sizeof(xpad_info) / sizeof(xpad_info[0])))
#define MAX_XPADW_DEV_NUM ((int32_t)(sizeof(xpadw_info) / sizeof(xpadw_info[0])))
#define MAX_PSPAD_DEV_NUM ((int32_t)(sizeof(pspad_info) / sizeof(pspad_info[0])))
#define MAX_XPAD_NUM CELL_PAD_MAX_PORT_NUM
#define MAX_XPADW_NUM 4 // endpoint units on an Xbox 360 wireless receiver (1 per paired pad)
#define MAX_UNIT_NUM (MAX_XPAD_NUM + MAX_XPADW_NUM)
#define XPAD_DATA_LEN 14+2 // +2 for count and size fields
#define XPADW_DATA_LEN 0x13+2
#define MAX_XPAD_PAYLOAD 64 // safe for compatible pads that advertise 64-byte interrupt-IN reports
#define MAX_XPAD_DATA_LEN (MAX_XPAD_PAYLOAD+2) // +2 for count and size fields
#define XPAD_OUT_LEN 12 // largest LED/rumble command we send (wireless rumble is 12 bytes)
#define RINGBUF_SIZE  10
#define XPAD_DEVICES_PATH "/dev_hdd0/plugins/ps3xpad/xpad_devices.txt"
#define XPAD_REMAP_PATH "/dev_hdd0/plugins/ps3xpad/xpad_remap.txt"
#define XPAD_SETTINGS_PATH "/dev_hdd0/plugins/ps3xpad/xpad_settings.txt"
#define XPAD_ANALOG_PATH "/dev_hdd0/plugins/ps3xpad/xpad_analog.txt"
#define REMAP_FILE_SIZE 32768
// debug log; low-volume lifecycle events only (registration, attach/detach,
// transfer errors, game-process injection), see xlog(). Rotated on every
// plugin start (see xlog_init): xpad.log -> xpad.log.1 -> xpad.log.2, so the
// two previous boots are preserved. This matters because a console that
// freezes when launching a game gets rebooted by the user, and without
// rotation that reboot would truncate the very log that captured the freeze.
#define XPAD_LOG_PATH   "/dev_hdd0/plugins/ps3xpad/xpad.log"
#define XPAD_LOG_PATH_1 "/dev_hdd0/plugins/ps3xpad/xpad.log.1"
#define XPAD_LOG_PATH_2 "/dev_hdd0/plugins/ps3xpad/xpad.log.2"
// cellUsbd error codes (cell/usbd_error.h). Earlier revisions of this plugin
// mislabeled 0x80110006 as "not initialized" and retried the whole
// registration pass on it, which can never succeed: 0x80110006 actually
// means an identical LDD is already registered (duplicate name or VID/PID -
// e.g. from a partial earlier pass, a duplicate table entry, or another
// plugin claiming the same device).
#ifndef CELL_USBD_ERROR_NOT_INITIALIZED
#define CELL_USBD_ERROR_NOT_INITIALIZED        ((int32_t)0x80110001)
#endif
#ifndef CELL_USBD_ERROR_LDD_ALREADY_REGISTERED
#define CELL_USBD_ERROR_LDD_ALREADY_REGISTERED ((int32_t)0x80110006)
#endif
// 0x801100FF is returned by cellUsbdRegisterExtraLdd() once cellUsbd's
// fixed-size LDD table is full (retail hardware accepts only ~22 entries,
// shared across every plugin on the system). This built-in device table is
// larger than that budget, so hitting it is EXPECTED, not fatal: the entries
// that registered keep working and the plugin stays active - it just cannot
// claim any further VID/PIDs. The device table is ordered so the controllers
// people actually use register before the budget runs out (see xpad_info).
#define CELL_USBD_ERROR_LDD_TABLE_FULL         ((int32_t)0x801100FF)

// Rumble IPC (game process -> this VSH plugin). The key, queue depth and wire
// format are defined in xpad_rumble.h, shared with the game-side sender so the
// two plugins can never disagree on them. See TODO.md section 8 for the design.
#define RUMBLE_DRAIN_MAX 16     // events drained per poll tick

// Auto-injection of the game-process plugin (xpad_game.sprx) via PS3MAPI.
// The PS3MAPI syscall-8 opcode/argument ABI below was verified against the
// authoritative Cobra/Mamba kernel source (aldostools/Mamba stage2/
// ps3mapi_core.{h,c}) and the ArtemisPS3 userland reference (ps3mapi_ps3_lib.c):
// the syscall-8 dispatch opcode, the syscall variant/argument order, and the
// kernel buffer sizes (MAX_PROCESS 16, MAX_FILE_LEN 256) all match. The earlier
// research-pass values for GET_ALL_PROC_PID (0x0011) and GET_PROC_NAME_BY_PID
// (0x0013) were WRONG and have been corrected to 0x0021 / 0x0022.
//
// LAUNCH-FREEZE FIX — the ABI is correct; the injection *timing* was the bug.
// An earlier enable-by-default froze the console on every game launch. The
// rotated debug log pinned it: the poll thread caught the game process the
// instant multiMAN spawned it and injected immediately —
//
//   [66.648] auto-inject: loading xpad_game.sprx into ..._main_EBOOT.BIN...
//   <no matching "loaded OK" - console frozen on a black screen from here on>
//
// ps3mapi_load_module() for the game EBOOT never returned. Loading a module
// into a process that is still booting (before it has mapped its own segments,
// resolved its prx imports and become schedulable) hangs the kernel prx loader,
// and xpad_game.sprx's start hook then runs against a half-constructed process.
// A manual webMAN "Game Plugins" injection never hits this because by the time
// a human triggers it the game has long since settled into running.
//
// The fix reproduces that safe timing automatically: try_auto_inject() no
// longer injects a freshly-seen process. It waits until a candidate has been
// observed alive across XPAD_INJECT_SETTLE_POLLS consecutive polls — i.e. the
// game has finished booting and is running — before loading the module. The
// same delay also skips multiMAN's transient loader helpers (prepNTFS.self,
// RELOAD.SELF), which exit before they reach the threshold. Input is unaffected
// either way; rumble just begins a few seconds into the game.
//
// INPUT-DEATH FIX — the settle delay stopped the console freeze, but a second
// field log showed the pad still going dead the moment a game starts:
//
//   [86.496] auto-inject: loading xpad_game.sprx into ..._main_EBOOT.BIN...
//   <no "loaded OK" and no "load_module failed" — but the console kept running>
//
// ps3mapi_load_module() hung in the kernel again (it can, even against a
// settled process, depending on CFW/game), and try_auto_inject() used to run
// INLINE on the 10 ms poll thread — the very thread that drains the USB ring
// buffers into cellPadLddDataInsert(). A hung injection therefore no longer
// froze the console, it silently froze THIS PLUGIN's input pump instead, which
// the user experiences as "the controller stops working when the game starts".
//
// Injection therefore runs on its own dedicated low-priority thread now. If
// ps3mapi_load_module() hangs, only that expendable thread blocks: input keeps
// flowing and only rumble auto-setup is lost for the session. The poll thread
// additionally watchdogs the injection thread (see inject_call_t0) and drops a
// breadcrumb in the log when an injection call has been stuck for a while.
//
// LOADER-WEDGE FIX (auto-injection now OPT-IN, default off) — the dedicated
// thread saved the input pump, but a third field report showed the real cost
// of a hung LOAD_PROC_MODULE: after the first injection attempt wedged, the
// console froze on STARTING ANYTHING — multiMAN, games, all of it. The thread
// the plugin sacrifices is expendable; the kernel state it is wedged in is
// not. The hung syscall sits inside lv2's process/PRX loader, and every
// subsequent launch (any process start, any module load) funnels through that
// same loader path and blocks behind it forever. So on a CFW where
// LOAD_PROC_MODULE hangs even post-settle - which this console demonstrably
// is - no amount of plugin-side thread isolation can make auto-injection
// safe: the only safe move is not to issue the syscall at all.
//
// Auto-injection is therefore OPT-IN at runtime: the injection thread is only
// started when the user has created XPAD_INJECT_OPT_IN_PATH (content
// irrelevant, existence is the switch). Without it the plugin makes no
// PS3MAPI calls whatsoever and rumble is set up manually instead via webMAN
// MOD's "Game Plugins" page, which injects xpad_game.sprx long after the game
// has settled and under the user's control. Users whose CFW handles
// LOAD_PROC_MODULE fine can restore the old convenience by creating the file.
#define XPAD_AUTO_INJECT 0 // HEN-safe build: never inject merely because a game appeared
#define XPAD_MANUAL_GAME_LOADER 1 // explicit SELECT+L3+R3 hold, after the game is ready
#define XPAD_GAME_SPRX_PATH "/dev_hdd0/plugins/ps3xpad/xpad_game.sprx"
// existence of this file enables auto-injection (see LOADER-WEDGE FIX above)
#define XPAD_INJECT_OPT_IN_PATH "/dev_hdd0/plugins/ps3xpad/xpad_auto_inject.txt"

#if XPAD_AUTO_INJECT || XPAD_MANUAL_GAME_LOADER
// Cobra/Mamba syscall-8 dispatch and PS3MAPI opcodes (see TODO.md section 8).
// Values verified against aldostools/Mamba stage2/ps3mapi_core.h.
#define SYSCALL8_OPCODE_PS3MAPI            0x7777ULL
#define PS3MAPI_OPCODE_GET_ALL_PROC_PID    0x0021ULL
#define PS3MAPI_OPCODE_GET_PROC_NAME_BY_PID 0x0022ULL
#define PS3MAPI_OPCODE_LOAD_PROC_MODULE    0x0044ULL
#define PS3MAPI_MAX_PROC 16 // == Mamba MAX_PROCESS; kernel fills exactly this many
#endif
#define MAX_EXTRA_DEV_NUM 24
#define EXTRA_NAME_LEN 48
#define DESCRIPTOR_TABLE_SIZE (sizeof(descriptor_table)/sizeof(descriptor_table_t))
#define SWAP16(x) ((uint16_t)((((x) & 0x00FF) << 8) | (((x) & 0xFF00) >> 8)))
// some Cell SDK versions misspell UsbInterfaceDescriptor's subclass field
// ("bInterfaceSublass"), so read it by its USB-spec offset instead of by name
#define USB_IF_SUBCLASS(ifd) (((const uint8_t *)(ifd))[6])

enum XTYPES {
  XTYPE_XBOX360 = 1,
  XTYPE_XBOX360W = 2,
  PTYPE_PS4 = 3,
  PTYPE_PS5 = 4
};

typedef struct xpad_device {
	uint16_t vid;
	uint16_t pid;
	const char *name;
} XPAD_INFO_t;

// xpad device info from linux xpad driver.
//
// ORDER MATTERS. cellUsbd holds registered LDDs in a fixed-size table and
// runs out of slots after a couple dozen entries (retail hardware has been
// observed to accept ~22 before cellUsbdRegisterExtraLdd() returns
// CELL_USBD_ERROR_LDD_TABLE_FULL, 0x801100FF). This table has more entries
// than that, so entries past the budget are silently skipped (non-fatal, see
// register_one()). The controllers this plugin is most used with - the
// standard Xbox 360 pad, the Logitech wired pads, and especially the modern
// 8BitDo / GameSir 2.4GHz dongles - are therefore listed FIRST so they always
// register within the budget. The long tail of legacy arcade sticks, guitars
// and clones follows and is what gets dropped when the table fills up.
static XPAD_INFO_t xpad_info[] = {
	{0x045e, 0x028e, "Xbox 360 / compatible XInput controller"},
	{0x046d, 0xc21d, "Logitech Gamepad F310"},
	{0x046d, 0xc21e, "Logitech Gamepad F510"},
	{0x046d, 0xc21f, "Logitech Gamepad F710"},
	{0x046d, 0xc242, "Logitech Chillstream Controller"},
	// 8BitDo pads/dongles in X mode speak the plain wired XInput protocol,
	// even the 2.4GHz dongles - they are NOT the Microsoft wireless receiver.
	// Listed high up so this plugin's headline controllers always fit within
	// the cellUsbd LDD budget (see the ORDER MATTERS note above).
	// NOTE: every name in these tables doubles as the USBD LDD name and must
	// be unique - registering two LDDs with the same name fails with
	// CELL_USBD_ERROR_LDD_ALREADY_REGISTERED (0x80110006)
	{0x2dc8, 0x301c, "8BitDo Ultimate 2C Wireless (2.4GHz dongle)"},
	{0x2dc8, 0x3106, "8BitDo Ultimate Wireless / Pro 2 Wired"},
	{0x2dc8, 0x3109, "8BitDo Ultimate Wireless (Bluetooth dongle)"},
	{0x2dc8, 0x310a, "8BitDo Ultimate 2C Wireless (2.4GHz dongle v2)"},
	{0x2dc8, 0x310b, "8BitDo Ultimate 2 Wireless (2.4GHz dongle)"},
	{0x2dc8, 0x6001, "8BitDo SN30 Pro"},
	// GameSir XInput pads; their 2.4GHz dongles also present as wired XInput
	{0x3537, 0x1004, "GameSir T4 Kaleid"},
	{0x3537, 0x100f, "GameSir Nova 2 Lite"},
	{0x044f, 0xb326, "Thrustmaster Gamepad GP XID"},
	{0x0738, 0x4716, "Mad Catz Wired Xbox 360 Controller" },
	{0x0738, 0x4718, "Mad Catz Street Fighter IV FightStick SE"},
	{0x0738, 0x4728, "Mad Catz Street Fighter IV FightPad"},
	{0x0738, 0x4736, "Mad Catz MicroCon Gamepad"},
	{0x0738, 0x4738, "Mad Catz Wired Xbox 360 Controller (SFIV)"},
	{0x0738, 0xbeef, "Mad Catz JOYTECH NEO SE Advanced GamePad"},
	{0x0e6f, 0x0113, "Afterglow AX.1 Gamepad for Xbox 360"},
	{0x0e6f, 0x011f, "Rock Candy Gamepad Wired Controller"},
	{0x0e6f, 0x0201, "Pelican PL-3601 'TSZ' Wired Xbox 360 Controller"},
	{0x0e6f, 0x0213, "Afterglow Gamepad for Xbox 360" },
	{0x0e6f, 0x0301, "Logic3 Controller"},
	{0x0e6f, 0x0401, "Logic3 Controller (0401)"},
	{0x0e6f, 0x0501, "PDP Xbox 360 Controller"},
	{0x0e6f, 0xf900, "PDP Afterglow AX.1"},
	{0x0f0d, 0x000d, "Hori Fighting Stick EX2"},
	{0x0f0d, 0x0016, "Hori Real Arcade Pro.EX"},
	{0x0f0d, 0x001b, "Hori Real Arcade Pro VX"},
	{0x11c9, 0x55f0, "Nacon GC-100XF"},
	{0x12ab, 0x0301, "PDP AFTERGLOW AX.1 (12ab)"},
	{0x1430, 0x4748, "RedOctane Guitar Hero X-plorer"},
	{0x1430, 0xf801, "RedOctane Controller"},
	{0x146b, 0x0601, "BigBen Interactive XBOX 360 Controller"},
	{0x1532, 0x0037, "Razer Sabertooth"},
	{0x15e4, 0x3f00, "Power A Mini Pro Elite"},
	{0x15e4, 0x3f0a, "Xbox Airflo wired controller"},
	{0x162e, 0xbeef, "Joytech Neo-Se Take2"},
	{0x1689, 0xfd00, "Razer Onza Tournament Edition"},
	{0x1689, 0xfd01, "Razer Onza Classic Edition"},
	// same product under a second VID/PID; the "(hex)" suffixes keep LDD
	// names unique (see the 0x80110006 note below)
	{0x1689, 0xfe00, "Razer Sabertooth (fe00)"},
	{0x1bad, 0x0003, "Harmonix Rock Band Drumkit"},
	{0x1bad, 0xf016, "Mad Catz Xbox 360 Controller"},
	{0x1bad, 0xf028, "Street Fighter IV FightPad"},
	{0x1bad, 0xf038, "Street Fighter IV FightStick TE"},
	{0x1bad, 0xf900, "Harmonix Xbox 360 Controller"},
	{0x1bad, 0xf901, "Gamestop Xbox 360 Controller"},
	{0x1bad, 0xf903, "Tron Xbox 360 controller"},
	{0x24c6, 0x5000, "Razer Atrox Arcade Stick"},
	{0x24c6, 0x5300, "PowerA MINI PROEX Controller"},
	{0x24c6, 0x530a, "Xbox 360 Pro EX Controller"},
	{0x24c6, 0x531a, "PowerA Pro Ex"},
	{0x24c6, 0x5397, "FUS1ON Tournament Controller"},
	{0x24c6, 0x5500, "Hori XBOX 360 EX 2 with Turbo"},
	{0x24c6, 0x5501, "Hori Real Arcade Pro VX-SA"},
	{0x24c6, 0x5503, "Hori Fighting Edge"},
	{0x24c6, 0x5b02, "Thrustmaster GPX Controller"},
	{0x24c6, 0x5d04, "Razer Sabertooth (5d04)"},
	{0x24c6, 0xfafe, "Rock Candy Gamepad for Xbox 360"},
};

static XPAD_INFO_t xpadw_info[] = {
 {0x045e, 0x0291, "Xbox 360 Wireless Receiver (XBOX)"},
 {0x045e, 0x0719, "Xbox 360 Wireless Receiver"},
};

typedef struct ps_pad_device {
	uint16_t vid;
	uint16_t pid;
	const char *name;
	uint8_t xtype;
} PSPAD_INFO_t;

/* Sony HID pads must be registered before the long XInput table consumes
   cellUsbd's small extra-LDD budget. These are direct USB connections: the
   raw report still contains the touchpad click before the PS3 pad driver
   reduces it to CellPadData. */
static PSPAD_INFO_t pspad_info[] = {
	{0x054c, 0x05c4, "Sony DualShock 4 (CUH-ZCT1)", PTYPE_PS4},
	{0x054c, 0x09cc, "Sony DualShock 4 (CUH-ZCT2)", PTYPE_PS4},
	{0x054c, 0x0ce6, "Sony DualSense", PTYPE_PS5},
	{0x054c, 0x0df2, "Sony DualSense Edge", PTYPE_PS5},
};

// user-supplied devices loaded from XPAD_DEVICES_PATH at plugin start
typedef struct {
	uint16_t vid;
	uint16_t pid;
	uint8_t xtype;
	char name[EXTRA_NAME_LEN];
} XPAD_EXTRA_t;

static XPAD_EXTRA_t extra_info[MAX_EXTRA_DEV_NUM];
static int32_t extra_count = 0;
static int32_t extra_skipped = 0; // device list lines rejected by the parser

// which table entries currently have an extra LDD registered. This makes
// register_devices() idempotent (a retry pass must not re-register entries
// that already succeeded - re-registering fails with 0x80110006) and lets
// shutdown_usb() unregister exactly what was registered.
static uint8_t xpad_lddreg[sizeof(xpad_info) / sizeof(xpad_info[0])];
static uint8_t xpadw_lddreg[sizeof(xpadw_info) / sizeof(xpadw_info[0])];
static uint8_t pspad_lddreg[sizeof(pspad_info) / sizeof(pspad_info[0])];
static uint8_t extra_lddreg[MAX_EXTRA_DEV_NUM];
static int32_t reg_conflicts;      // entries skipped: LDD already registered
static int32_t reg_budget_full;    // cellUsbd LDD table filled up mid-pass
static int32_t reg_skipped_full;   // entries left unregistered once it filled up
static uint16_t reg_fail_vid, reg_fail_pid; // entry a fatal error occurred on

typedef struct {
  uint8_t bDescriptorType;
  int32_t (*dump_descriptor)(int32_t dev_id, void *desc);
} descriptor_table_t;

typedef struct xpad_unit {
  int32_t dev_id; /* Device id */
  uint16_t vid; /* USB vendor id */
  uint16_t pid; /* USB product id */
  const char *name; /* Device name for notifications (points into a device table) */
  int32_t unit_idx; /* Index into XPAD.units[] */
  int32_t number; /* Pad slot (index into handle[]), -1 while no LDD pad is registered */
  int32_t last_port; /* Last known cellPadLddGetPortNo() result, -1 if unknown */
  int32_t c_pipe; /* Control pipe id */
  int32_t i_pipe; /* In pipe id */
  int32_t o_pipe; /* Out pipe id */
  int32_t payload; /* Size of payload */
  uint8_t ifnum; /* Interface number */
  uint8_t as; /* Alternate setting number */
  int32_t tcount; /* Transfer counts */
  uint8_t xtype;

  /* Preserve short button presses and trigger peaks while the 2 ms worker
     consumes only the newest USB state. Values use the same byte order as
     XBOX360_IN_REPORT on the big-endian PPU. */
  uint16_t producer_buttons;
  uint16_t pending_press_edges;
  uint32_t producer_ps_buttons;
  uint32_t pending_ps_press_edges;
  uint8_t pending_trig_l;
  uint8_t pending_trig_r;

  // methods to their respective controllers
  int32_t (*read_input)(struct xpad_unit *unit, void *data);
  int32_t (*set_led)(struct xpad_unit *unit, uint8_t led);
  int32_t (*set_rumble)(struct xpad_unit *unit, uint8_t lval, uint8_t rval);

  /* Ring buffer (one per unit so pads do not contend with each other) */
  sys_mutex_t rb_mutex; /* Guards rp/wp/rblen/ringbuf and the fields below */
  int32_t rp; /* Read pointer   */
  int32_t wp; /* Write pointer  */
  int32_t rblen; /* Buffer length  */
  unsigned char ringbuf[RINGBUF_SIZE][MAX_XPAD_DATA_LEN]; /* Ring buffer */

  /* Out-transfer buffer: cellUsbdInterruptTransfer() is asynchronous, so
     LED/rumble payloads must live here, not in the caller's stack frame.
     out_busy is set while a transfer using out_data is in flight. */
  uint8_t out_data[XPAD_OUT_LEN];
  uint8_t out_busy;

  /* Lifecycle guards: 'closing' stops completion callbacks from touching
     the unit or resubmitting transfers; 'pending' counts async USBD
     requests whose callbacks have not finished yet. unit_free() sets
     'closing', closes the pipes and waits for 'pending' to drain. */
  volatile int32_t pending;
  volatile uint8_t closing;

  /* Debug-log state: first successful input report logged once, transfer
     errors logged up to a small cap so a dying device cannot flood the log */
  uint8_t dbg_first_in;
  uint8_t dbg_err_count;

  /* Buffer for interrupt transfer */
  unsigned char data[0];

} XPAD_UNIT_t;

typedef struct {
  int32_t n; /* Number of claimed pad slots */
  XPAD_UNIT_t *units[MAX_UNIT_NUM]; /* Every allocated endpoint unit (wired or wireless) */
  XPAD_UNIT_t *pad_unit[MAX_XPAD_NUM]; /* Which unit owns each pad slot, NULL = free */
} XPAD_t;

typedef struct {
  uint16_t buttons;
  uint8_t lt;
  uint8_t rt;
  uint8_t lx;
  uint8_t ly;
  uint8_t rx;
  uint8_t ry;
  uint8_t connected;
} VIEWER_STATE_t;

/* USB report layouts shared by DS4/DualSense revisions. Only the leading
   gamepad fields are named; the remainder contains sensors, touch contacts
   and status bytes which are not needed to expose the touchpad click. */
typedef struct {
  uint8_t report_id; /* 0x01 */
  uint8_t x, y, rx, ry;
  uint8_t buttons[3];
  uint8_t z, rz;
  uint8_t remainder[54];
} PACKED DS4_USB_REPORT_t;

typedef struct {
  uint8_t report_id; /* 0x01 */
  uint8_t x, y, rx, ry;
  uint8_t z, rz;
  uint8_t sequence;
  uint8_t buttons[4];
  uint8_t remainder[52];
} PACKED DS5_USB_REPORT_t;

/* HEN-safe remapping happens on the normalized CellPadData immediately
   before cellPadLddDataInsert(). It therefore needs no game-process hook,
   DEX EBOOT or PS3MAPI injection. Digital mappings keep pressure values,
   so an analog trigger may be mapped to a pressure-sensitive face button. */
enum REMAP_DIGITAL_ID {
  REMAP_CROSS = 0,
  REMAP_CIRCLE,
  REMAP_TRIANGLE,
  REMAP_SQUARE,
  REMAP_R1,
  REMAP_R2,
  REMAP_R3,
  REMAP_L1,
  REMAP_L2,
  REMAP_L3,
  REMAP_DPAD_UP,
  REMAP_DPAD_LEFT,
  REMAP_DPAD_DOWN,
  REMAP_DPAD_RIGHT,
  REMAP_START,
  REMAP_SELECT,
  REMAP_PS,
  REMAP_DIGITAL_OUTPUT_COUNT,
  REMAP_TOUCHPAD = REMAP_DIGITAL_OUTPUT_COUNT,
  REMAP_DIGITAL_COUNT
};

#define REMAP_EXTRA_TOUCHPAD 0x01

enum REMAP_AXIS_ID {
  REMAP_AXIS_LEFT_X = 0,
  REMAP_AXIS_LEFT_Y,
  REMAP_AXIS_RIGHT_X,
  REMAP_AXIS_RIGHT_Y,
  REMAP_AXIS_COUNT
};

typedef struct {
  uint8_t digital_map[REMAP_DIGITAL_COUNT];
  uint32_t digital_explicit;
  uint8_t axis_map[REMAP_AXIS_COUNT];
  uint8_t axis_explicit;
  uint8_t invert_axes;
  uint8_t profile;
  uint8_t enabled;
  uint8_t start_enabled;
  uint8_t loaded;
  uint16_t rules;
  uint16_t rejected;
} REMAP_CONFIG_t;

int xpadd_start(uint64_t arg);
int xpadd_stop(void);

// wired Xbox 360 controller methods
static int32_t xpad_probe(int32_t dev_id);
static int32_t xpad_attach(int32_t dev_id);
static int32_t xpad_detach(int32_t dev_id);
static int32_t xpad_detach_all(void);
static int32_t xpad_read_input(XPAD_UNIT_t *unit, void *data);
static void xpad_read_report(int32_t id, uint8_t *readBuf);
static int32_t xpad_set_led(XPAD_UNIT_t *unit, uint8_t led);
static int32_t xpad_set_rumble(XPAD_UNIT_t *unit, uint8_t lval, uint8_t rval);

// wired DualShock 4 / DualSense methods
static int32_t pspad_probe(int32_t dev_id);
static int32_t pspad_attach(int32_t dev_id);
static int32_t pspad_detach(int32_t dev_id);
static int32_t pspad_detach_all(void);
static int32_t pspad_read_input(XPAD_UNIT_t *unit, void *data);
static void pspad_read_report(XPAD_UNIT_t *unit, uint8_t *readBuf);
static int32_t pspad_set_led(XPAD_UNIT_t *unit, uint8_t led);
static int32_t pspad_set_rumble(XPAD_UNIT_t *unit, uint8_t lval, uint8_t rval);

// wireless Xbox 360 controller methods
static int32_t xpadw_probe(int32_t dev_id);
static int32_t xpadw_attach(int32_t dev_id);
static int32_t xpadw_detach(int32_t dev_id);
static int32_t xpadw_detach_all(void);
static int32_t xpadw_read_input(XPAD_UNIT_t *unit, void *data);
static void xpadw_read_report(int32_t id, uint8_t *readBuf);
static int32_t xpadw_set_led(XPAD_UNIT_t *unit, uint8_t led);
static int32_t xpadw_set_rumble(XPAD_UNIT_t *unit, uint8_t lval, uint8_t rval);

// common methods
static void data_transfer_done(int32_t result, int32_t count, void *arg);
static void data_transfer(XPAD_UNIT_t *unit);
static void set_config_done(int32_t result, int32_t count, void *arg);
static void set_interface_done(int32_t result, int32_t count, void *arg);
static XPAD_UNIT_t *unit_alloc(int32_t dev_id, int32_t payload, uint8_t ifnum, uint8_t as, uint8_t xtype);
static void unit_free(XPAD_UNIT_t *unit);
static int32_t check_pad_status(void);
static int32_t register_ldd_controller(XPAD_UNIT_t *unit);
static int32_t unregister_ldd_controller(XPAD_UNIT_t *unit);

// rumble IPC
static int32_t create_rumble_queue(void);
static void destroy_rumble_queue(void);
static void route_rumble(int32_t port, uint8_t lval, uint8_t rval);
static void drain_rumble_queue(void);
static void poll_game_ready_file(void);
#if XPAD_MANUAL_GAME_LOADER
static void poll_manual_game_loader(void);
static void manual_loader_feed_normalized(int32_t id, const CellPadData *data);
#endif
#if XPAD_AUTO_INJECT
static void try_auto_inject(void);
#endif

// notifications
static void notify_pad_connected(XPAD_UNIT_t *unit);
static void notify_pad_disconnected(XPAD_UNIT_t *unit);

// UDP gamepad viewer
static void viewer_network_thread(uint64_t arg);
static void viewer_update_x360(int32_t id, const XBOX360_IN_REPORT *report);
static void viewer_update_x360w(int32_t id, const XBOX360W_IN_REPORT *report);
static void viewer_set_connected(int32_t id, uint8_t connected);

// HEN-safe VSH remapping
static void load_remap_config(void);
static void remap_apply(int32_t id, CellPadData *data, uint8_t extra_buttons);
static void load_analog_config(void);

// debug logging to XPAD_LOG_PATH (no-ops if the log file cannot be created)
static void xlog(const char *text);
static void xlog_code(const char *what, int32_t code);
static void xlog_dev(const char *what, uint16_t vid, uint16_t pid, int32_t code);
static void xlog_proc(const char *what, const char *name, uint32_t pid);

// vsh methods
void *getNIDfunc(const char *vsh_module, uint32_t fnid, int32_t offset);
static void show_msg(char *msg);
int (*vshtask_notify)(int, const char *) = NULL;
void *(*vsh_malloc)(unsigned int size) = NULL;
int (*vsh_free)(void *ptr) = NULL;

// usb methods
static int32_t get_device_desc(int32_t dev_id, void *p);
static int32_t get_configration_desc(int32_t dev_id, void *p);
static int32_t get_interface_desc(int32_t dev_id, void *p);
static int32_t get_endpoint_desc(int32_t dev_id, void *p);

descriptor_table_t descriptor_table[] = {
  {USB_DESCRIPTOR_TYPE_DEVICE, get_device_desc},
  {USB_DESCRIPTOR_TYPE_CONFIGURATION, get_configration_desc},
  {USB_DESCRIPTOR_TYPE_INTERFACE, get_interface_desc},
  {USB_DESCRIPTOR_TYPE_ENDPOINT, get_endpoint_desc},
};

static CellUsbdLddOps xpad_ops = {
  0,
  xpad_probe,
  xpad_attach,
  xpad_detach
};

static CellUsbdLddOps xpadw_ops = {
  0,
  xpadw_probe,
  xpadw_attach,
  xpadw_detach
};

static CellUsbdLddOps pspad_ops = {
  0,
  pspad_probe,
  pspad_attach,
  pspad_detach
};

static XPAD_t XPAD;
static uint8_t xpad_led[4] = {ledOn1, ledOn2, ledOn3, ledOn4};
static sys_ppu_thread_t thread_id = 1;
static sys_ppu_thread_t viewer_thread_id = (sys_ppu_thread_t)-1;
static sys_mutex_t xpad_mutex;
static sys_mutex_t viewer_mutex;
static int32_t handle[CELL_PAD_MAX_PORT_NUM];
static volatile uint8_t running;
static sys_event_queue_t rumble_queue;
static uint8_t rumble_queue_ready; // set once the queue exists
static VIEWER_STATE_t viewer_state[MAX_XPAD_NUM];
static uint16_t viewer_sequence[MAX_XPAD_NUM];
static REMAP_CONFIG_t remap_config;
static XPAD_ANALOG_CONFIG_t analog_config;
static uint8_t remap_hotkey_down[MAX_XPAD_NUM];
#if XPAD_MANUAL_GAME_LOADER
static volatile uint8_t game_loader_inflight;
static volatile uint32_t game_loader_attempted_pid;
static volatile uint32_t game_loader_active_pid;
static volatile uint64_t game_loader_call_t0;
static uint8_t game_loader_hang_logged;
static uint8_t game_loader_combo_latched;
static uint64_t game_loader_combo_since;
static uint64_t game_loader_last_pad_poll;
static uint64_t game_loader_last_pid_poll;
static uint16_t game_loader_pad_d1[MAX_XPAD_NUM];
static uint16_t game_loader_pad_d2[MAX_XPAD_NUM];
static uint64_t game_loader_usb_combo_since[MAX_XPAD_NUM];
static uint8_t game_loader_usb_combo_latched[MAX_XPAD_NUM];
static volatile uint8_t game_loader_usb_request_port;
static uint8_t game_loader_libpad_probe_logged;
static uint8_t game_loader_libpad_init_attempted;
static uint8_t game_loader_libpad_button_logs;
#endif

SYS_MODULE_INFO(XPADD, 0, 1, 0);
SYS_MODULE_START(xpadd_start);
SYS_MODULE_STOP(xpadd_stop);

static inline void _sys_ppu_thread_exit(uint64_t val) {
  system_call_1(41, val);
}

static inline sys_prx_id_t prx_get_module_id_by_address(void *addr) {
  system_call_1(461, (uint64_t)(uint32_t)addr);
  return((int)p1);
}

// lv2 sys_time_get_current_time (syscall 145); used only to timestamp the
// debug log, so relative accuracy between two log lines is all that matters
static inline uint64_t time_now_usec(void) {
  uint64_t sec = 0, nsec = 0;

  system_call_2(145, (uint64_t)(uint32_t)&sec, (uint64_t)(uint32_t)&nsec);
  return(sec * 1000000ULL + nsec / 1000ULL);
}

static inline void sys_pad_dbg_ldd_register_controller(uint8_t *data, int32_t *handle, uint8_t addr, uint32_t capability) {

  // syscall for registering a virtual controller with custom capabilities
  system_call_4(574, (uint64_t)(uint32_t)data, (uint64_t)(uint32_t)handle,
                (uint64_t)addr, (uint64_t)capability);
}

static inline void sys_pad_dbg_ldd_set_data_insert_mode(int32_t handle, uint16_t addr, uint32_t *mode, uint8_t addr2) {

  // syscall for controlling button data filter (allows a virtual controller to be used in games)
  system_call_4(573, (uint64_t)handle, (uint64_t)addr,
                (uint64_t)(uint32_t)mode, (uint64_t)addr2);
}

void *getNIDfunc(const char * vsh_module, uint32_t fnid, int32_t offset) {

  // from webman-MOD source
  // used to find malloc, free, and show notification

  // 0x10000 = ELF
  // 0x10080 = segment 2 start
  // 0x10200 = code start

  uint32_t table = (*(uint32_t *)0x1008C) + 0x984; // vsh table address
  //  uint32_t table = (*(uint32_t*)0x1002C) + 0x214 - 0x10000; // vsh table address
  //  uint32_t table = 0x63A9D4;

  while (((uint32_t)*(uint32_t *)table) != 0) {
    uint32_t *export_stru_ptr = (uint32_t *)*(uint32_t *)table; // ptr to export stub, size 2C, "sys_io" usually... Exports:0000000000635BC0 stru_635BC0:    ExportStub_s <0x1C00, 1, 9, 0x39, 0, 0x2000000, aSys_io, ExportFNIDTa
    const char *lib_name_ptr =  (const char *)*(uint32_t *)((char *)export_stru_ptr + 0x10);
    if(strncmp(vsh_module, lib_name_ptr, strlen(lib_name_ptr)) == 0) {
      // we got the proper export struct
      uint32_t lib_fnid_ptr = *(uint32_t *)((char *)export_stru_ptr + 0x14);
      uint32_t lib_func_ptr = *(uint32_t *)((char *)export_stru_ptr + 0x18);
      uint16_t count = *(uint16_t *)((char *)export_stru_ptr + 6); // number of exports
      for (int i = 0; i < count; i++) {
        if (fnid == *(uint32_t *)((char *)lib_fnid_ptr + i*4)) {
          // take address from OPD
          return (void **)*((uint32_t *)(lib_func_ptr) + i) + offset;
        }
      }
    }
    table = table + 4;
  }
  return(0);
}

static void show_msg(char* msg) {

  // from webman-MOD
  // displays a notification on the PS3
  if (!vshtask_notify) {
    vshtask_notify = (void *)((int)getNIDfunc("vshtask", 0xA02D46E7, 0));
  }
  if (strlen(msg) > 200) {
    msg[200] = 0;
  }
  xlog(msg); // every on-screen notification also lands in the debug log
  if (vshtask_notify) {
    vshtask_notify(0, msg);
  }
}

// tiny formatting helpers (no printf in this PRX)
static char *append_str(char *dst, const char *src) {
  while (*src) {
    *dst++ = *src++;
  }
  *dst = 0;
  return(dst);
}

static char *append_hex16(char *dst, uint16_t v) {
  static const char hexdig[] = "0123456789ABCDEF";
  int32_t i;

  for (i = 12; i >= 0; i -= 4) {
    *dst++ = hexdig[(v >> i) & 0xF];
  }
  *dst = 0;
  return(dst);
}

static char *append_int(char *dst, int32_t v) {
  char tmp[12];
  int32_t i = 0;

  if (v < 0) {
    *dst++ = '-';
    v = -v;
  }
  do {
    tmp[i++] = (char)('0' + (v % 10));
    v /= 10;
  } while (v > 0);
  while (i > 0) {
    *dst++ = tmp[--i];
  }
  *dst = 0;
  return(dst);
}

static char *append_hex32(char *dst, uint32_t v) {
  static const char hexdig[] = "0123456789ABCDEF";
  int32_t i;

  for (i = 28; i >= 0; i -= 4) {
    *dst++ = hexdig[(v >> i) & 0xF];
  }
  *dst = 0;
  return(dst);
}

// bounded append so a long message cannot overrun a log line buffer
static char *append_strn(char *dst, const char *src, int32_t max) {
  while (*src && max-- > 0) {
    *dst++ = *src++;
  }
  *dst = 0;
  return(dst);
}

// ---------------------------------------------------------------------------
// debug log. One short line per lifecycle event (registration, attach,
// detach, transfer errors, game-process injection), timestamped in seconds
// since plugin start, to XPAD_LOG_PATH. Each line is written with its own
// open/append/close so a VSH crash loses at most one line and no file
// descriptor is shared between the plugin thread and USBD callbacks.
static sys_mutex_t log_mutex;
static uint8_t log_ok;    // log file created and mutex usable
static uint64_t log_t0;   // timestamp origin

// Rotate the previous boot's log out of the way before starting a fresh one,
// keeping the two most recent old logs (xpad.log.1 = last boot, xpad.log.2 =
// the boot before that). Renames are cheap and best-effort: a missing source
// (first ever boot, or a boot that never created a log) just makes that rename
// a no-op error we ignore. Done before the fresh XPAD_LOG_PATH is truncated so
// the current file still holds last boot's contents at rename time.
static void xlog_rotate(void) {
  cellFsUnlink(XPAD_LOG_PATH_2);            // drop the oldest kept log
  cellFsRename(XPAD_LOG_PATH_1, XPAD_LOG_PATH_2); // last boot -> older slot
  cellFsRename(XPAD_LOG_PATH, XPAD_LOG_PATH_1);   // this file -> last-boot slot
}

static void xlog_init(void) {
  sys_mutex_attribute_t mutex_attr;
  int32_t fd;

  sys_mutex_attribute_initialize(mutex_attr);
  if (sys_mutex_create(&log_mutex, &mutex_attr) != CELL_OK) {
    return;
  }
  // preserve the last two boots' logs so a freeze-then-reboot does not wipe
  // the log that captured the freeze
  xlog_rotate();
  if (cellFsOpen(XPAD_LOG_PATH, CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC, &fd, NULL, 0) != CELL_FS_SUCCEEDED) {
    sys_mutex_destroy(log_mutex);
    return; // no log (e.g. no space); the plugin works fine without it
  }
  cellFsClose(fd);
  log_t0 = time_now_usec();
  log_ok = 1;
  xlog("xpad debug log start (timestamps: seconds since plugin start)");
}

static void xlog(const char *text) {
  char buf[224], *m;
  int32_t fd;
  uint64_t nw, dt;
  uint32_t ms;

  if (!log_ok) {
    return;
  }
  dt = time_now_usec() - log_t0;
  ms = (uint32_t)((dt / 1000ULL) % 1000ULL);
  m = buf;
  *m++ = '[';
  m = append_int(m, (int32_t)(dt / 1000000ULL));
  *m++ = '.';
  *m++ = (char)('0' + (ms / 100) % 10);
  *m++ = (char)('0' + (ms / 10) % 10);
  *m++ = (char)('0' + ms % 10);
  m = append_str(m, "] ");
  m = append_strn(m, text, 200);
  *m++ = '\n';
  // plain lock/unlock (not block()/unblock()): those exit the calling thread
  // on failure, which must never happen on a USBD callback thread
  if (sys_mutex_lock(log_mutex, 0) != CELL_OK) {
    return;
  }
  if (cellFsOpen(XPAD_LOG_PATH, CELL_FS_O_WRONLY | CELL_FS_O_APPEND, &fd, NULL, 0) == CELL_FS_SUCCEEDED) {
    cellFsWrite(fd, buf, (uint64_t)(m - buf), &nw);
    cellFsClose(fd);
  }
  sys_mutex_unlock(log_mutex);
}

// "<what> (code 0x00000000)"
static void xlog_code(const char *what, int32_t code) {
  char msg[192], *m;

  if (!log_ok) {
    return;
  }
  m = msg;
  m = append_strn(m, what, 140);
  m = append_str(m, " (code 0x");
  m = append_hex32(m, (uint32_t)code);
  m = append_str(m, ")");
  xlog(msg);
}

// "<what> VID:PID (code 0x00000000)"; code 0 is omitted
static void xlog_dev(const char *what, uint16_t vid, uint16_t pid, int32_t code) {
  char msg[192], *m;

  if (!log_ok) {
    return;
  }
  m = msg;
  m = append_strn(m, what, 120);
  m = append_str(m, " ");
  m = append_hex16(m, vid);
  m = append_str(m, ":");
  m = append_hex16(m, pid);
  if (code != 0) {
    m = append_str(m, " (code 0x");
    m = append_hex32(m, (uint32_t)code);
    m = append_str(m, ")");
  }
  xlog(msg);
}

// "<what> <name> (pid N)"; used for game-process (injection) logging where
// the process name is the identifying detail, not a VID/PID
static void xlog_proc(const char *what, const char *name, uint32_t pid) {
  char msg[256], *m;

  if (!log_ok) {
    return;
  }
  m = msg;
  m = append_strn(m, what, 100);
  m = append_str(m, " ");
  m = append_strn(m, name, 120);
  m = append_str(m, " (pid ");
  m = append_int(m, (int32_t)pid);
  m = append_str(m, ")");
  xlog(msg);
}
// ---------------------------------------------------------------------------

// find the display name for a VID/PID across the built-in and user tables
static const char *find_device_name(uint16_t vid, uint16_t pid) {
  int32_t i;

  for (i = 0; i < MAX_PSPAD_DEV_NUM; i++) {
    if (pspad_info[i].vid == vid && pspad_info[i].pid == pid) {
      return(pspad_info[i].name);
    }
  }
  for (i = 0; i < MAX_XPAD_DEV_NUM; i++) {
    if (xpad_info[i].vid == vid && xpad_info[i].pid == pid) {
      return(xpad_info[i].name);
    }
  }
  for (i = 0; i < MAX_XPADW_DEV_NUM; i++) {
    if (xpadw_info[i].vid == vid && xpadw_info[i].pid == pid) {
      return(xpadw_info[i].name);
    }
  }
  for (i = 0; i < extra_count; i++) {
    if (extra_info[i].vid == vid && extra_info[i].pid == pid) {
      return(extra_info[i].name);
    }
  }
  return(NULL);
}

// parse up to 4 hex digits (optional 0x prefix); advances *pp past the number
static int32_t parse_hex16(char **pp, uint16_t *out) {
  char *p = *pp;
  uint32_t v = 0;
  int32_t digits = 0, d;

  while (*p == ' ' || *p == '\t') {
    p++;
  }
  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
    p += 2;
  }
  for (;;) {
    char c = *p;
    if (c >= '0' && c <= '9') {
      d = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      d = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      d = c - 'A' + 10;
    } else {
      break;
    }
    if (++digits > 4) {
      return(-1);
    }
    v = (v << 4) | (uint32_t)d;
    p++;
  }
  if (digits == 0) {
    return(-1);
  }
  *pp = p;
  *out = (uint16_t)v;
  return(0);
}

// trim whitespace and stray quotes from both ends, in place
static char *trim_field(char *s) {
  char *end;

  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '"') {
    s++;
  }
  end = s + strlen(s);
  while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '"')) {
    *--end = 0;
  }
  return(s);
}

static int32_t builtin_known(uint16_t vid, uint16_t pid) {
  int32_t i;

  for (i = 0; i < MAX_PSPAD_DEV_NUM; i++) {
    if (pspad_info[i].vid == vid && pspad_info[i].pid == pid) {
      return(1);
    }
  }
  for (i = 0; i < MAX_XPAD_DEV_NUM; i++) {
    if (xpad_info[i].vid == vid && xpad_info[i].pid == pid) {
      return(1);
    }
  }
  for (i = 0; i < MAX_XPADW_DEV_NUM; i++) {
    if (xpadw_info[i].vid == vid && xpadw_info[i].pid == pid) {
      return(1);
    }
  }
  return(0);
}

static int32_t extra_known(uint16_t vid, uint16_t pid) {
  int32_t i;

  for (i = 0; i < extra_count; i++) {
    if (extra_info[i].vid == vid && extra_info[i].pid == pid) {
      return(1);
    }
  }
  return(0);
}

static void devlist_warn(int32_t line_no, const char *reason) {
  char msg[96], *m;

  extra_skipped++;
  m = msg;
  m = append_str(m, "XPAD Rev: devices.txt line ");
  m = append_int(m, line_no);
  m = append_str(m, " skipped (");
  m = append_str(m, reason);
  m = append_str(m, ")");
  show_msg(msg);
}

// load user devices from XPAD_DEVICES_PATH; format per line:
//   VID, PID, NAME, XTYPE     (# comments, blank lines and quotes tolerated)
static void load_extra_devices(void) {
  static char fbuf[8192];
  int32_t fd, line_no = 0;
  uint64_t nread = 0;
  char *line, *next, *p, *name, *xt, *last_comma, *scan;
  uint16_t vid, pid;
  uint8_t xtype;

  if (cellFsOpen(XPAD_DEVICES_PATH, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED) {
    xlog("no xpad_devices.txt, using built-in device table only");
    return; // no device list installed - built-ins only
  }
  cellFsRead(fd, fbuf, sizeof(fbuf) - 1, &nread);
  cellFsClose(fd);
  fbuf[nread] = 0;

  line = fbuf;
  while (line != NULL && *line != 0) {
    line_no++;
    if ((next = strchr(line, '\n')) != NULL) {
      *next++ = 0;
    }

    // '#' starts a comment anywhere on the line
    if ((p = strchr(line, '#')) != NULL) {
      *p = 0;
    }
    p = trim_field(line);
    if (*p == 0) {
      line = next;
      continue;
    }
    if (parse_hex16(&p, &vid) < 0) {
      devlist_warn(line_no, "bad VID");
      line = next;
      continue;
    }
    while (*p == ' ' || *p == '\t') p++;
    if (*p++ != ',') {
      devlist_warn(line_no, "bad format");
      line = next;
      continue;
    }
    if (parse_hex16(&p, &pid) < 0) {
      devlist_warn(line_no, "bad PID");
      line = next;
      continue;
    }
    while (*p == ' ' || *p == '\t') p++;
    if (*p++ != ',') {
      devlist_warn(line_no, "bad format");
      line = next;
      continue;
    }

    // the name runs up to the LAST comma so it may itself contain commas
    last_comma = NULL;
    for (scan = p; *scan; scan++) {
      if (*scan == ',') {
        last_comma = scan;
      }
    }
    if (last_comma == NULL) {
      devlist_warn(line_no, "missing XTYPE");
      line = next;
      continue;
    }
    *last_comma = 0;
    name = trim_field(p);
    xt = trim_field(last_comma + 1);

    if (strcasecmp(xt, "XTYPE_XBOX360") == 0) {
      xtype = XTYPE_XBOX360;
    } else if (strcasecmp(xt, "XTYPE_XBOX360W") == 0) {
      // XTYPE_XBOX360W selects the Microsoft 4-pad wireless receiver
      // protocol; 8BitDo/GameSir 2.4GHz dongles speak plain wired XInput.
      // treat non-Microsoft VIDs as wired to avoid the classic misconfig
      if (vid != 0x045e) {
        show_msg((char *)"XPAD Rev: XBOX360W is only for MS receivers, using XBOX360");
        xtype = XTYPE_XBOX360;
      } else {
        xtype = XTYPE_XBOX360W;
      }
    } else if (strcasecmp(xt, "PTYPE_PS4") == 0) {
      xtype = PTYPE_PS4;
    } else if (strcasecmp(xt, "PTYPE_PS5") == 0 || strcasecmp(xt, "PTYPE_DUALSENSE") == 0) {
      xtype = PTYPE_PS5;
    } else if (strcasecmp(xt, "XTYPE_XBOX") == 0 || strcasecmp(xt, "XTYPE_XBOXONE") == 0 ||
               strcasecmp(xt, "PTYPE_PS3") == 0 || strcasecmp(xt, "PTYPE_BT") == 0) {
      devlist_warn(line_no, "type not supported yet");
      line = next;
      continue;
    } else {
      devlist_warn(line_no, "unknown XTYPE");
      line = next;
      continue;
    }

    // registering the same VID/PID twice with different LDD ops is
    // undefined, so keep the first entry (built-ins win over the file).
    // built-in duplicates are skipped silently - listing an already
    // supported pad in the file is harmless
    if (builtin_known(vid, pid)) {
      line = next;
      continue;
    }
    if (extra_known(vid, pid)) {
      devlist_warn(line_no, "duplicate VID/PID");
      line = next;
      continue;
    }
    if (extra_count >= MAX_EXTRA_DEV_NUM) {
      devlist_warn(line_no, "list full");
      line = next;
      continue;
    }
    extra_info[extra_count].vid = vid;
    extra_info[extra_count].pid = pid;
    extra_info[extra_count].xtype = xtype;
    if (*name == 0) {
      name = (char *)"Custom controller";
    }
    strncpy(extra_info[extra_count].name, name, EXTRA_NAME_LEN - 1);
    extra_info[extra_count].name[EXTRA_NAME_LEN - 1] = 0;
    xlog_dev("xpad_devices.txt entry accepted", vid, pid, 0);
    extra_count++;
    line = next;
  }
  xlog_code("xpad_devices.txt entries loaded", extra_count);
}

static const char *remap_digital_names[REMAP_DIGITAL_COUNT] = {
  "CROSS", "CIRCLE", "TRIANGLE", "SQUARE",
  "R1", "R2", "R3", "L1", "L2", "L3",
  "DPAD_UP", "DPAD_LEFT", "DPAD_DOWN", "DPAD_RIGHT",
  "START", "SELECT", "PS", "TOUCHPAD"
};

static const char *remap_axis_names[REMAP_AXIS_COUNT] = {
  "ANALOG_LEFT_X", "ANALOG_LEFT_Y", "ANALOG_RIGHT_X", "ANALOG_RIGHT_Y"
};

static int32_t read_text_file(const char *path, char *buffer, uint32_t size) {
  int32_t fd, result;
  uint64_t nread = 0;

  if (size < 2 || cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED) {
    return(-1);
  }
  result = cellFsRead(fd, buffer, size - 1, &nread);
  cellFsClose(fd);
  if (result != CELL_FS_SUCCEEDED) {
    return(-1);
  }
  if (nread >= size) {
    nread = size - 1;
  }
  buffer[nread] = 0;
  return((int32_t)nread);
}

static void load_analog_config(void) {
  static char buffer[4096];

  xpad_analog_defaults(&analog_config);
  if (read_text_file(XPAD_ANALOG_PATH, buffer, sizeof(buffer)) < 0) {
    xlog("analog: no xpad_analog.txt, using safe DS3 defaults (80%, 4% deadzone)");
    return;
  }
  xpad_analog_parse_buffer(&analog_config, buffer);
  xlog_code("analog: saturation percent", analog_config.saturation);
  xlog_code("analog: radial deadzone percent", analog_config.deadzone);
}

static int32_t parse_decimal(char *text, int32_t *value) {
  char *p = trim_field(text);
  int32_t v = 0, digits = 0;

  while (*p >= '0' && *p <= '9') {
    v = v * 10 + (*p++ - '0');
    digits++;
  }
  while (*p == ' ' || *p == '\t' || *p == '\r') {
    p++;
  }
  if (digits == 0 || *p != 0) {
    return(-1);
  }
  *value = v;
  return(0);
}

static int32_t remap_find_digital(const char *name) {
  int32_t i;

  for (i = 0; i < REMAP_DIGITAL_COUNT; i++) {
    if (strcasecmp(name, remap_digital_names[i]) == 0) {
      return(i);
    }
  }
  /* Friendly XInput aliases. PS3 names remain canonical in the file. */
  if (strcasecmp(name, "A") == 0) return(REMAP_CROSS);
  if (strcasecmp(name, "B") == 0) return(REMAP_CIRCLE);
  if (strcasecmp(name, "Y") == 0) return(REMAP_TRIANGLE);
  if (strcasecmp(name, "X") == 0) return(REMAP_SQUARE);
  if (strcasecmp(name, "LB") == 0) return(REMAP_L1);
  if (strcasecmp(name, "LT") == 0) return(REMAP_L2);
  if (strcasecmp(name, "LS") == 0) return(REMAP_L3);
  if (strcasecmp(name, "RB") == 0) return(REMAP_R1);
  if (strcasecmp(name, "RT") == 0) return(REMAP_R2);
  if (strcasecmp(name, "RS") == 0) return(REMAP_R3);
  if (strcasecmp(name, "BACK") == 0) return(REMAP_SELECT);
  if (strcasecmp(name, "GUIDE") == 0 || strcasecmp(name, "HOME") == 0) return(REMAP_PS);
  return(-1);
}

static int32_t remap_find_axis(const char *name) {
  int32_t i;

  for (i = 0; i < REMAP_AXIS_COUNT; i++) {
    if (strcasecmp(name, remap_axis_names[i]) == 0) {
      return(i);
    }
  }
  if (strcasecmp(name, "LX") == 0) return(REMAP_AXIS_LEFT_X);
  if (strcasecmp(name, "LY") == 0) return(REMAP_AXIS_LEFT_Y);
  if (strcasecmp(name, "RX") == 0) return(REMAP_AXIS_RIGHT_X);
  if (strcasecmp(name, "RY") == 0) return(REMAP_AXIS_RIGHT_Y);
  return(-1);
}

static int32_t remap_invert_axis(const char *line) {
  if (strcasecmp(line, "INVERT_ANALOG_LEFT_X_AXIS") == 0) return(REMAP_AXIS_LEFT_X);
  if (strcasecmp(line, "INVERT_ANALOG_LEFT_Y_AXIS") == 0) return(REMAP_AXIS_LEFT_Y);
  if (strcasecmp(line, "INVERT_ANALOG_RIGHT_X_AXIS") == 0) return(REMAP_AXIS_RIGHT_X);
  if (strcasecmp(line, "INVERT_ANALOG_RIGHT_Y_AXIS") == 0) return(REMAP_AXIS_RIGHT_Y);
  return(-1);
}

static int32_t remap_profile_from_settings(void) {
  static char buffer[2048];
  char *line, *next, *comment, *equals, *key, *value;
  int32_t profile;

  if (read_text_file(XPAD_SETTINGS_PATH, buffer, sizeof(buffer)) < 0) {
    return(-1);
  }
  line = buffer;
  while (line != NULL && *line != 0) {
    if ((next = strchr(line, '\n')) != NULL) *next++ = 0;
    if ((comment = strchr(line, '#')) != NULL) *comment = 0;
    key = trim_field(line);
    if ((equals = strchr(key, '=')) != NULL) {
      *equals = 0;
      value = trim_field(equals + 1);
      key = trim_field(key);
      if (strcasecmp(key, "REMAP") == 0 && parse_decimal(value, &profile) == 0 &&
          profile >= 0 && profile <= 10) {
        return(profile);
      }
    }
    line = next;
  }
  return(-1);
}

static void remap_reset(void) {
  int32_t i;

  memset(&remap_config, 0, sizeof(remap_config));
  memset(remap_hotkey_down, 0, sizeof(remap_hotkey_down));
  for (i = 0; i < REMAP_DIGITAL_COUNT; i++) {
    remap_config.digital_map[i] = (uint8_t)i;
  }
  for (i = 0; i < REMAP_AXIS_COUNT; i++) {
    remap_config.axis_map[i] = (uint8_t)i;
  }
}

/* Compatible with the useful part of the official 0.8 xpad_remap.txt:
     [REMAP_SETTING_N]
     R1 = R2
     R2 = R1
   This build also accepts ACTIVE_PROFILE and START_ENABLED before the first
   section. If ACTIVE_PROFILE is omitted, REMAP=N from xpad_settings.txt is
   used. Empty mappings and comments are ignored. */
static void load_remap_config(void) {
  static char buffer[REMAP_FILE_SIZE];
  char *line, *next, *comment, *p, *equals, *key, *value, *section_end;
  int32_t profile, file_profile = -1, start_enabled = -1;
  int32_t section = 0, src, dst, axis, value_num;

  remap_reset();
  profile = remap_profile_from_settings();
  if (profile < 0) {
    profile = 0;
  }
  if (read_text_file(XPAD_REMAP_PATH, buffer, sizeof(buffer)) < 0) {
    xlog("remap: no xpad_remap.txt, fixed standard mapping active");
    return;
  }

  /* First pass: configuration directives may select the profile whose
     section the second pass will parse. */
  line = buffer;
  while (line != NULL && *line != 0) {
    if ((next = strchr(line, '\n')) != NULL) *next++ = 0;
    if ((comment = strchr(line, '#')) != NULL) *comment = 0;
    p = trim_field(line);
    if (*p == '[') {
      line = next;
      continue;
    }
    if ((equals = strchr(p, '=')) != NULL) {
      *equals = 0;
      key = trim_field(p);
      value = trim_field(equals + 1);
      if (strcasecmp(key, "ACTIVE_PROFILE") == 0 && parse_decimal(value, &value_num) == 0 &&
          value_num >= 0 && value_num <= 10) {
        file_profile = value_num;
      } else if (strcasecmp(key, "START_ENABLED") == 0 && parse_decimal(value, &value_num) == 0 &&
                 (value_num == 0 || value_num == 1)) {
        start_enabled = value_num;
      }
    }
    line = next;
  }
  if (file_profile >= 0) {
    profile = file_profile;
  }
  remap_config.profile = (uint8_t)profile;
  remap_config.start_enabled = (uint8_t)(start_enabled < 0 ? (profile > 0) : start_enabled);
  if (profile == 0) {
    xlog("remap: disabled by profile 0");
    return;
  }

  /* The first pass split the buffer in place, so reload it for the profile
     parser. */
  if (read_text_file(XPAD_REMAP_PATH, buffer, sizeof(buffer)) < 0) {
    return;
  }
  line = buffer;
  while (line != NULL && *line != 0) {
    if ((next = strchr(line, '\n')) != NULL) *next++ = 0;
    if ((comment = strchr(line, '#')) != NULL) *comment = 0;
    p = trim_field(line);
    if (*p == 0) {
      line = next;
      continue;
    }
    if (*p == '[') {
      section = 0;
      section_end = strchr(p + 1, ']');
      if (section_end != NULL) {
        *section_end = 0;
        key = trim_field(p + 1);
        if (strncasecmp(key, "REMAP_SETTING_", 14) == 0 &&
            parse_decimal(key + 14, &value_num) == 0) {
          section = value_num;
        }
      }
      line = next;
      continue;
    }
    if (section != profile) {
      line = next;
      continue;
    }

    axis = remap_invert_axis(p);
    if (axis >= 0) {
      remap_config.invert_axes |= (uint8_t)(1U << axis);
      remap_config.rules++;
      line = next;
      continue;
    }
    equals = strchr(p, '=');
    if (equals == NULL) {
      remap_config.rejected++;
      line = next;
      continue;
    }
    *equals = 0;
    key = trim_field(p);
    value = trim_field(equals + 1);
    if (*value == 0) {
      line = next;
      continue;
    }
    src = remap_find_digital(key);
    dst = remap_find_digital(value);
    if (src >= 0 && dst >= 0 && dst < REMAP_DIGITAL_OUTPUT_COUNT) {
      remap_config.digital_map[src] = (uint8_t)dst;
      remap_config.digital_explicit |= (uint32_t)(1U << src);
      remap_config.rules++;
      line = next;
      continue;
    }
    src = remap_find_axis(key);
    dst = remap_find_axis(value);
    if (src >= 0 && dst >= 0) {
      remap_config.axis_map[src] = (uint8_t)dst;
      remap_config.axis_explicit |= (uint8_t)(1U << src);
      remap_config.rules++;
      line = next;
      continue;
    }
    remap_config.rejected++;
    line = next;
  }

  remap_config.loaded = remap_config.rules > 0 ? 1 : 0;
  remap_config.enabled = (remap_config.loaded && remap_config.start_enabled) ? 1 : 0;
  xlog_code("remap: active profile", remap_config.profile);
  xlog_code("remap: rules loaded", remap_config.rules);
  if (remap_config.rejected > 0) {
    xlog_code("remap: invalid rules skipped", remap_config.rejected);
  }
}

static uint8_t remap_read_digital(const CellPadData *data, int32_t id, uint8_t extra_buttons) {
  switch (id) {
    case REMAP_CROSS: return((uint8_t)data->button[CELL_PAD_BTN_OFFSET_PRESS_CROSS]);
    case REMAP_CIRCLE: return((uint8_t)data->button[CELL_PAD_BTN_OFFSET_PRESS_CIRCLE]);
    case REMAP_TRIANGLE: return((uint8_t)data->button[CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE]);
    case REMAP_SQUARE: return((uint8_t)data->button[CELL_PAD_BTN_OFFSET_PRESS_SQUARE]);
    case REMAP_R1: return((uint8_t)data->button[CELL_PAD_BTN_OFFSET_PRESS_R1]);
    case REMAP_R2: return((uint8_t)data->button[CELL_PAD_BTN_OFFSET_PRESS_R2]);
    case REMAP_L1: return((uint8_t)data->button[CELL_PAD_BTN_OFFSET_PRESS_L1]);
    case REMAP_L2: return((uint8_t)data->button[CELL_PAD_BTN_OFFSET_PRESS_L2]);
    case REMAP_DPAD_UP: return((uint8_t)data->button[CELL_PAD_BTN_OFFSET_PRESS_UP]);
    case REMAP_DPAD_LEFT: return((uint8_t)data->button[CELL_PAD_BTN_OFFSET_PRESS_LEFT]);
    case REMAP_DPAD_DOWN: return((uint8_t)data->button[CELL_PAD_BTN_OFFSET_PRESS_DOWN]);
    case REMAP_DPAD_RIGHT: return((uint8_t)data->button[CELL_PAD_BTN_OFFSET_PRESS_RIGHT]);
    case REMAP_R3: return((data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] & CELL_PAD_CTRL_R3) ? 0xff : 0);
    case REMAP_L3: return((data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] & CELL_PAD_CTRL_L3) ? 0xff : 0);
    case REMAP_START: return((data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] & CELL_PAD_CTRL_START) ? 0xff : 0);
    case REMAP_SELECT: return((data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] & CELL_PAD_CTRL_SELECT) ? 0xff : 0);
    case REMAP_PS: return((data->button[0] & CELL_PAD_CTRL_LDD_PS) ? 0xff : 0);
    case REMAP_TOUCHPAD: return((extra_buttons & REMAP_EXTRA_TOUCHPAD) ? 0xff : 0);
  }
  return(0);
}

static void remap_clear_digital_source(CellPadData *data, int32_t id) {
  switch (id) {
    case REMAP_CROSS:
      data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] &= ~CELL_PAD_CTRL_CROSS;
      data->button[CELL_PAD_BTN_OFFSET_PRESS_CROSS] = 0;
      break;
    case REMAP_CIRCLE:
      data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] &= ~CELL_PAD_CTRL_CIRCLE;
      data->button[CELL_PAD_BTN_OFFSET_PRESS_CIRCLE] = 0;
      break;
    case REMAP_TRIANGLE:
      data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] &= ~CELL_PAD_CTRL_TRIANGLE;
      data->button[CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE] = 0;
      break;
    case REMAP_SQUARE:
      data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] &= ~CELL_PAD_CTRL_SQUARE;
      data->button[CELL_PAD_BTN_OFFSET_PRESS_SQUARE] = 0;
      break;
    case REMAP_R1:
      data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] &= ~CELL_PAD_CTRL_R1;
      data->button[CELL_PAD_BTN_OFFSET_PRESS_R1] = 0;
      break;
    case REMAP_R2:
      data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] &= ~CELL_PAD_CTRL_R2;
      data->button[CELL_PAD_BTN_OFFSET_PRESS_R2] = 0;
      break;
    case REMAP_R3:
      data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] &= ~CELL_PAD_CTRL_R3;
      break;
    case REMAP_L1:
      data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] &= ~CELL_PAD_CTRL_L1;
      data->button[CELL_PAD_BTN_OFFSET_PRESS_L1] = 0;
      break;
    case REMAP_L2:
      data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] &= ~CELL_PAD_CTRL_L2;
      data->button[CELL_PAD_BTN_OFFSET_PRESS_L2] = 0;
      break;
    case REMAP_L3:
      data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] &= ~CELL_PAD_CTRL_L3;
      break;
    case REMAP_DPAD_UP:
      data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] &= ~CELL_PAD_CTRL_UP;
      data->button[CELL_PAD_BTN_OFFSET_PRESS_UP] = 0;
      break;
    case REMAP_DPAD_LEFT:
      data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] &= ~CELL_PAD_CTRL_LEFT;
      data->button[CELL_PAD_BTN_OFFSET_PRESS_LEFT] = 0;
      break;
    case REMAP_DPAD_DOWN:
      data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] &= ~CELL_PAD_CTRL_DOWN;
      data->button[CELL_PAD_BTN_OFFSET_PRESS_DOWN] = 0;
      break;
    case REMAP_DPAD_RIGHT:
      data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] &= ~CELL_PAD_CTRL_RIGHT;
      data->button[CELL_PAD_BTN_OFFSET_PRESS_RIGHT] = 0;
      break;
    case REMAP_START:
      data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] &= ~CELL_PAD_CTRL_START;
      break;
    case REMAP_SELECT:
      data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] &= ~CELL_PAD_CTRL_SELECT;
      break;
    case REMAP_PS:
      data->button[0] &= ~CELL_PAD_CTRL_LDD_PS;
      break;
  }
}

static void remap_write_digital(CellPadData *data, int32_t id, uint8_t pressure) {
  uint16_t *digit0 = &data->button[0];
  uint16_t *digit1 = &data->button[CELL_PAD_BTN_OFFSET_DIGITAL1];
  uint16_t *digit2 = &data->button[CELL_PAD_BTN_OFFSET_DIGITAL2];

  if (pressure == 0) return;
  switch (id) {
    case REMAP_CROSS: *digit2 |= CELL_PAD_CTRL_CROSS; data->button[CELL_PAD_BTN_OFFSET_PRESS_CROSS] = pressure; break;
    case REMAP_CIRCLE: *digit2 |= CELL_PAD_CTRL_CIRCLE; data->button[CELL_PAD_BTN_OFFSET_PRESS_CIRCLE] = pressure; break;
    case REMAP_TRIANGLE: *digit2 |= CELL_PAD_CTRL_TRIANGLE; data->button[CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE] = pressure; break;
    case REMAP_SQUARE: *digit2 |= CELL_PAD_CTRL_SQUARE; data->button[CELL_PAD_BTN_OFFSET_PRESS_SQUARE] = pressure; break;
    case REMAP_R1: *digit2 |= CELL_PAD_CTRL_R1; data->button[CELL_PAD_BTN_OFFSET_PRESS_R1] = pressure; break;
    case REMAP_R2: *digit2 |= CELL_PAD_CTRL_R2; data->button[CELL_PAD_BTN_OFFSET_PRESS_R2] = pressure; break;
    case REMAP_R3: *digit1 |= CELL_PAD_CTRL_R3; break;
    case REMAP_L1: *digit2 |= CELL_PAD_CTRL_L1; data->button[CELL_PAD_BTN_OFFSET_PRESS_L1] = pressure; break;
    case REMAP_L2: *digit2 |= CELL_PAD_CTRL_L2; data->button[CELL_PAD_BTN_OFFSET_PRESS_L2] = pressure; break;
    case REMAP_L3: *digit1 |= CELL_PAD_CTRL_L3; break;
    case REMAP_DPAD_UP: *digit1 |= CELL_PAD_CTRL_UP; data->button[CELL_PAD_BTN_OFFSET_PRESS_UP] = pressure; break;
    case REMAP_DPAD_LEFT: *digit1 |= CELL_PAD_CTRL_LEFT; data->button[CELL_PAD_BTN_OFFSET_PRESS_LEFT] = pressure; break;
    case REMAP_DPAD_DOWN: *digit1 |= CELL_PAD_CTRL_DOWN; data->button[CELL_PAD_BTN_OFFSET_PRESS_DOWN] = pressure; break;
    case REMAP_DPAD_RIGHT: *digit1 |= CELL_PAD_CTRL_RIGHT; data->button[CELL_PAD_BTN_OFFSET_PRESS_RIGHT] = pressure; break;
    case REMAP_START: *digit1 |= CELL_PAD_CTRL_START; break;
    case REMAP_SELECT: *digit1 |= CELL_PAD_CTRL_SELECT; break;
    case REMAP_PS: *digit0 |= CELL_PAD_CTRL_LDD_PS; break;
  }
}

static void remap_apply(int32_t id, CellPadData *data, uint8_t extra_buttons) {
  uint8_t input[REMAP_DIGITAL_COUNT], output[REMAP_DIGITAL_OUTPUT_COUNT];
  uint8_t axis_input[REMAP_AXIS_COUNT], axis_output[REMAP_AXIS_COUNT];
  uint16_t digit1;
  int32_t i, target;
  uint8_t combo;

  if (id < 0 || id >= MAX_XPAD_NUM || data == NULL) {
    return;
  }

#if XPAD_MANUAL_GAME_LOADER
  /* cellPadGetData in the VSH process does not always receive LDD updates
     once a game owns the pad stream. Feed the loader hotkey from the exact
     normalized sample that is about to be inserted into the virtual port.
     This is the same in-game path already proven by the remap hotkey. */
  manual_loader_feed_normalized(id, data);
#endif

  /* Physical START+SELECT+DPAD_RIGHT toggles the selected profile. Detect
     before remapping and consume the combo while held so games never see a
     stray START/SELECT/right press. */
  digit1 = data->button[CELL_PAD_BTN_OFFSET_DIGITAL1];
  combo = ((digit1 & (CELL_PAD_CTRL_START | CELL_PAD_CTRL_SELECT | CELL_PAD_CTRL_RIGHT)) ==
           (CELL_PAD_CTRL_START | CELL_PAD_CTRL_SELECT | CELL_PAD_CTRL_RIGHT));
  if (combo) {
    if (!remap_hotkey_down[id]) {
      remap_hotkey_down[id] = 1;
      if (remap_config.loaded) {
        remap_config.enabled = remap_config.enabled ? 0 : 1;
        if (remap_config.enabled) {
          show_msg((char *)"XPAD Rev: remap enabled");
        } else {
          show_msg((char *)"XPAD Rev: remap disabled");
        }
      } else {
        show_msg((char *)"XPAD Rev: no remap profile loaded");
      }
    }
    if (remap_config.loaded) {
      data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] &=
        ~(CELL_PAD_CTRL_START | CELL_PAD_CTRL_SELECT | CELL_PAD_CTRL_RIGHT);
      data->button[CELL_PAD_BTN_OFFSET_PRESS_RIGHT] = 0;
    }
  } else {
    remap_hotkey_down[id] = 0;
  }
  if (!remap_config.enabled) {
    return;
  }

  /* Preserve every control that is not named as a physical source. Older
     remapper builds cleared and rebuilt the entire digital pad even for a
     profile containing only TOUCHPAD=SELECT. */
  memset(output, 0, sizeof(output));
  for (i = 0; i < REMAP_DIGITAL_COUNT; i++) {
    input[i] = remap_read_digital(data, i, extra_buttons);
  }
  for (i = 0; i < REMAP_DIGITAL_COUNT; i++) {
    if (!(remap_config.digital_explicit & (uint32_t)(1U << i))) {
      continue;
    }
    target = remap_config.digital_map[i];
    /* Multiple sources may feed one destination. Preserve the strongest
       pressure instead of making the result depend on rule order. */
    if (target >= 0 && target < REMAP_DIGITAL_OUTPUT_COUNT && input[i] > output[target]) {
      output[target] = input[i];
    }
  }
  /* A rule is a move. Remove only explicitly mapped physical sources, then
     merge their saved values into the requested destinations. TOUCHPAD has
     no native CellPadData destination and is therefore never cleared. */
  for (i = 0; i < REMAP_DIGITAL_OUTPUT_COUNT; i++) {
    if (remap_config.digital_explicit & (uint32_t)(1U << i)) {
      remap_clear_digital_source(data, i);
    }
  }
  for (i = 0; i < REMAP_DIGITAL_OUTPUT_COUNT; i++) {
    uint8_t passthrough;
    if (output[i] == 0) {
      continue;
    }
    passthrough = remap_read_digital(data, i, 0);
    if (passthrough > output[i]) {
      output[i] = passthrough;
    }
    remap_write_digital(data, i, output[i]);
  }

  axis_input[REMAP_AXIS_LEFT_X] = (uint8_t)data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X];
  axis_input[REMAP_AXIS_LEFT_Y] = (uint8_t)data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y];
  axis_input[REMAP_AXIS_RIGHT_X] = (uint8_t)data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X];
  axis_input[REMAP_AXIS_RIGHT_Y] = (uint8_t)data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y];
  /* Unmapped axes remain 1:1. Explicit sources are removed from their old
     destinations first, then written to their requested destinations. This
     makes a single LX=RX a move (LX becomes neutral) and reciprocal rules a
     clean swap. */
  memset(axis_output, 0x80, sizeof(axis_output));
  for (i = 0; i < REMAP_AXIS_COUNT; i++) {
    if (!(remap_config.axis_explicit & (1U << i))) {
      axis_output[i] = axis_input[i];
    }
  }
  for (i = 0; i < REMAP_AXIS_COUNT; i++) {
    if (remap_config.axis_explicit & (1U << i)) {
      target = remap_config.axis_map[i];
      if (target >= 0 && target < REMAP_AXIS_COUNT) {
        axis_output[target] = axis_input[i];
      }
    }
  }
  for (i = 0; i < REMAP_AXIS_COUNT; i++) {
    if (remap_config.invert_axes & (1U << i)) {
      axis_output[i] = (uint8_t)(0xff - axis_output[i]);
    }
  }
  data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X] = axis_output[REMAP_AXIS_LEFT_X];
  data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y] = axis_output[REMAP_AXIS_LEFT_Y];
  data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] = axis_output[REMAP_AXIS_RIGHT_X];
  data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] = axis_output[REMAP_AXIS_RIGHT_Y];
}

static void *_malloc(unsigned int size) {

  // vsh export for malloc
  if (!vsh_malloc) {
    vsh_malloc = getNIDfunc("allocator", 0x759E0635, 0);
  }
  if (vsh_malloc) {
    return vsh_malloc(size);
  }
  return(NULL);
}

static void _free(void *ptr) {

  // vsh export for free
  if (!vsh_free) {
    vsh_free = (void *)((int)getNIDfunc("allocator", 0x77A602DD, 0));
  }
  if (vsh_free) {
    vsh_free(ptr);
  }
}

static void block(sys_mutex_t mutex) {
  int32_t r;

  if ((r = sys_mutex_lock(mutex, 0)) != CELL_OK) {
    sys_ppu_thread_exit(0);
  }
}

static void unblock(sys_mutex_t mutex) {
  int32_t r;

  if ((r = sys_mutex_unlock(mutex)) != CELL_OK) {
    sys_ppu_thread_exit(0);
  }
}

static uint16_t viewer_buttons(uint16_t buttons) {
  uint16_t out = 0;
  if (buttons & btnA) out |= 0x0001;
  if (buttons & btnB) out |= 0x0002;
  if (buttons & btnX) out |= 0x0004;
  if (buttons & btnY) out |= 0x0008;
  if (buttons & btnShoulderLeft) out |= 0x0010;
  if (buttons & btnShoulderRight) out |= 0x0020;
  if (buttons & btnBack) out |= 0x0040;
  if (buttons & btnStart) out |= 0x0080;
  if (buttons & btnHatLeft) out |= 0x0100;
  if (buttons & btnHatRight) out |= 0x0200;
  if (buttons & btnDigiUp) out |= 0x0400;
  if (buttons & btnDigiDown) out |= 0x0800;
  if (buttons & btnDigiLeft) out |= 0x1000;
  if (buttons & btnDigiRight) out |= 0x2000;
  if (buttons & btnXbox) out |= 0x4000;
  return out;
}

static uint8_t viewer_axis_x(int16_t value) {
  /* Match xpad_read_report(): the USB report is little-endian while the PPU
     reads the packed 16-bit field in big-endian order. */
  return (uint8_t)((value - 0x80) & 0xff);
}

static uint8_t viewer_axis_y(int16_t value) {
  return (uint8_t)(((value ^ 0xff) - 0x80) & 0xff);
}

static void viewer_set_connected(int32_t id, uint8_t connected) {
  if (id < 0 || id >= MAX_XPAD_NUM) {
    return;
  }
  block(viewer_mutex);
  viewer_state[id].connected = connected ? 1 : 0;
  if (!connected) {
    remap_hotkey_down[id] = 0;
    viewer_state[id].buttons = 0;
    viewer_state[id].lt = 0;
    viewer_state[id].rt = 0;
    viewer_state[id].lx = 0x80;
    viewer_state[id].ly = 0x80;
    viewer_state[id].rx = 0x80;
    viewer_state[id].ry = 0x80;
  }
  unblock(viewer_mutex);
}

static void viewer_update_x360(int32_t id, const XBOX360_IN_REPORT *report) {
  if (id < 0 || id >= MAX_XPAD_NUM || report == NULL) {
    return;
  }
  block(viewer_mutex);
  viewer_state[id].buttons = viewer_buttons(report->buttons);
  viewer_state[id].lt = report->trigL;
  viewer_state[id].rt = report->trigR;
  viewer_state[id].lx = viewer_axis_x(report->left.x);
  viewer_state[id].ly = viewer_axis_y(report->left.y);
  viewer_state[id].rx = viewer_axis_x(report->right.x);
  viewer_state[id].ry = viewer_axis_y(report->right.y);
  viewer_state[id].connected = 1;
  unblock(viewer_mutex);
}

static void viewer_update_x360w(int32_t id, const XBOX360W_IN_REPORT *report) {
  if (id < 0 || id >= MAX_XPAD_NUM || report == NULL) {
    return;
  }
  block(viewer_mutex);
  viewer_state[id].buttons = viewer_buttons(report->buttons);
  viewer_state[id].lt = report->trigL;
  viewer_state[id].rt = report->trigR;
  viewer_state[id].lx = viewer_axis_x(report->left.x);
  viewer_state[id].ly = viewer_axis_y(report->left.y);
  viewer_state[id].rx = viewer_axis_x(report->right.x);
  viewer_state[id].ry = viewer_axis_y(report->right.y);
  viewer_state[id].connected = 1;
  unblock(viewer_mutex);
}

static void viewer_network_thread(uint64_t arg) {
  int sock = -1;
  int has_dest = 0;
  uint64_t last_discovery = 0;
  struct sockaddr_in dest;
  (void)arg;

  memset(&dest, 0, sizeof(dest));

  while (running) {
    int32_t i;
    sys_timer_usleep(1000 * 8); /* 125 Hz telemetry, independent of 500 Hz input. */
    if (sock < 0) {
      struct sockaddr_in local;
      sock = socket(AF_INET, SOCK_DGRAM, 0);
      if (sock < 0) {
        sys_timer_sleep(1);
        continue;
      }
      memset(&local, 0, sizeof(local));
      local.sin_family = AF_INET;
      local.sin_port = htons(VIEWER_DISCOVERY_PORT);
      local.sin_addr.s_addr = htonl(INADDR_ANY);
      if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        socketclose(sock);
        sock = -1;
        sys_timer_sleep(1);
        continue;
      }
    }
    {
      uint8_t discovery[32];
      struct sockaddr_in sender;
      socklen_t sender_len = sizeof(sender);
      int received;

      memset(&sender, 0, sizeof(sender));
      received = recvfrom(sock, discovery, sizeof(discovery), MSG_DONTWAIT,
                          (struct sockaddr *)&sender, &sender_len);
      if (received >= VIEWER_DISCOVERY_SIZE &&
          discovery[0] == 'X' && discovery[1] == 'P' &&
          discovery[2] == 'V' && discovery[3] == 'D' &&
          discovery[4] == 1 && sender.sin_family == AF_INET &&
          sender.sin_addr.s_addr != htonl(INADDR_ANY) && sender.sin_port != 0) {
        uint64_t now = time_now_usec();
        int same_pc = has_dest && dest.sin_addr.s_addr == sender.sin_addr.s_addr;
        if (!has_dest || same_pc || now - last_discovery > VIEWER_DISCOVERY_STALE_USEC) {
          int changed = !has_dest || dest.sin_addr.s_addr != sender.sin_addr.s_addr ||
                        dest.sin_port != sender.sin_port;
          dest = sender; /* Reply to the viewer's actual IP and UDP source port. */
          has_dest = 1;
          last_discovery = now;
          if (changed) {
            xlog("viewer: PC discovered; dynamic unicast telemetry enabled");
          }
        }
      }
    }
    if (has_dest && time_now_usec() - last_discovery > VIEWER_DISCOVERY_STALE_USEC) {
      has_dest = 0;
      xlog("viewer: PC discovery expired; waiting for a viewer announcement");
    }
    if (!has_dest) {
      continue;
    }
    for (i = 0; i < MAX_XPAD_NUM; i++) {
      uint8_t packet[VIEWER_PACKET_SIZE];
      VIEWER_STATE_t state;
      block(viewer_mutex);
      state = viewer_state[i];
      unblock(viewer_mutex);
      if (!state.connected) {
        continue;
      }
      memset(packet, 0, sizeof(packet));
      packet[0] = 'X'; packet[1] = 'P'; packet[2] = 'V'; packet[3] = '3';
      packet[4] = 1;
      packet[5] = (uint8_t)i;
      packet[6] = 1;
      packet[8] = (uint8_t)(state.buttons & 0xff);
      packet[9] = (uint8_t)(state.buttons >> 8);
      packet[10] = state.lt; packet[11] = state.rt;
      packet[12] = state.lx; packet[13] = state.ly;
      packet[14] = state.rx; packet[15] = state.ry;
      packet[16] = (uint8_t)(viewer_sequence[i] & 0xff);
      packet[17] = (uint8_t)(viewer_sequence[i] >> 8);
      viewer_sequence[i]++;
      sendto(sock, packet, sizeof(packet), MSG_DONTWAIT,
             (struct sockaddr *)&dest, sizeof(dest));
    }
  }
  if (sock >= 0) {
    socketclose(sock);
  }
  sys_ppu_thread_exit(0);
}

// take a 'pending' reference for an async USBD request about to be
// submitted; refused (returns 0) once the unit is being freed
static int32_t unit_submit_begin(XPAD_UNIT_t *unit) {
  int32_t ok = 0;

  block(unit->rb_mutex);
  if (!unit->closing) {
    unit->pending++;
    ok = 1;
  }
  unblock(unit->rb_mutex);
  return(ok);
}

// drop a 'pending' reference: after a completion callback is done with the
// unit, or when submitting the request failed (no callback will fire)
static void unit_submit_end(XPAD_UNIT_t *unit) {
  block(unit->rb_mutex);
  unit->pending--;
  unblock(unit->rb_mutex);
}

static void data_transfer_done(int32_t result, int32_t count, void *arg) {
  XPAD_UNIT_t *unit = (XPAD_UNIT_t *)arg;
  unsigned char *xpadbuf;
  int32_t log_first = 0, log_err = 0;
  uint16_t vid, pid;

  block(unit->rb_mutex);
  if (unit->closing) {
    unit->pending--;
    unblock(unit->rb_mutex);
    return;
  }
  if (result == CELL_OK) {
    if (!unit->dbg_first_in) {
      unit->dbg_first_in = 1;
      log_first = 1;
    }
  } else if (unit->dbg_err_count < 5) {
    // a failing or resetting device shows up here; cap the log lines so a
    // dying dongle cannot flood the log
    unit->dbg_err_count++;
    log_err = 1;
  }

  /* The worker consumes only the newest wired report. Remember press edges
     and trigger peaks that may have appeared and disappeared between two
     2 ms worker ticks so short inputs are still exposed for one update. */
  if (result == CELL_OK && unit->xtype == XTYPE_XBOX360 &&
      count >= (int32_t)sizeof(XBOX360_IN_REPORT)) {
    XBOX360_IN_REPORT *edge_report = (XBOX360_IN_REPORT *)unit->data;
    if (edge_report->header.command == inReport &&
        edge_report->header.size == sizeof(XBOX360_IN_REPORT)) {
      uint16_t buttons = edge_report->buttons;
      unit->pending_press_edges |= (buttons & ~unit->producer_buttons);
      unit->producer_buttons = buttons;
      if (edge_report->trigL > unit->pending_trig_l) {
        unit->pending_trig_l = edge_report->trigL;
      }
      if (edge_report->trigR > unit->pending_trig_r) {
        unit->pending_trig_r = edge_report->trigR;
      }
    }
  }

  /* Sony reports also use the newest-state path. Preserve quick face,
     shoulder, stick, PS and touchpad clicks (the d-pad's hat nibble is
     intentionally excluded because it is an encoded direction, not bits). */
  if (result == CELL_OK && (unit->xtype == PTYPE_PS4 || unit->xtype == PTYPE_PS5)) {
    uint8_t *raw = unit->data;
    uint8_t *buttons = NULL;
    uint8_t trig_l = 0, trig_r = 0;
    uint32_t button_bits;

    if (unit->xtype == PTYPE_PS4 && count >= 10 && raw[0] == 0x01) {
      buttons = &raw[5];
      trig_l = raw[8];
      trig_r = raw[9];
    } else if (unit->xtype == PTYPE_PS5 && count >= 12 && raw[0] == 0x01) {
      buttons = &raw[8];
      trig_l = raw[5];
      trig_r = raw[6];
    }
    if (buttons != NULL) {
      button_bits = (uint32_t)(buttons[0] & 0xf0) |
                    ((uint32_t)buttons[1] << 8) |
                    ((uint32_t)buttons[2] << 16);
      unit->pending_ps_press_edges |= button_bits & ~unit->producer_ps_buttons;
      unit->producer_ps_buttons = button_bits;
      if (trig_l > unit->pending_trig_l) unit->pending_trig_l = trig_l;
      if (trig_r > unit->pending_trig_r) unit->pending_trig_r = trig_r;
    }
  }

  /* For wired pads, prefer fresh state when the queue fills instead of
     discarding the new report and retaining stale movement. */
  if (result == CELL_OK && unit->xtype != XTYPE_XBOX360W &&
      unit->rblen >= RINGBUF_SIZE) {
    if (++unit->rp >= RINGBUF_SIZE) {
      unit->rp = 0;
    }
    unit->rblen--;
  }
  if (result == CELL_OK && unit->rblen < RINGBUF_SIZE) {
    xpadbuf = &unit->ringbuf[unit->wp][0];
    xpadbuf[0] = (unsigned char)(++unit->tcount & 0xFF);
    count = (count <= MAX_XPAD_DATA_LEN - 2) ? count : MAX_XPAD_DATA_LEN - 2;
    if (count < 0) {
      count = 0;
    }
    xpadbuf[1] = (unsigned char)(count & 0xFF); // store the clamped size, readers copy exactly this many bytes
    memcpy(&xpadbuf[2], unit->data, count);
    if (++unit->wp >= RINGBUF_SIZE) {
      unit->wp = 0;
    }
    unit->rblen++;
  }
  unblock(unit->rb_mutex);
  vid = unit->vid;
  pid = unit->pid;
  data_transfer(unit); // resubmit (takes its own reference)
  unit_submit_end(unit); // this callback's reference
  // log only after dropping the reference (file I/O can be slow and must
  // not extend the window unit_free() waits on); the unit may be freed by
  // now, hence the captured vid/pid locals
  if (log_first) {
    xlog_dev("first input report received from", vid, pid, count);
  }
  if (log_err) {
    xlog_dev("input transfer error on", vid, pid, result);
  }
}

static void data_transfer(XPAD_UNIT_t *unit) {
  int32_t r;
  uint16_t vid, pid;

  if (!unit_submit_begin(unit)) {
    return;
  }
  vid = unit->vid;
  pid = unit->pid;
  if ((r = cellUsbdInterruptTransfer(unit->i_pipe, unit->data, unit->payload, data_transfer_done, unit)) != CELL_OK) {
    unit_submit_end(unit);
    xlog_dev("input transfer submit failed for", vid, pid, r); // after the ref is dropped, see above
  }
}

static void set_interface_done(int32_t result, int32_t count, void *arg) {
  XPAD_UNIT_t *unit = (XPAD_UNIT_t *)arg;
  (void)count;
  xlog_dev("set_interface completed for", unit->vid, unit->pid, result);
  if (!unit->closing) {
    data_transfer(unit);
  }
  unit_submit_end(unit);
}

static void set_config_done(int32_t result, int32_t count, void *arg) {
  XPAD_UNIT_t *unit = (XPAD_UNIT_t *)arg;
  (void)count;
  xlog_dev("set_configuration completed for", unit->vid, unit->pid, result);
  if (unit->closing) {
    unit_submit_end(unit);
    return;
  }
  if (unit->as > 0) {
    if (unit_submit_begin(unit) &&
        cellUsbdSetInterface(unit->c_pipe, unit->ifnum, unit->as, set_interface_done, unit) != CELL_OK) {
      unit_submit_end(unit);
    }
  } else {
    data_transfer(unit);
  }

  // some 2.4GHz dongles only start streaming input after receiving an
  // LED packet once configured, so re-send the assigned LED here
  if (unit->number >= 0 && handle[unit->number] >= 0 && unit->last_port >= 0) {
    unit->set_led(unit, xpad_led[unit->last_port % 4]);
  }
  unit_submit_end(unit);
}

static void unit_free(XPAD_UNIT_t *unit) {
  int32_t i;

  if (unit) {
    block(unit->rb_mutex);
    unit->closing = 1;
    unblock(unit->rb_mutex);

    // closing the pipes cancels outstanding transfers so their completion
    // callbacks (which see 'closing' and bail out) fire promptly, and
    // releases the pipe resources this driver used to leak on shutdown
    if (unit->i_pipe >= 0) {
      cellUsbdClosePipe(unit->i_pipe);
    }
    if (unit->o_pipe >= 0) {
      cellUsbdClosePipe(unit->o_pipe);
    }
    if (unit->c_pipe >= 0) {
      cellUsbdClosePipe(unit->c_pipe);
    }

    // wait (bounded) for queued completion callbacks to finish with the
    // unit before freeing it; if USBD drops canceled callbacks entirely
    // this exits early via pending == 0. The bound is generous because a
    // callback can be blocked on a debug-log write to the HDD.
    for (i = 0; i < 500 && unit->pending > 0; i++) {
      sys_timer_usleep(1000);
    }

    // barrier: a callback that just dropped the last reference may still
    // be inside rb_mutex; taking it once ensures the handover completed
    block(unit->rb_mutex);
    unblock(unit->rb_mutex);
    sys_mutex_destroy(unit->rb_mutex);
    _free(unit);
  }
}

// return this unit's pad slot to the pool; callers must hold xpad_mutex
static void release_pad_slot(XPAD_UNIT_t *unit) {
  if (unit->number >= 0 && unit->number < MAX_XPAD_NUM) {
    if (XPAD.pad_unit[unit->number] == unit) {
      XPAD.pad_unit[unit->number] = NULL;
      XPAD.n--;
    }
    unit->number = -1;
  }
}

// release the unit-list entry reserved by unit_alloc() and free the unit;
// for attach error paths (callers must NOT hold xpad_mutex)
static void unit_release(XPAD_UNIT_t *unit) {
  if (unit) {
    block(xpad_mutex);
    if (unit->unit_idx >= 0 && unit->unit_idx < MAX_UNIT_NUM && XPAD.units[unit->unit_idx] == unit) {
      XPAD.units[unit->unit_idx] = NULL;
    }
    release_pad_slot(unit);
    unblock(xpad_mutex);
    unit_free(unit);
  }
}

static XPAD_UNIT_t *unit_alloc(int32_t dev_id, int32_t payload, uint8_t ifnum, uint8_t as, uint8_t xtype) {
  XPAD_UNIT_t *unit;
  int32_t i;
  sys_mutex_attribute_t mutex_attr;

  if ((unit = (XPAD_UNIT_t *)_malloc(sizeof(XPAD_UNIT_t) + payload)) != NULL) {
    memset(unit, 0, sizeof(XPAD_UNIT_t));
    unit->dev_id = dev_id;
    unit->c_pipe = -1; // so unit_free() only closes pipes this unit opened
    unit->i_pipe = -1;
    unit->o_pipe = -1;
    unit->payload = payload;
    unit->ifnum = ifnum;
    unit->as = as;
    unit->tcount = 0;
    unit->rp = 0;
    unit->wp = 0;
    unit->rblen = 0;
    unit->xtype = xtype;
    if (xtype == XTYPE_XBOX360) {
      unit->read_input = xpad_read_input;
      unit->set_led = xpad_set_led;
      unit->set_rumble = xpad_set_rumble;
    } else if (xtype == XTYPE_XBOX360W) {
      unit->read_input = xpadw_read_input;
      unit->set_led = xpadw_set_led;
      unit->set_rumble = xpadw_set_rumble;
    } else if (xtype == PTYPE_PS4 || xtype == PTYPE_PS5) {
      unit->read_input = pspad_read_input;
      unit->set_led = pspad_set_led;
      unit->set_rumble = pspad_set_rumble;
    }
    sys_mutex_attribute_initialize(mutex_attr);
    if (sys_mutex_create(&unit->rb_mutex, &mutex_attr) != CELL_OK) {
      _free(unit);
      return(NULL);
    }

    // claim the first free unit-list entry; fail cleanly when full.
    // no pad slot is claimed here - that happens in
    // register_ldd_controller() when the pad is actually usable
    unit->number = -1;
    unit->last_port = -1;
    block(xpad_mutex);
    for (i = 0; i < MAX_UNIT_NUM; i++) {
      if (XPAD.units[i] == NULL) {
        break;
      }
    }
    if (i >= MAX_UNIT_NUM) {
      unblock(xpad_mutex);
      unit_free(unit);
      return(NULL);
    }
    unit->unit_idx = i;
    XPAD.units[i] = unit; // reserve the entry so a concurrent alloc cannot claim it
    unblock(xpad_mutex);
  }
  return(unit);
}

// callers must hold xpad_mutex
static int32_t register_ldd_controller(XPAD_UNIT_t *unit) {
  uint8_t data[0x114];
  int32_t port, s, r;
  uint32_t capability, mode, port_setting;

  // claim a pad slot on first registration; endpoint units (e.g. the four
  // per wireless receiver) exist without a slot until a pad is really there
  if (unit->number < 0) {
    for (s = 0; s < MAX_XPAD_NUM; s++) {
      if (XPAD.pad_unit[s] == NULL) {
        break;
      }
    }
    if (s >= MAX_XPAD_NUM) {
      return(-1); // no free pad slot
    }
    XPAD.pad_unit[s] = unit;
    unit->number = s;
    XPAD.n++;
  }

  // register ldd controller with custom device capability
  if (handle[unit->number] < 0) {
    capability = 0xFFFF; // CELL_PAD_CAPABILITY_PS3_CONFORMITY | CELL_PAD_CAPABILITY_PRESS_MODE | CELL_PAD_CAPABILITY_HP_ANALOG_STICK | CELL_PAD_CAPABILITY_ACTUATOR;
    sys_pad_dbg_ldd_register_controller(data, (int32_t *)&(handle[unit->number]), 5, (uint32_t)capability << 1);
    //handle[unit->number] = cellPadLddRegisterController();
    sys_timer_usleep(1000*10); // allow some time for ps3 to register ldd controller
    if (handle[unit->number] < 0) {
      r = handle[unit->number];
      release_pad_slot(unit);
      return(r);
    }

    // all pad data into games
    mode = CELL_PAD_LDD_INSERT_DATA_INTO_GAME_MODE_ON; // = (1)
    sys_pad_dbg_ldd_set_data_insert_mode((int32_t)handle[unit->number], 0x100, (uint32_t *)&mode, 4);

    // set press and sensor mode on
    port_setting = CELL_PAD_SETTING_PRESS_ON | CELL_PAD_SETTING_SENSOR_ON;
    port = cellPadLddGetPortNo(handle[unit->number]);
    if (port < 0) {
      return(port);
    }
    cellPadSetPortSetting(port, port_setting);

    // set Xbox led corresponding to port number; with more than 4 pads two
    // controllers will show the same LED pattern (there are only 4 LEDs)
    unit->last_port = port;
    unit->set_led(unit, xpad_led[port%4]);
  }
  return(CELL_PAD_OK);
}

// callers must hold xpad_mutex
static int32_t unregister_ldd_controller(XPAD_UNIT_t *unit) {
  int32_t r = CELL_PAD_OK;

  if (unit->number >= 0) {
    viewer_set_connected(unit->number, 0);
  }
  if (unit->number >= 0 && handle[unit->number] >= 0) {
    if ((r = cellPadLddUnregisterController(handle[unit->number])) == CELL_OK) {
      //xpad_set_led(unit->number, xpad_led[ledBlinkingAll]);
      handle[unit->number] = -1;
    }
    // if unregistering failed, keep the handle so the next unit claiming
    // this slot adopts the still-registered LDD pad instead of leaking it
  }

  // always release the slot - the unit may be about to be freed and
  // XPAD.pad_unit[] must never point at a freed unit
  release_pad_slot(unit);
  unit->last_port = -1;
  return(r);
}

// create the named event queue the game-process plugin sends rumble events
// to. Best-effort: if it cannot be created the plugin keeps working, just
// without rumble (input is unaffected).
static int32_t create_rumble_queue(void) {
  sys_event_queue_attribute_t attr;
  int32_t r;

  rumble_queue_ready = 0;
  sys_event_queue_attribute_initialize(attr);
  r = sys_event_queue_create(&rumble_queue, &attr, RUMBLE_IPC_KEY, RUMBLE_QUEUE_DEPTH);
  if (r != CELL_OK) {
    xlog_code("rumble IPC: named event queue create failed", r);
    return(r);
  }
  rumble_queue_ready = 1;
  xlog("rumble IPC: named event queue ready");
  return(CELL_OK);
}

static void destroy_rumble_queue(void) {
  if (rumble_queue_ready) {
    rumble_queue_ready = 0;
    // SYS_EVENT_QUEUE_DESTROY_FORCE tears the queue down even if the game
    // side still has a port connected (e.g. plugin unloaded mid-game)
    sys_event_queue_destroy(rumble_queue, SYS_EVENT_QUEUE_DESTROY_FORCE);
  }
}

// deliver an actuator value to the pad currently assigned to 'port'.
// 'port' is a libpad port number: the game side reads it straight from the
// cellPadSetActDirect(port_no, ...) call it hooks, and unit->last_port is the
// same value read back here via cellPadLddGetPortNo(). Both name the one
// system-wide libpad pad array - an LDD (virtual) pad is inserted into that
// array and addressed by its index, so cellPadLddGetPortNo() returns exactly
// the index cellPadSetActDirect() uses. There is no separate LDD port
// namespace for the two to drift against, so this equality match is correct
// by construction (verified against the libpad LDD semantics in RPCS3).
static void route_rumble(int32_t port, uint8_t lval, uint8_t rval) {
  int32_t i;
  XPAD_UNIT_t *unit;

  block(xpad_mutex);
  for (i = 0; i < MAX_UNIT_NUM; i++) {
    if ((unit = XPAD.units[i]) != NULL && unit->number >= 0 &&
        handle[unit->number] >= 0 && unit->last_port == port && unit->set_rumble) {
      unit->set_rumble(unit, lval, rval);
      break; // ports are unique per pad
    }
  }
  unblock(xpad_mutex);
}

static uint64_t game_ready_last_notice;
static uint64_t game_ready_last_file_poll;

static void report_game_ready(int32_t hook_count, uint32_t process_id,
                              const char *source) {
  uint64_t now = time_now_usec();

#if XPAD_MANUAL_GAME_LOADER
  /* An older game module may acknowledge without a PID. While a manual load
     is pending, its requested PID is the only possible source. */
  if (process_id == 0) process_id = game_loader_attempted_pid;
  if (process_id != 0) {
    game_loader_active_pid = process_id;
    game_loader_attempted_pid = process_id;
  }
#endif

  /* An acknowledgement can arrive through IPC and the marker almost at the
     same time. Collapse the pair into one toast, but allow a later reload. */
  if (game_ready_last_notice != 0 &&
      now - game_ready_last_notice < 2000000ULL) return;
  game_ready_last_notice = now;
  if (hook_count >= 4) {
    show_msg((char *)"XPAD Rev: game module active");
    xlog(source);
  } else {
    show_msg((char *)"XPAD Rev: game module loaded with hook error");
    xlog_code(source, hook_count);
  }
}

/* HEN-safe fallback for builds where syscall 140 refuses a cross-process
   event-port connection. It runs only four times a second and performs no
   disk access unless the marker exists. */
static void poll_game_ready_file(void) {
  XPAD_GAME_READY_FILE_t ready;
  uint64_t now = time_now_usec(), nread = 0;
  int32_t fd, r;

  if (game_ready_last_file_poll != 0 &&
      now - game_ready_last_file_poll < 250000ULL) return;
  game_ready_last_file_poll = now;
  memset(&ready, 0, sizeof(ready));
  r = cellFsOpen(XPAD_GAME_READY_PATH, CELL_FS_O_RDONLY, &fd, NULL, 0);
  if (r != CELL_FS_SUCCEEDED) return;
  r = cellFsRead(fd, &ready, sizeof(ready), &nread);
  cellFsClose(fd);
  cellFsUnlink(XPAD_GAME_READY_PATH);
  if (r != CELL_FS_SUCCEEDED || nread < XPAD_GAME_READY_V1_SIZE ||
      ready.magic != XPAD_GAME_READY_MAGIC) {
    xlog("game ready marker: invalid or incomplete; ignored");
    return;
  }
  report_game_ready((int32_t)ready.hook_count,
                    nread >= sizeof(ready) ? ready.process_id : 0,
                    "game marker: Bluetooth/rumble module loaded, all hooks active");
}

// drain any rumble events the game plugin has posted since the last tick and
// forward them to the USB pads. Non-blocking so the poll loop never stalls.
static void drain_rumble_queue(void) {
  sys_event_t ev[RUMBLE_DRAIN_MAX];
  int32_t n = 0, i;

  if (!rumble_queue_ready) {
    return;
  }
  if (sys_event_queue_tryreceive(rumble_queue, ev, RUMBLE_DRAIN_MAX, &n) != CELL_OK) {
    return;
  }
  for (i = 0; i < n; i++) {
    if (ev[i].data1 == XPAD_EV_GAME_READY) {
      report_game_ready((int32_t)ev[i].data2, (uint32_t)ev[i].data3,
                        "game IPC: Bluetooth/rumble module loaded, all hooks active");
      continue;
    }
    int32_t port = RUMBLE_EV_PORT(ev[i].data1);
    uint8_t lval = RUMBLE_EV_LVAL(ev[i].data2);
    uint8_t rval = RUMBLE_EV_RVAL(ev[i].data3);
    route_rumble(port, lval, rval);
  }
}

#if XPAD_MANUAL_GAME_LOADER
/* -------------------------------------------------------------------------
 * Explicit, controller-driven game-module loader.
 *
 * This is deliberately not auto-injection. The user must hold
 * SELECT+L3+R3 after the game is already running. That reproduces the safe
 * timing of a manual webMAN Game Plugins load while removing the PC/PID step.
 * The potentially blocking LOAD_PROC_MODULE call runs on a disposable thread
 * so it can never stall the 2 ms USB input worker.
 * ------------------------------------------------------------------------- */
#define GAME_LOADER_PAD_POLL_USEC 10000ULL
#define GAME_LOADER_PID_POLL_USEC 500000ULL
#define GAME_LOADER_HOLD_USEC     800000ULL
#define GAME_LOADER_HANG_USEC   15000000ULL

/* Reliable path for controllers owned by XPAD Revolution (XInput and
   DS4/DualSense over USB). remap_apply() calls this with the physical,
   normalized state before remapping. It only publishes a tiny request flag;
   process discovery and LOAD_PROC_MODULE remain outside xpad_mutex. */
static void manual_loader_feed_normalized(int32_t id, const CellPadData *data) {
  const uint16_t combo = CELL_PAD_CTRL_SELECT | CELL_PAD_CTRL_L3 | CELL_PAD_CTRL_R3;
  uint16_t digit1;
  uint64_t now;

  if (id < 0 || id >= MAX_XPAD_NUM || data == NULL) return;
  digit1 = data->button[CELL_PAD_BTN_OFFSET_DIGITAL1];
  if ((digit1 & combo) == combo) {
    now = time_now_usec();
    if (game_loader_usb_combo_since[id] == 0) {
      game_loader_usb_combo_since[id] = now;
    }
    if (!game_loader_usb_combo_latched[id] &&
        now - game_loader_usb_combo_since[id] >= GAME_LOADER_HOLD_USEC) {
      game_loader_usb_combo_latched[id] = 1;
      game_loader_usb_request_port = (uint8_t)(id + 1);
    }
  } else {
    game_loader_usb_combo_since[id] = 0;
    game_loader_usb_combo_latched[id] = 0;
  }
}

static int32_t loader_ascii_equal_ci(char a, char b) {
  if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
  if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
  return(a == b);
}

static int32_t loader_contains_ci(const char *text, const char *needle) {
  const char *p, *q, *n;

  if (text == NULL || needle == NULL || *needle == 0) return(0);
  for (p = text; *p != 0; p++) {
    q = p;
    n = needle;
    while (*q != 0 && *n != 0 && loader_ascii_equal_ci(*q, *n)) {
      q++;
      n++;
    }
    if (*n == 0) return(1);
  }
  return(0);
}

static int32_t loader_name_is_system(const char *name) {
  if (name == NULL || *name == 0) return(1);
  return(loader_contains_ci(name, "vsh") ||
         loader_contains_ci(name, "sys_") ||
         loader_contains_ci(name, "default.self"));
}

static int32_t loader_get_all_pids(uint32_t *pids) {
  memset(pids, 0, sizeof(uint32_t) * PS3MAPI_MAX_PROC);
  system_call_3(8, SYSCALL8_OPCODE_PS3MAPI,
                PS3MAPI_OPCODE_GET_ALL_PROC_PID,
                (uint64_t)(uint32_t)pids);
  return((int32_t)p1);
}

static int32_t loader_get_proc_name(uint32_t pid, char *name) {
  name[0] = 0;
  system_call_4(8, SYSCALL8_OPCODE_PS3MAPI,
                PS3MAPI_OPCODE_GET_PROC_NAME_BY_PID,
                (uint64_t)pid, (uint64_t)(uint32_t)name);
  return((int32_t)p1);
}

static int32_t loader_load_module(uint32_t pid, const char *path) {
  system_call_6(8, SYSCALL8_OPCODE_PS3MAPI,
                PS3MAPI_OPCODE_LOAD_PROC_MODULE,
                (uint64_t)pid, (uint64_t)(uint32_t)path, 0, 0);
  return((int32_t)p1);
}

static int32_t loader_pid_present(uint32_t pid, const uint32_t *pids) {
  int32_t i;

  for (i = 0; i < PS3MAPI_MAX_PROC; i++) {
    if (pids[i] == pid) return(1);
  }
  return(0);
}

/* A retail or homebrew game process exposes an EBOOT path through PS3MAPI.
   Require that marker instead of guessing among arbitrary non-system PIDs. */
static uint32_t loader_find_game_process(char *selected_name) {
  uint32_t pids[PS3MAPI_MAX_PROC];
  uint32_t selected = 0;
  char name[256];
  int32_t i, r;

  selected_name[0] = 0;
  r = loader_get_all_pids(pids);
  if (r < 0) {
    xlog_code("manual loader: get_all_pids failed", r);
    return(0);
  }
  for (i = 0; i < PS3MAPI_MAX_PROC; i++) {
    if (pids[i] == 0) continue;
    if (loader_get_proc_name(pids[i], name) < 0) continue;
    name[sizeof(name) - 1] = 0;
    if (loader_name_is_system(name) || !loader_contains_ci(name, "EBOOT")) continue;

    /* More than one EBOOT is unusual after a game has settled. Prefer the
       newest/highest PID, which is the process webMAN presents as current. */
    if (selected == 0 || pids[i] > selected) {
      selected = pids[i];
      strncpy(selected_name, name, 255);
      selected_name[255] = 0;
    }
  }
  return(selected);
}

static void loader_show_error(const char *prefix, int32_t code) {
  char msg[112], *m = msg;

  m = append_str(m, prefix);
  m = append_str(m, " (0x");
  m = append_hex32(m, (uint32_t)code);
  append_str(m, ")");
  show_msg(msg);
}

static void manual_game_loader_thread(uint64_t arg) {
  uint32_t pid = (uint32_t)arg;
  char name[256];
  int32_t r;

  name[0] = 0;
  r = loader_get_proc_name(pid, name);
  name[sizeof(name) - 1] = 0;
  if (r < 0 || loader_name_is_system(name) ||
      !loader_contains_ci(name, "EBOOT")) {
    xlog_code("manual loader: selected process disappeared or changed", r);
    game_loader_attempted_pid = 0;
    game_loader_inflight = 0;
    show_msg((char *)"XPAD Rev: game process disappeared; try again");
    sys_ppu_thread_exit(0);
    return;
  }

  xlog_proc("manual loader: loading xpad_game.sprx into", name, pid);
  game_loader_call_t0 = time_now_usec();
  r = loader_load_module(pid, XPAD_GAME_SPRX_PATH);
  game_loader_call_t0 = 0;
  game_loader_inflight = 0;
  if (r < 0) {
    game_loader_attempted_pid = 0;
    xlog_code("manual loader: load_module failed", r);
    loader_show_error("XPAD Rev: game module load failed", r);
  } else {
    xlog_proc("manual loader: LOAD_PROC_MODULE returned OK for", name, pid);
    /* Final success is announced only by xpad_game's ready marker/IPC after
       all hooks have actually run. Keep attempted_pid set to prevent a
       duplicate load if that acknowledgement is delayed or unavailable. */
  }
  sys_ppu_thread_exit(0);
}

static void request_manual_game_load(void) {
  sys_ppu_thread_t loader_thread;
  char name[256];
  uint32_t pid;
  int32_t r;

  if (game_loader_inflight) {
    show_msg((char *)"XPAD Rev: game module load already in progress");
    return;
  }
  pid = loader_find_game_process(name);
  if (pid == 0) {
    show_msg((char *)"XPAD Rev: no running game process found");
    xlog("manual loader: hotkey pressed but no EBOOT process was found");
    return;
  }
  if (game_loader_active_pid == pid) {
    show_msg((char *)"XPAD Rev: game module already active");
    return;
  }
  if (game_loader_attempted_pid == pid) {
    show_msg((char *)"XPAD Rev: game module was already requested");
    return;
  }

  game_loader_attempted_pid = pid;
  game_loader_inflight = 1;
  game_loader_hang_logged = 0;
  show_msg((char *)"XPAD Rev: loading game module...");
  r = sys_ppu_thread_create(&loader_thread, manual_game_loader_thread,
                            (uint64_t)pid, 1000, 0x4000,
                            0,
                            MANUAL_LOADER_THREAD_NAME);
  if (r != CELL_OK) {
    game_loader_inflight = 0;
    game_loader_attempted_pid = 0;
    xlog_code("manual loader: thread create failed", r);
    loader_show_error("XPAD Rev: loader thread failed", r);
  }
}

static int32_t manual_loader_combo_down(void) {
  CellPadInfo2 info;
  CellPadData data;
  uint16_t combo = CELL_PAD_CTRL_SELECT | CELL_PAD_CTRL_L3 | CELL_PAD_CTRL_R3;
  uint16_t old_d1, old_d2, new_d1, new_d2;
  uint32_t connected_mask = 0;
  int32_t port, r, init_r, down = 0;

  memset(&info, 0, sizeof(info));
  r = cellPadGetInfo2(&info);
  if (r == CELL_PAD_ERROR_UNINITIALIZED && !game_loader_libpad_init_attempted) {
    game_loader_libpad_init_attempted = 1;
    init_r = cellPadInit(MAX_XPAD_NUM);
    xlog_code("manual loader: libpad init attempt", init_r);
    memset(&info, 0, sizeof(info));
    r = cellPadGetInfo2(&info);
  }
  if (r != CELL_PAD_OK) {
    if (!game_loader_libpad_probe_logged) {
      game_loader_libpad_probe_logged = 1;
      xlog_code("manual loader: cellPadGetInfo2 failed", r);
    }
    return(0);
  }
  for (port = 0; port < MAX_XPAD_NUM; port++) {
    if (info.port_status[port] & CELL_PAD_STATUS_CONNECTED) {
      connected_mask |= (1U << port);
    }
  }
  if (!game_loader_libpad_probe_logged) {
    game_loader_libpad_probe_logged = 1;
    xlog_code("manual loader: libpad monitoring ready, connected mask", (int32_t)connected_mask);
  }
  for (port = 0; port < MAX_XPAD_NUM; port++) {
    if (!(info.port_status[port] & CELL_PAD_STATUS_CONNECTED)) {
      game_loader_pad_d1[port] = 0;
      game_loader_pad_d2[port] = 0;
      continue;
    }
    memset(&data, 0, sizeof(data));
    r = cellPadGetData((uint32_t)port, &data);
    if (r == CELL_PAD_OK && data.len > 0) {
      old_d1 = game_loader_pad_d1[port];
      old_d2 = game_loader_pad_d2[port];
      new_d1 = data.button[CELL_PAD_BTN_OFFSET_DIGITAL1];
      new_d2 = data.button[CELL_PAD_BTN_OFFSET_DIGITAL2];
      game_loader_pad_d1[port] = new_d1;
      game_loader_pad_d2[port] = new_d2;
      /* At most twelve breadcrumbs per boot. Packed code is
         0x00PPDDDD: one-based port, DIGITAL1, DIGITAL2. */
      if ((new_d1 & combo) != 0 && (new_d1 != old_d1 || new_d2 != old_d2) &&
          game_loader_libpad_button_logs < 12) {
        game_loader_libpad_button_logs++;
        xlog_code("manual loader: libpad target-button sample",
                  (int32_t)(((uint32_t)(port + 1) << 16) |
                            ((uint32_t)(new_d1 & 0xff) << 8) |
                            (uint32_t)(new_d2 & 0xff)));
      }
    }
    if ((game_loader_pad_d1[port] & combo) == combo) {
      down = 1;
    }
  }
  return(down);
}

static void poll_manual_game_loader(void) {
  uint64_t now = time_now_usec();
  uint32_t tracked, pids[PS3MAPI_MAX_PROC];
  uint8_t usb_port;

  /* Forget the per-process guard once that game exits, including when a later
     title reuses the same numeric PID. The XMB gap is much longer than 0.5 s. */
  if (!game_loader_inflight &&
      (game_loader_active_pid != 0 || game_loader_attempted_pid != 0) &&
      (game_loader_last_pid_poll == 0 ||
       now - game_loader_last_pid_poll >= GAME_LOADER_PID_POLL_USEC)) {
    game_loader_last_pid_poll = now;
    tracked = game_loader_active_pid != 0 ?
              game_loader_active_pid : game_loader_attempted_pid;
    if (loader_get_all_pids(pids) >= 0 && !loader_pid_present(tracked, pids)) {
      xlog_code("manual loader: game process exited; loader re-armed for next game",
                (int32_t)tracked);
      game_loader_active_pid = 0;
      game_loader_attempted_pid = 0;
    }
  }

  if (game_loader_inflight && !game_loader_hang_logged &&
      game_loader_call_t0 != 0 &&
      now - game_loader_call_t0 > GAME_LOADER_HANG_USEC) {
    game_loader_hang_logged = 1;
    xlog("manual loader: LOAD_PROC_MODULE stuck >15s; input thread still running");
  }

  usb_port = game_loader_usb_request_port;
  if (usb_port != 0) {
    game_loader_usb_request_port = 0;
    game_loader_combo_latched = 1; /* suppress a simultaneous libpad copy */
    game_loader_combo_since = now;
    xlog_code("manual loader: normalized USB combo accepted on port", usb_port);
    request_manual_game_load();
  }

  if (game_loader_last_pad_poll != 0 &&
      now - game_loader_last_pad_poll < GAME_LOADER_PAD_POLL_USEC) return;
  game_loader_last_pad_poll = now;

  if (manual_loader_combo_down()) {
    if (game_loader_combo_since == 0) game_loader_combo_since = now;
    if (!game_loader_combo_latched &&
        now - game_loader_combo_since >= GAME_LOADER_HOLD_USEC) {
      game_loader_combo_latched = 1;
      request_manual_game_load();
    }
  } else {
    game_loader_combo_since = 0;
    game_loader_combo_latched = 0;
  }
}
#endif

#if XPAD_AUTO_INJECT
// --- PS3MAPI auto-injection of the game-process plugin --------------------
// Loads xpad_game.sprx into running game processes so rumble needs no per-game
// setup. Best-effort: any failure just means that game has no rumble; input is
// unaffected. Compiled out until the PS3MAPI ABI is confirmed (see the
// XPAD_AUTO_INJECT note above and TODO.md section 8).

static uint32_t injected_pids[PS3MAPI_MAX_PROC];

// Settle tracking. A newly-created game process must NOT be injected
// immediately (that mid-boot injection is the launch freeze - see the
// XPAD_AUTO_INJECT note above). pending_pids[] holds candidate processes we
// have seen but not yet injected, and pending_age[] counts how many consecutive
// polls each has stayed alive; a candidate is only injected once its age
// reaches XPAD_INJECT_SETTLE_POLLS, by which point it has settled into a running
// game. Slots are kept in lockstep (pending_age[i] belongs to pending_pids[i]).
// try_auto_inject() runs every ~2 s (see the poll loop), so the settle window
// below is ~2 s * XPAD_INJECT_SETTLE_POLLS.
#define XPAD_INJECT_SETTLE_POLLS 6 // ~12 s: comfortably past a game's own boot
static uint32_t pending_pids[PS3MAPI_MAX_PROC];
static int32_t  pending_age[PS3MAPI_MAX_PROC];

// Budget for auto-inject log lines. A process whose injection keeps failing is
// not marked injected, so try_auto_inject() retries it every ~2s; without a cap
// that would flood the rotated log and push the interesting boot-time events
// (including whatever preceded a freeze) out of the two kept files. The budget
// is generous enough to capture every launch during a normal session but stops
// a stuck process from filling the disk.
#define XPAD_INJECT_LOG_BUDGET 80
static int32_t inject_log_budget = XPAD_INJECT_LOG_BUDGET;

// Injection runs on its own thread (see the INPUT-DEATH FIX note above):
// ps3mapi_load_module() can hang in the kernel, and anything that hangs must
// not share a thread with the input pump. inject_call_t0 is the lv2 timestamp
// of the load_module call currently in flight (0 when idle); the poll thread
// watchdogs it so a hang leaves a breadcrumb in the debug log instead of
// vanishing silently. Written only by the injection thread, read by the poll
// thread; a uint64_t load/store is atomic on the PPU so no lock is needed.
static sys_ppu_thread_t inject_thread_id = (sys_ppu_thread_t)-1;
static volatile uint64_t inject_call_t0;
// how long an in-flight load_module call may run before the watchdog logs it;
// a healthy call returns in well under a second
#define XPAD_INJECT_HANG_USEC 15000000ULL
static uint8_t inject_hang_logged;

// never inject into VSH or the system processes - only into an actual game/app
static int32_t is_system_process(const char *name) {
  static const char *sys[] = {"vsh", "sys_", "default.self"};
  int32_t i;

  for (i = 0; i < (int32_t)(sizeof(sys) / sizeof(sys[0])); i++) {
    if (strstr(name, sys[i]) != NULL) {
      return 1;
    }
  }
  return 0;
}

static int32_t ps3mapi_get_all_pids(uint32_t *pids) {
  memset(pids, 0, sizeof(uint32_t) * PS3MAPI_MAX_PROC);
  system_call_3(8, SYSCALL8_OPCODE_PS3MAPI, PS3MAPI_OPCODE_GET_ALL_PROC_PID, (uint64_t)(uint32_t)pids);
  return (int32_t)p1;
}

static void ps3mapi_get_proc_name(uint32_t pid, char *name) {
  name[0] = 0;
  system_call_4(8, SYSCALL8_OPCODE_PS3MAPI, PS3MAPI_OPCODE_GET_PROC_NAME_BY_PID, (uint64_t)pid, (uint64_t)(uint32_t)name);
}

static int32_t ps3mapi_load_module(uint32_t pid, const char *path) {
  system_call_6(8, SYSCALL8_OPCODE_PS3MAPI, PS3MAPI_OPCODE_LOAD_PROC_MODULE, (uint64_t)pid, (uint64_t)(uint32_t)path, 0, 0);
  return (int32_t)p1;
}

static int32_t pid_in_list(uint32_t pid, const uint32_t *pids) {
  int32_t i;

  for (i = 0; i < PS3MAPI_MAX_PROC; i++) {
    if (pids[i] == pid) {
      return 1;
    }
  }
  return 0;
}

static int32_t already_injected(uint32_t pid) {
  return pid_in_list(pid, injected_pids);
}

static void mark_injected(uint32_t pid) {
  int32_t i;

  for (i = 0; i < PS3MAPI_MAX_PROC; i++) {
    if (injected_pids[i] == 0) {
      injected_pids[i] = pid;
      return;
    }
  }
}

// Record that pid was seen alive this poll and return its consecutive-seen age
// (capped at the settle threshold), adding it to the pending table on first
// sight. Returns 0 if the table is full - an untracked candidate is treated as
// not-yet-settled and therefore not injected, which fails safe.
static int32_t pending_age_bump(uint32_t pid) {
  int32_t i, free = -1;

  for (i = 0; i < PS3MAPI_MAX_PROC; i++) {
    if (pending_pids[i] == pid) {
      if (pending_age[i] < XPAD_INJECT_SETTLE_POLLS) {
        pending_age[i]++;
      }
      return pending_age[i];
    }
    if (free < 0 && pending_pids[i] == 0) {
      free = i;
    }
  }
  if (free >= 0) {
    pending_pids[free] = pid;
    pending_age[free] = 1;
    return 1;
  }
  return 0;
}

static void pending_forget(uint32_t pid) {
  int32_t i;

  for (i = 0; i < PS3MAPI_MAX_PROC; i++) {
    if (pending_pids[i] == pid) {
      pending_pids[i] = 0;
      pending_age[i] = 0;
      return;
    }
  }
}

static void try_auto_inject(void) {
  uint32_t pids[PS3MAPI_MAX_PROC];
  char name[256]; // generous: PS3MAPI writes the process self-path here
  int32_t i, r, age;

  if ((r = ps3mapi_get_all_pids(pids)) < 0) {
    if (inject_log_budget > 0) {
      inject_log_budget--;
      xlog_code("auto-inject: get_all_pids failed", r);
    }
    return;
  }

  // forget pids that have exited so their slots (and a possibly-reused pid)
  // can be injected again next time
  for (i = 0; i < PS3MAPI_MAX_PROC; i++) {
    if (injected_pids[i] != 0 && !pid_in_list(injected_pids[i], pids)) {
      injected_pids[i] = 0;
    }
    // a candidate that exited before it settled (e.g. multiMAN's prepNTFS /
    // RELOAD loader helpers) drops out of the pending table, and a reused pid
    // restarts its settle window from scratch
    if (pending_pids[i] != 0 && !pid_in_list(pending_pids[i], pids)) {
      pending_pids[i] = 0;
      pending_age[i] = 0;
    }
  }

  for (i = 0; i < PS3MAPI_MAX_PROC; i++) {
    if (pids[i] == 0 || already_injected(pids[i])) {
      continue;
    }
    ps3mapi_get_proc_name(pids[i], name);
    name[sizeof(name) - 1] = 0;
    if (name[0] == 0 || is_system_process(name)) {
      continue;
    }
    // Do NOT inject a process the first time we see it. Injecting into a game
    // that is still booting hangs the kernel prx loader (the launch freeze -
    // see the XPAD_AUTO_INJECT note above). Age the candidate instead and only
    // inject once it has stayed alive long enough to have finished booting.
    age = pending_age_bump(pids[i]);
    if (age < XPAD_INJECT_SETTLE_POLLS) {
      continue;
    }
    // Log the process and the attempt BEFORE the syscall. Because each xlog()
    // line is flushed with its own open/write/close and the log survives a
    // reboot, this line is the breadcrumb that names which game we were
    // injecting into if the console ever does lock up here.
    if (inject_log_budget > 0) {
      inject_log_budget--;
      xlog_proc("auto-inject: loading xpad_game.sprx into", name, pids[i]);
    }
    inject_call_t0 = time_now_usec();
    r = ps3mapi_load_module(pids[i], XPAD_GAME_SPRX_PATH);
    inject_call_t0 = 0;
    if (r >= 0) {
      mark_injected(pids[i]);
      pending_forget(pids[i]); // settled and injected: stop tracking it
      if (inject_log_budget > 0) {
        inject_log_budget--;
        xlog_proc("auto-inject: loaded xpad_game.sprx OK into", name, pids[i]);
      }
    } else if (inject_log_budget > 0) {
      // not marked injected: left in the pending table with its age pinned at
      // the threshold, so it retries next pass. The budget keeps a persistent
      // failure from flooding the log
      inject_log_budget--;
      xlog_code("auto-inject: load_module failed", r);
    }
  }
}

// Auto-injection is enabled only when the user has created the opt-in file
// (see the LOADER-WEDGE FIX note above). Existence is the whole switch; the
// content is ignored.
static int32_t auto_inject_opted_in(void) {
  int32_t fd;

  if (cellFsOpen(XPAD_INJECT_OPT_IN_PATH, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED) {
    return 0;
  }
  cellFsClose(fd);
  return 1;
}

// Dedicated injection thread: scans for game processes every ~2 s, same
// cadence the poll loop used when it did this inline. All the injection state
// (injected_pids/pending_*/inject_log_budget) is touched only from this thread.
// If ps3mapi_load_module() hangs, this thread blocks forever and injection is
// simply lost for the session — input, which lives on the poll thread, is
// untouched. On a normal shutdown the thread notices 'running' going to 0
// within one sleep period and exits; xpadd_stop_thread joins it.
static void inject_thread(uint64_t arg) {
  (void)arg;
  while (running) {
    sys_timer_sleep(2);
    if (!running) {
      break;
    }
    try_auto_inject();
  }
  sys_ppu_thread_exit(0);
}
#endif // XPAD_AUTO_INJECT

void usb_done_cb(int32_t result, int32_t count, void *arg) {
  XPAD_UNIT_t *unit = (XPAD_UNIT_t *)arg;
  (void)result;
  (void)count;

  // LED/rumble out-transfer finished; unit->out_data is free again
  block(unit->rb_mutex);
  unit->out_busy = 0;
  unit->pending--;
  unblock(unit->rb_mutex);
}

// send an LED/rumble command; the payload is copied into unit->out_data
// because the transfer is asynchronous and would otherwise read the
// caller's stack frame after it returned
static int32_t write_xpad(XPAD_UNIT_t *unit, uint8_t *data, int32_t len) {
  int32_t r;

  if (unit == NULL || unit->o_pipe < 0 || len > XPAD_OUT_LEN) {
    return(-1);
  }
  block(unit->rb_mutex);
  if (unit->closing || unit->out_busy) {
    // a previous command is still in flight and owns out_data; commands
    // are rare (LED changes) and callers tolerate the occasional drop
    unblock(unit->rb_mutex);
    return(-1);
  }
  unit->out_busy = 1;
  unit->pending++;
  memcpy(unit->out_data, data, len);
  unblock(unit->rb_mutex);
  if ((r = cellUsbdInterruptTransfer(unit->o_pipe, unit->out_data, len, usb_done_cb, unit)) != CELL_OK) {
    block(unit->rb_mutex);
    unit->out_busy = 0;
    unit->pending--;
    unblock(unit->rb_mutex);
    if (unit->dbg_err_count < 5) {
      unit->dbg_err_count++;
      xlog_dev("LED/rumble transfer submit failed for", unit->vid, unit->pid, r);
    }
  }
  return(r);
}

// on-screen connect/disconnect feedback so users can tell at a glance
// whether their controller/dongle was recognized
static void notify_pad_connected(XPAD_UNIT_t *unit) {
  char msg[96], *m;

  m = msg;
  m = append_str(m, "XPAD Rev: ");
  m = append_str(m, unit->name ? unit->name : "Controller");
  m = append_str(m, " (");
  m = append_hex16(m, unit->vid);
  m = append_str(m, ":");
  m = append_hex16(m, unit->pid);
  m = append_str(m, ")");
  if (unit->last_port >= 0) {
    m = append_str(m, " on port ");
    m = append_int(m, unit->last_port + 1);
  }
  show_msg(msg);
}

static void notify_pad_disconnected(XPAD_UNIT_t *unit) {
  char msg[96], *m;

  m = msg;
  m = append_str(m, "XPAD Rev: ");
  m = append_str(m, unit->name ? unit->name : "Controller");
  m = append_str(m, " disconnected");
  show_msg(msg);
}

// start of wired controller specific methods
static int32_t xpad_probe(int32_t dev_id) {
  uint16_t idVendor, idProduct;
  uint32_t i;
  UsbDeviceDescriptor *ddesc;
  UsbInterfaceDescriptor *idesc;

  block(xpad_mutex);
  if (XPAD.n >= MAX_XPAD_NUM) { // no free pad slot
    unblock(xpad_mutex);
    xlog("probe rejected: no free pad slot");
    return(CELL_USBD_PROBE_FAILED);
  }
  unblock(xpad_mutex);
  if ((ddesc = (UsbDeviceDescriptor *)cellUsbdScanStaticDescriptor(dev_id, NULL, USB_DESCRIPTOR_TYPE_DEVICE)) == NULL) {
    xlog("probe rejected: no device descriptor");
    return(CELL_USBD_PROBE_FAILED);
  }
  idesc = (UsbInterfaceDescriptor *)ddesc;
  if ((idesc = (UsbInterfaceDescriptor *)cellUsbdScanStaticDescriptor(dev_id, idesc, USB_DESCRIPTOR_TYPE_INTERFACE)) == NULL) {
    xlog("probe rejected: no interface descriptor");
    return(CELL_USBD_PROBE_FAILED);
  }

  // make sure product id and vendor id are valid
  idVendor = SWAP16(ddesc->idVendor);
  idProduct = SWAP16(ddesc->idProduct);
  for (i = 0; i < MAX_XPAD_DEV_NUM; i++) {
    if (xpad_info[i].vid == idVendor && xpad_info[i].pid == idProduct) {
      xlog_dev("probe matched (wired)", idVendor, idProduct, 0);
      return(CELL_USBD_PROBE_SUCCEEDED);
    }
  }
  for (i = 0; i < (uint32_t)extra_count; i++) {
    if (extra_info[i].xtype == XTYPE_XBOX360 && extra_info[i].vid == idVendor && extra_info[i].pid == idProduct) {
      xlog_dev("probe matched (wired, devices.txt)", idVendor, idProduct, 0);
      return(CELL_USBD_PROBE_SUCCEEDED);
    }
  }
  xlog_dev("probe rejected: VID/PID not in wired table", idVendor, idProduct, 0);
  return(CELL_USBD_PROBE_FAILED);
}

static int32_t xpad_attach(int32_t dev_id) {
  int32_t payload;
  uint8_t *desc = NULL;
  UsbConfigurationDescriptor *cdesc;
  UsbInterfaceDescriptor *ifd, *cur_if = NULL, *xin_if = NULL, *first_if = NULL;
  UsbEndpointDescriptor *ed, *ep_in = NULL, *ep_out = NULL, *first_in = NULL, *first_out = NULL;
  XPAD_UNIT_t *unit;

  UsbDeviceDescriptor *ddesc;
  uint16_t idVendor = 0, idProduct = 0;

  if ((ddesc = (UsbDeviceDescriptor *)cellUsbdScanStaticDescriptor(dev_id, NULL, USB_DESCRIPTOR_TYPE_DEVICE)) != NULL) {
    idVendor = SWAP16(ddesc->idVendor);
    idProduct = SWAP16(ddesc->idProduct);
  }
  xlog_dev("attach starting for", idVendor, idProduct, 0);
  if ((cdesc = (UsbConfigurationDescriptor *) cellUsbdScanStaticDescriptor(dev_id, NULL, USB_DESCRIPTOR_TYPE_CONFIGURATION)) == NULL) {
    xlog("attach failed: no configuration descriptor");
    return (CELL_USBD_ATTACH_FAILED);
  }

  // some dongles put audio/HID interfaces first or use in endpoints other
  // than 0x81, so scan every interface for the XInput class triple
  // (class 0xFF, subclass 0x5D, protocol 0x01) and take that interface's
  // interrupt IN/OUT endpoints, whatever their addresses
  while ((desc = (uint8_t *)cellUsbdScanStaticDescriptor(dev_id, desc, 0)) != NULL) {
    if (desc[1] == USB_DESCRIPTOR_TYPE_INTERFACE) {
      ifd = (UsbInterfaceDescriptor *)desc;
      if (first_if == NULL) {
        first_if = ifd;
      }
      if (xin_if == NULL &&
          ifd->bInterfaceClass == 0xFF && USB_IF_SUBCLASS(ifd) == 0x5D && ifd->bInterfaceProtocol == 0x01) {
        xin_if = ifd;
      }
      cur_if = ifd;
    } else if (desc[1] == USB_DESCRIPTOR_TYPE_ENDPOINT) {
      ed = (UsbEndpointDescriptor *)desc;
      if ((ed->bmAttributes & 0x03) != 0x03) { // only interrupt endpoints
        continue;
      }
      if (cur_if != NULL && cur_if == xin_if) {
        if ((ed->bEndpointAddress & 0x80) && ep_in == NULL) {
          ep_in = ed;
        } else if (!(ed->bEndpointAddress & 0x80) && ep_out == NULL) {
          ep_out = ed;
        }
      }
      if (cur_if != NULL && cur_if == first_if) {
        if ((ed->bEndpointAddress & 0x80) && first_in == NULL) {
          first_in = ed;
        } else if (!(ed->bEndpointAddress & 0x80) && first_out == NULL) {
          first_out = ed;
        }
      }
    }
  }
  if (xin_if == NULL || ep_in == NULL) {
    // no XInput interface advertised; fall back to the first interface
    // (the pre-scan behavior, minus the hard 0x81 requirement)
    xlog("attach: no XInput class interface found, falling back to first interface");
    xin_if = first_if;
    ep_in = first_in;
    ep_out = first_out;
  }
  if (xin_if == NULL || ep_in == NULL) {
    xlog("attach failed: no usable interface/interrupt-IN endpoint");
    return(CELL_USBD_ATTACH_FAILED);
  }
  payload = SWAP16(ep_in->wMaxPacketSize);
  {
    char dbg[128], *m = dbg;
    m = append_str(m, "attach: interface ");
    m = append_int(m, xin_if->bInterfaceNumber);
    m = append_str(m, " alt ");
    m = append_int(m, xin_if->bAlternateSetting);
    m = append_str(m, ", ep_in 0x");
    m = append_hex16(m, ep_in->bEndpointAddress);
    m = append_str(m, ", ep_out ");
    if (ep_out != NULL) {
      m = append_str(m, "0x");
      m = append_hex16(m, ep_out->bEndpointAddress);
    } else {
      m = append_str(m, "none");
    }
    m = append_str(m, ", payload ");
    m = append_int(m, payload);
    xlog(dbg);
  }
  if ((unit = unit_alloc(dev_id, payload, xin_if->bInterfaceNumber, xin_if->bAlternateSetting, XTYPE_XBOX360)) == NULL) {
    xlog("attach failed: unit alloc (out of memory or unit list full)");
    return(CELL_USBD_ATTACH_FAILED);
  }
  unit->vid = idVendor;
  unit->pid = idProduct;
  unit->name = find_device_name(idVendor, idProduct);
  if ((unit->c_pipe = cellUsbdOpenPipe(dev_id, NULL)) < 0) {
    xlog_code("attach failed: control pipe open", unit->c_pipe);
    unit_release(unit);
    return(CELL_USBD_ATTACH_FAILED);
  }
  if ((unit->i_pipe = cellUsbdOpenPipe(dev_id, ep_in)) < 0) {
    xlog_code("attach failed: IN pipe open", unit->i_pipe);
    unit_release(unit);
    return(CELL_USBD_ATTACH_FAILED);
  }
  if (ep_out != NULL) {
    if ((unit->o_pipe = cellUsbdOpenPipe(dev_id, ep_out)) < 0) {
      xlog_code("attach failed: OUT pipe open", unit->o_pipe);
      unit_release(unit);
      return(CELL_USBD_ATTACH_FAILED);
    }
  } else {
    /* Never invent an OUT address by mutating the IN descriptor. Continue in
       input-only mode when a device genuinely exposes no interrupt OUT pipe.
       compatible devices advertise their real OUT endpoint, so descriptor
       discovery selects the correct address instead of assuming one. */
    xlog("attach: no interrupt-OUT endpoint; continuing input-only");
  }

  // endpoint found; a wired pad is only useful with an LDD pad behind it,
  // so claim the pad slot and register before starting configuration
  block(xpad_mutex);
  if (register_ldd_controller(unit) != CELL_PAD_OK) {
    unblock(xpad_mutex);
    unit_release(unit);
    show_msg((char *)"XPAD Rev: no free pad slot for new controller");
    return(CELL_USBD_ATTACH_FAILED);
  }
  unblock(xpad_mutex);
  cellUsbdSetPrivateData(dev_id, unit);
  if (unit_submit_begin(unit)) {
    int32_t cr = cellUsbdSetConfiguration(unit->c_pipe, cdesc->bConfigurationValue, set_config_done, unit);
    if (cr != CELL_OK) {
      xlog_code("attach: set_configuration submit failed", cr);
      unit_submit_end(unit);
    }
  }
  {
    char dbg[64], *m = dbg;
    m = append_str(m, "attach OK, LDD pad port ");
    m = append_int(m, unit->last_port);
    xlog(dbg);
  }
  notify_pad_connected(unit);
  return(CELL_USBD_ATTACH_SUCCEEDED);
}

static int32_t xpad_detach(int32_t dev_id) {
  XPAD_UNIT_t *unit;

  // Xbox controller has been unplugged
  // disconnect virtual controller associated to it
  if ((unit = (XPAD_UNIT_t *)cellUsbdGetPrivateData(dev_id)) == NULL) {
    xlog("USB detach for unknown device (no private data)");
    return(CELL_USBD_DETACH_FAILED);
  }
  xlog_dev("USB detach:", unit->vid, unit->pid, 0);
  block(xpad_mutex);
  if (unit->unit_idx >= 0 && unit->unit_idx < MAX_UNIT_NUM && XPAD.units[unit->unit_idx] == unit) {
    XPAD.units[unit->unit_idx] = NULL;
  }
  unregister_ldd_controller(unit);
  unblock(xpad_mutex);
  notify_pad_disconnected(unit);
  unit_free(unit);
  return(CELL_USBD_DETACH_SUCCEEDED);
}

static int32_t xpad_detach_all(void) {
  int32_t i;
  XPAD_UNIT_t *unit;

  // detach all wired controllers
  block(xpad_mutex);
  for (i = 0; i < MAX_UNIT_NUM; i++) {
    if ((unit = XPAD.units[i]) != NULL && unit->xtype == XTYPE_XBOX360) {
      XPAD.units[i] = NULL;
      unregister_ldd_controller(unit);
      unit_free(unit);
    }
  }
  unblock(xpad_mutex);
  return(CELL_USBD_DETACH_SUCCEEDED);
}

static void xpad_read_report(int32_t id, uint8_t *readBuf) {
  uint16_t *digit0, *digit1, *digit2,
           *analog_rx, *analog_ry, *analog_lx, *analog_ly,
           *press_l2, *press_r2, *press_l1, *press_r1,
           *press_up, *press_down, *press_left, *press_right,
           *press_cross, *press_square, *press_circle, *press_triangle;
  XBOX360_IN_REPORT *report;
  CellPadData data;

  report = (XBOX360_IN_REPORT *)readBuf;
  viewer_update_x360(id, report);
  memset(&data, 0, sizeof(CellPadData));
  data.len = 24;

  // map location of each button in virtual pad's data
  digit0 = &data.button[0];
  digit1 = &data.button[CELL_PAD_BTN_OFFSET_DIGITAL1];
  digit2 = &data.button[CELL_PAD_BTN_OFFSET_DIGITAL2];
  analog_rx = &data.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X];
  analog_ry = &data.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y];
  analog_lx = &data.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X];
  analog_ly = &data.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y];
  press_l2 = &data.button[CELL_PAD_BTN_OFFSET_PRESS_L2];
  press_r2 = &data.button[CELL_PAD_BTN_OFFSET_PRESS_R2];
  press_l1 = &data.button[CELL_PAD_BTN_OFFSET_PRESS_L1];
  press_r1 = &data.button[CELL_PAD_BTN_OFFSET_PRESS_R1];
  press_right = &data.button[CELL_PAD_BTN_OFFSET_PRESS_RIGHT];
  press_left = &data.button[CELL_PAD_BTN_OFFSET_PRESS_LEFT];
  press_up = &data.button[CELL_PAD_BTN_OFFSET_PRESS_UP];
  press_down = &data.button[CELL_PAD_BTN_OFFSET_PRESS_DOWN];
  press_cross = &data.button[CELL_PAD_BTN_OFFSET_PRESS_CROSS];
  press_square = &data.button[CELL_PAD_BTN_OFFSET_PRESS_SQUARE];
  press_circle = &data.button[CELL_PAD_BTN_OFFSET_PRESS_CIRCLE];
  press_triangle = &data.button[CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE];

  // set default controller values
  data.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] = 0x0080;
  data.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] = 0x0080;
  data.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X] = 0x0080;
  data.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y] = 0x0080;
  data.button[CELL_PAD_BTN_OFFSET_SENSOR_X] = 0x0200;
  data.button[CELL_PAD_BTN_OFFSET_SENSOR_Y] = 0x0200;
  data.button[CELL_PAD_BTN_OFFSET_SENSOR_Z] = 0x0200;
  data.button[CELL_PAD_BTN_OFFSET_SENSOR_G] = 0x0200;

  // read values from Xbox controller and map it to the virtual pad
  *digit0 = (report->buttons & btnXbox) ? *digit0 | CELL_PAD_CTRL_LDD_PS : *digit0 & ~CELL_PAD_CTRL_LDD_PS;
  *digit1 = (report->buttons & btnDigiLeft) ? *digit1 | CELL_PAD_CTRL_LEFT : *digit1 & ~CELL_PAD_CTRL_LEFT;
  *digit1 = (report->buttons & btnDigiDown) ? *digit1 | CELL_PAD_CTRL_DOWN : *digit1 & ~CELL_PAD_CTRL_DOWN;
  *digit1 = (report->buttons & btnDigiRight) ? *digit1 | CELL_PAD_CTRL_RIGHT : *digit1 & ~CELL_PAD_CTRL_RIGHT;
  *digit1 = (report->buttons & btnDigiUp) ? *digit1 | CELL_PAD_CTRL_UP : *digit1 & ~CELL_PAD_CTRL_UP;
  *digit1 = (report->buttons & btnStart) ? *digit1 | CELL_PAD_CTRL_START : *digit1 & ~CELL_PAD_CTRL_START;
  *digit1 = (report->buttons & btnHatRight) ? *digit1 | CELL_PAD_CTRL_R3 : *digit1 & ~CELL_PAD_CTRL_R3;
  *digit1 = (report->buttons & btnHatLeft) ? *digit1 | CELL_PAD_CTRL_L3 : *digit1 & ~CELL_PAD_CTRL_L3;
  *digit1 = (report->buttons & btnBack) ? *digit1 | CELL_PAD_CTRL_SELECT : *digit1 & ~CELL_PAD_CTRL_SELECT;
  *digit2 = (report->buttons & btnX) ? *digit2 | CELL_PAD_CTRL_SQUARE : *digit2 & ~CELL_PAD_CTRL_SQUARE;
  *digit2 = (report->buttons & btnA) ? *digit2 | CELL_PAD_CTRL_CROSS : *digit2 & ~CELL_PAD_CTRL_CROSS;
  *digit2 = (report->buttons & btnB) ? *digit2 | CELL_PAD_CTRL_CIRCLE : *digit2 & ~CELL_PAD_CTRL_CIRCLE;
  *digit2 = (report->buttons & btnY) ? *digit2 | CELL_PAD_CTRL_TRIANGLE : *digit2 & ~CELL_PAD_CTRL_TRIANGLE;
  *digit2 = (report->buttons & btnShoulderRight) ? *digit2 | CELL_PAD_CTRL_R1 : *digit2 & ~CELL_PAD_CTRL_R1;
  *digit2 = (report->buttons & btnShoulderLeft) ? *digit2 | CELL_PAD_CTRL_L1 : *digit2 & ~CELL_PAD_CTRL_L1;
  *digit2 = (report->trigL > 0) ? *digit2 | CELL_PAD_CTRL_L2 : *digit2 & ~CELL_PAD_CTRL_L2;
  *digit2 = (report->trigR > 0) ? *digit2 | CELL_PAD_CTRL_R2 : *digit2 & ~CELL_PAD_CTRL_R2;

  // emulate pressure values except for L2 and R2, button presses correspond to max sensitivity value
  *press_l2 = (report->trigL);
  *press_r2 = (report->trigR);
  *press_l1 = (report->buttons & btnShoulderLeft) ? 0xFF : 0;
  *press_r1 = (report->buttons & btnShoulderRight) ? 0xFF : 0;
  *press_up = (report->buttons & btnDigiUp) ? 0xFF : 0;
  *press_down = (report->buttons & btnDigiDown) ? 0xFF : 0;
  *press_left = (report->buttons & btnDigiLeft) ? 0xFF : 0;
  *press_right = (report->buttons & btnDigiRight) ? 0xFF : 0;
  *press_square = (report->buttons & btnX) ? 0xFF : 0;
  *press_circle = (report->buttons & btnB) ? 0xFF : 0;
  *press_cross = (report->buttons & btnA) ? 0xFF : 0;
  *press_triangle = (report->buttons & btnY) ? 0xFF : 0;

  // PS3 pads use 8 bit values for each axis while Xbox pads use 16 bit
  // convert Xbox analog values for PS3 compatibility
  *analog_rx = (report->right.x-0x80) & 0x00FF;
  *analog_ry = ((report->right.y ^ 0xFF)-0x80) & 0x00FF;
  *analog_lx = (report->left.x-0x80) & 0x00FF;
  *analog_ly = ((report->left.y ^ 0xFF)-0x80) & 0x00FF;

  remap_apply(id, &data, 0);
  xpad_analog_apply(&analog_config, &data);
  // send pad data to virtual pad
  cellPadLddDataInsert(handle[id], &data);
}

static int32_t xpad_read_input(XPAD_UNIT_t *unit, void *data) {
  unsigned char *p;
  unsigned char *xpadbuf;
  int32_t have_report = 0;
  XBOX360_IN_REPORT *report;

  p = (unsigned char *)data;
  if (unit == NULL) {
    return(-1);
  }

  // Copy the newest report directly and discard stale snapshots. Digital
  // press edges and trigger peaks observed by the USB callback are merged
  // into this update before the queue is cleared.
  block(unit->rb_mutex);
  if (unit->rblen > 0) {
    int32_t latest = unit->wp - 1;
    if (latest < 0) {
      latest = RINGBUF_SIZE - 1;
    }
    xpadbuf = &unit->ringbuf[latest][0];
    report = (XBOX360_IN_REPORT *)&xpadbuf[2];
    if ((xpadbuf[1] >= sizeof(XBOX360_IN_REPORT)) &&
        (report->header.command == inReport) && (report->header.size == sizeof(XBOX360_IN_REPORT))) {
      memcpy(p, &xpadbuf[2], xpadbuf[1]);
      report = (XBOX360_IN_REPORT *)p;
      report->buttons |= (unit->pending_press_edges & ~report->buttons);
      if (unit->pending_trig_l > report->trigL) {
        report->trigL = unit->pending_trig_l;
      }
      if (unit->pending_trig_r > report->trigR) {
        report->trigR = unit->pending_trig_r;
      }
      unit->pending_press_edges = 0;
      unit->pending_trig_l = 0;
      unit->pending_trig_r = 0;
      have_report = 1;
    }
    unit->rp = unit->wp;
    unit->rblen = 0;
  }
  unblock(unit->rb_mutex);

  // parse outside the ring-buffer lock so USB completion callbacks
  // are never blocked on cellPadLddDataInsert()
  if (have_report && unit->number >= 0 && handle[unit->number] >= 0) {
    xpad_read_report(unit->number, p);
  }
  return(0);
}

static int32_t xpad_set_led(XPAD_UNIT_t *unit, uint8_t led) {
  uint8_t out[3] = {0x01, 0x03, led};

  if (write_xpad(unit, out, 3) < 0) {
    return(-1);
  }
  return(CELL_OK);
}

static int32_t xpad_set_rumble(XPAD_UNIT_t *unit, uint8_t lval, uint8_t rval) {
  uint8_t out[8] = {0x00, 0x08, 0x00, lval, rval, 0x00, 0x00, 0x00};

  if (write_xpad(unit, out, 8) < 0) {
    return(-1);
  }
  return(CELL_OK);
}
// end of wired controller specific methods 

// start of wired Sony controller specific methods
static int32_t pspad_type_for_device(uint16_t vid, uint16_t pid) {
  int32_t i;

  for (i = 0; i < MAX_PSPAD_DEV_NUM; i++) {
    if (pspad_info[i].vid == vid && pspad_info[i].pid == pid) {
      return(pspad_info[i].xtype);
    }
  }
  for (i = 0; i < extra_count; i++) {
    if ((extra_info[i].xtype == PTYPE_PS4 || extra_info[i].xtype == PTYPE_PS5) &&
        extra_info[i].vid == vid && extra_info[i].pid == pid) {
      return(extra_info[i].xtype);
    }
  }
  return(-1);
}

static int32_t pspad_probe(int32_t dev_id) {
  UsbDeviceDescriptor *ddesc;
  uint16_t vid, pid;

  block(xpad_mutex);
  if (XPAD.n >= MAX_XPAD_NUM) {
    unblock(xpad_mutex);
    return(CELL_USBD_PROBE_FAILED);
  }
  unblock(xpad_mutex);
  ddesc = (UsbDeviceDescriptor *)cellUsbdScanStaticDescriptor(dev_id, NULL, USB_DESCRIPTOR_TYPE_DEVICE);
  if (ddesc == NULL) return(CELL_USBD_PROBE_FAILED);
  vid = SWAP16(ddesc->idVendor);
  pid = SWAP16(ddesc->idProduct);
  if (pspad_type_for_device(vid, pid) < 0) return(CELL_USBD_PROBE_FAILED);
  xlog_dev("probe matched (Sony HID)", vid, pid, 0);
  return(CELL_USBD_PROBE_SUCCEEDED);
}

static int32_t pspad_attach(int32_t dev_id) {
  uint8_t *desc = NULL;
  UsbDeviceDescriptor *ddesc;
  UsbConfigurationDescriptor *cdesc;
  UsbInterfaceDescriptor *ifd, *cur_if = NULL, *hid_if = NULL;
  UsbEndpointDescriptor *ed, *ep_in = NULL, *ep_out = NULL;
  XPAD_UNIT_t *unit;
  uint16_t vid, pid;
  int32_t xtype, payload;

  ddesc = (UsbDeviceDescriptor *)cellUsbdScanStaticDescriptor(dev_id, NULL, USB_DESCRIPTOR_TYPE_DEVICE);
  cdesc = (UsbConfigurationDescriptor *)cellUsbdScanStaticDescriptor(dev_id, NULL, USB_DESCRIPTOR_TYPE_CONFIGURATION);
  if (ddesc == NULL || cdesc == NULL) return(CELL_USBD_ATTACH_FAILED);
  vid = SWAP16(ddesc->idVendor);
  pid = SWAP16(ddesc->idProduct);
  xtype = pspad_type_for_device(vid, pid);
  if (xtype < 0) return(CELL_USBD_ATTACH_FAILED);

  while ((desc = (uint8_t *)cellUsbdScanStaticDescriptor(dev_id, desc, 0)) != NULL) {
    if (desc[1] == USB_DESCRIPTOR_TYPE_INTERFACE) {
      ifd = (UsbInterfaceDescriptor *)desc;
      cur_if = ifd;
      if (hid_if == NULL && ifd->bInterfaceClass == 0x03) {
        hid_if = ifd;
      }
    } else if (desc[1] == USB_DESCRIPTOR_TYPE_ENDPOINT) {
      ed = (UsbEndpointDescriptor *)desc;
      if (cur_if == hid_if && (ed->bmAttributes & 0x03) == 0x03) {
        if ((ed->bEndpointAddress & 0x80) && ep_in == NULL) ep_in = ed;
        if (!(ed->bEndpointAddress & 0x80) && ep_out == NULL) ep_out = ed;
      }
    }
  }
  if (hid_if == NULL || ep_in == NULL) {
    xlog_dev("Sony attach failed: no HID interrupt-IN", vid, pid, 0);
    return(CELL_USBD_ATTACH_FAILED);
  }
  payload = SWAP16(ep_in->wMaxPacketSize);
  if (payload < 12 || payload > MAX_XPAD_PAYLOAD) {
    xlog_dev("Sony attach failed: unexpected report size", vid, pid, payload);
    return(CELL_USBD_ATTACH_FAILED);
  }
  unit = unit_alloc(dev_id, payload, hid_if->bInterfaceNumber, hid_if->bAlternateSetting, (uint8_t)xtype);
  if (unit == NULL) return(CELL_USBD_ATTACH_FAILED);
  unit->vid = vid;
  unit->pid = pid;
  unit->name = find_device_name(vid, pid);
  if ((unit->c_pipe = cellUsbdOpenPipe(dev_id, NULL)) < 0 ||
      (unit->i_pipe = cellUsbdOpenPipe(dev_id, ep_in)) < 0) {
    unit_release(unit);
    return(CELL_USBD_ATTACH_FAILED);
  }
  if (ep_out != NULL && (unit->o_pipe = cellUsbdOpenPipe(dev_id, ep_out)) < 0) {
    unit_release(unit);
    return(CELL_USBD_ATTACH_FAILED);
  }

  block(xpad_mutex);
  if (register_ldd_controller(unit) != CELL_PAD_OK) {
    unblock(xpad_mutex);
    unit_release(unit);
    show_msg((char *)"XPAD Rev: no free pad slot for Sony controller");
    return(CELL_USBD_ATTACH_FAILED);
  }
  unblock(xpad_mutex);
  cellUsbdSetPrivateData(dev_id, unit);
  if (unit_submit_begin(unit)) {
    int32_t cr = cellUsbdSetConfiguration(unit->c_pipe, cdesc->bConfigurationValue, set_config_done, unit);
    if (cr != CELL_OK) unit_submit_end(unit);
  }
  xlog_dev("Sony HID attached", vid, pid, payload);
  notify_pad_connected(unit);
  return(CELL_USBD_ATTACH_SUCCEEDED);
}

static int32_t pspad_detach(int32_t dev_id) {
  return(xpad_detach(dev_id));
}

static int32_t pspad_detach_all(void) {
  int32_t i;
  XPAD_UNIT_t *unit;

  block(xpad_mutex);
  for (i = 0; i < MAX_UNIT_NUM; i++) {
    unit = XPAD.units[i];
    if (unit != NULL && (unit->xtype == PTYPE_PS4 || unit->xtype == PTYPE_PS5)) {
      XPAD.units[i] = NULL;
      unregister_ldd_controller(unit);
      unit_free(unit);
    }
  }
  unblock(xpad_mutex);
  return(CELL_USBD_DETACH_SUCCEEDED);
}

static void pspad_viewer_update(int32_t id, uint8_t b0, uint8_t b1, uint8_t b2,
                                uint8_t l2, uint8_t r2, uint8_t lx, uint8_t ly,
                                uint8_t rx, uint8_t ry) {
  uint16_t buttons = 0;
  uint8_t hat = b0 & 0x0f;

  if (hat == 0 || hat == 1 || hat == 7) buttons |= btnDigiUp;
  if (hat == 1 || hat == 2 || hat == 3) buttons |= btnDigiRight;
  if (hat == 3 || hat == 4 || hat == 5) buttons |= btnDigiDown;
  if (hat == 5 || hat == 6 || hat == 7) buttons |= btnDigiLeft;
  if (b0 & 0x10) buttons |= btnX;
  if (b0 & 0x20) buttons |= btnA;
  if (b0 & 0x40) buttons |= btnB;
  if (b0 & 0x80) buttons |= btnY;
  if (b1 & 0x01) buttons |= btnShoulderLeft;
  if (b1 & 0x02) buttons |= btnShoulderRight;
  if (b1 & 0x10) buttons |= btnBack;
  if (b1 & 0x20) buttons |= btnStart;
  if (b1 & 0x40) buttons |= btnHatLeft;
  if (b1 & 0x80) buttons |= btnHatRight;
  if (b2 & 0x01) buttons |= btnXbox;
  block(viewer_mutex);
  viewer_state[id].buttons = viewer_buttons(buttons);
  viewer_state[id].lt = l2; viewer_state[id].rt = r2;
  viewer_state[id].lx = lx; viewer_state[id].ly = ly;
  viewer_state[id].rx = rx; viewer_state[id].ry = ry;
  viewer_state[id].connected = 1;
  unblock(viewer_mutex);
}

static void pspad_read_report(XPAD_UNIT_t *unit, uint8_t *readBuf) {
  CellPadData data;
  uint8_t x, y, rx, ry, l2, r2, b0, b1, b2, hat, touchpad;
  uint16_t *digit0, *digit1, *digit2;

  if (unit->xtype == PTYPE_PS4) {
    DS4_USB_REPORT_t *report = (DS4_USB_REPORT_t *)readBuf;
    x = report->x; y = report->y; rx = report->rx; ry = report->ry;
    b0 = report->buttons[0]; b1 = report->buttons[1]; b2 = report->buttons[2];
    l2 = report->z; r2 = report->rz;
  } else {
    DS5_USB_REPORT_t *report = (DS5_USB_REPORT_t *)readBuf;
    x = report->x; y = report->y; rx = report->rx; ry = report->ry;
    b0 = report->buttons[0]; b1 = report->buttons[1]; b2 = report->buttons[2];
    l2 = report->z; r2 = report->rz;
  }
  touchpad = (b2 & 0x02) ? REMAP_EXTRA_TOUCHPAD : 0;
  pspad_viewer_update(unit->number, b0, b1, b2, l2, r2, x, y, rx, ry);

  memset(&data, 0, sizeof(data));
  data.len = 24;
  digit0 = &data.button[0];
  digit1 = &data.button[CELL_PAD_BTN_OFFSET_DIGITAL1];
  digit2 = &data.button[CELL_PAD_BTN_OFFSET_DIGITAL2];
  data.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X] = x;
  data.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y] = y;
  data.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] = rx;
  data.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] = ry;
  data.button[CELL_PAD_BTN_OFFSET_SENSOR_X] = 0x0200;
  data.button[CELL_PAD_BTN_OFFSET_SENSOR_Y] = 0x0200;
  data.button[CELL_PAD_BTN_OFFSET_SENSOR_Z] = 0x0200;
  data.button[CELL_PAD_BTN_OFFSET_SENSOR_G] = 0x0200;

  hat = b0 & 0x0f;
  if (hat == 0 || hat == 1 || hat == 7) { *digit1 |= CELL_PAD_CTRL_UP; data.button[CELL_PAD_BTN_OFFSET_PRESS_UP] = 0xff; }
  if (hat == 1 || hat == 2 || hat == 3) { *digit1 |= CELL_PAD_CTRL_RIGHT; data.button[CELL_PAD_BTN_OFFSET_PRESS_RIGHT] = 0xff; }
  if (hat == 3 || hat == 4 || hat == 5) { *digit1 |= CELL_PAD_CTRL_DOWN; data.button[CELL_PAD_BTN_OFFSET_PRESS_DOWN] = 0xff; }
  if (hat == 5 || hat == 6 || hat == 7) { *digit1 |= CELL_PAD_CTRL_LEFT; data.button[CELL_PAD_BTN_OFFSET_PRESS_LEFT] = 0xff; }
  if (b1 & 0x10) *digit1 |= CELL_PAD_CTRL_SELECT;
  if (b1 & 0x20) *digit1 |= CELL_PAD_CTRL_START;
  if (b1 & 0x40) *digit1 |= CELL_PAD_CTRL_L3;
  if (b1 & 0x80) *digit1 |= CELL_PAD_CTRL_R3;
  if (b2 & 0x01) *digit0 |= CELL_PAD_CTRL_LDD_PS;

  if (b0 & 0x10) { *digit2 |= CELL_PAD_CTRL_SQUARE; data.button[CELL_PAD_BTN_OFFSET_PRESS_SQUARE] = 0xff; }
  if (b0 & 0x20) { *digit2 |= CELL_PAD_CTRL_CROSS; data.button[CELL_PAD_BTN_OFFSET_PRESS_CROSS] = 0xff; }
  if (b0 & 0x40) { *digit2 |= CELL_PAD_CTRL_CIRCLE; data.button[CELL_PAD_BTN_OFFSET_PRESS_CIRCLE] = 0xff; }
  if (b0 & 0x80) { *digit2 |= CELL_PAD_CTRL_TRIANGLE; data.button[CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE] = 0xff; }
  if (b1 & 0x01) { *digit2 |= CELL_PAD_CTRL_L1; data.button[CELL_PAD_BTN_OFFSET_PRESS_L1] = 0xff; }
  if (b1 & 0x02) { *digit2 |= CELL_PAD_CTRL_R1; data.button[CELL_PAD_BTN_OFFSET_PRESS_R1] = 0xff; }
  if ((b1 & 0x04) || l2) *digit2 |= CELL_PAD_CTRL_L2;
  if ((b1 & 0x08) || r2) *digit2 |= CELL_PAD_CTRL_R2;
  data.button[CELL_PAD_BTN_OFFSET_PRESS_L2] = l2;
  data.button[CELL_PAD_BTN_OFFSET_PRESS_R2] = r2;

  remap_apply(unit->number, &data, touchpad);
  xpad_analog_apply(&analog_config, &data);
  cellPadLddDataInsert(handle[unit->number], &data);
}

static int32_t pspad_read_input(XPAD_UNIT_t *unit, void *data) {
  uint8_t *out = (uint8_t *)data;
  uint8_t *queued, *buttons;
  int32_t latest, have_report = 0;
  uint32_t edges;

  if (unit == NULL) return(-1);
  block(unit->rb_mutex);
  if (unit->rblen > 0) {
    latest = unit->wp - 1;
    if (latest < 0) latest = RINGBUF_SIZE - 1;
    queued = &unit->ringbuf[latest][0];
    if (queued[1] >= 12 && queued[2] == 0x01) {
      memcpy(out, &queued[2], queued[1]);
      if (unit->xtype == PTYPE_PS4) {
        buttons = &out[5];
        if (unit->pending_trig_l > out[8]) out[8] = unit->pending_trig_l;
        if (unit->pending_trig_r > out[9]) out[9] = unit->pending_trig_r;
      } else {
        buttons = &out[8];
        if (unit->pending_trig_l > out[5]) out[5] = unit->pending_trig_l;
        if (unit->pending_trig_r > out[6]) out[6] = unit->pending_trig_r;
      }
      edges = unit->pending_ps_press_edges;
      buttons[0] |= (uint8_t)(edges & 0xf0);
      buttons[1] |= (uint8_t)(edges >> 8);
      buttons[2] |= (uint8_t)(edges >> 16);
      unit->pending_ps_press_edges = 0;
      unit->pending_trig_l = 0;
      unit->pending_trig_r = 0;
      have_report = 1;
    }
    unit->rp = unit->wp;
    unit->rblen = 0;
  }
  unblock(unit->rb_mutex);
  if (have_report && unit->number >= 0 && handle[unit->number] >= 0) {
    pspad_read_report(unit, out);
  }
  return(0);
}

/* Input-only first pass. Leaving Sony output reports untouched is safer on
   HEN and still provides the requested remap; rumble/lightbar can be added
   after the input path has been validated on real hardware. */
static int32_t pspad_set_led(XPAD_UNIT_t *unit, uint8_t led) {
  (void)unit; (void)led;
  return(CELL_OK);
}

static int32_t pspad_set_rumble(XPAD_UNIT_t *unit, uint8_t lval, uint8_t rval) {
  (void)unit; (void)lval; (void)rval;
  return(CELL_OK);
}
// end of wired Sony controller specific methods

// start of wireless controller specific methods
static int32_t get_device_desc(int32_t dev_id, void *p) {
  (void) dev_id;
  UsbDeviceDescriptor *desc = (UsbDeviceDescriptor *)p;

  // do nothing
  return(CELL_OK);
}

static int32_t get_configration_desc(int32_t dev_id, void *p) {
  (void) dev_id;
  UsbConfigurationDescriptor *desc = (UsbConfigurationDescriptor *)p;
  uint16_t wTotalLength;
  wTotalLength = SWAP16(desc->wTotalLength);

  // do nothing
  return(CELL_OK);
}

static int32_t get_interface_desc(int32_t dev_id, void *p) {
  (void) dev_id;
  UsbInterfaceDescriptor *desc = (UsbInterfaceDescriptor *)p;

  // do nothing
  return(CELL_OK);
}

static int32_t get_endpoint_desc(int32_t dev_id, void *p) {
  (void) dev_id;
  UsbEndpointDescriptor *edesc = (UsbEndpointDescriptor *)p;
  int32_t payload;
  XPAD_UNIT_t *unit;

  if (edesc->bEndpointAddress == 0x81 || edesc->bEndpointAddress == 0x83 || edesc->bEndpointAddress == 0x85 || edesc->bEndpointAddress == 0x87) {
    payload = SWAP16(edesc->wMaxPacketSize);
    if ((unit = unit_alloc(dev_id, payload, (edesc->bEndpointAddress - 0x01) & 0x0f, 0, XTYPE_XBOX360W)) == NULL) {
      return(CELL_USBD_ATTACH_FAILED);
    }
    unit->name = "Wireless pad";
    if ((unit->c_pipe = cellUsbdOpenPipe(dev_id, NULL)) < 0) {
      unit_release(unit);
      return(CELL_USBD_ATTACH_FAILED);
    }
    if ((unit->i_pipe = cellUsbdOpenPipe(dev_id, edesc)) < 0) {
      unit_release(unit);
      return(CELL_USBD_ATTACH_FAILED);
    }
    edesc->bEndpointAddress &= 0x0f; // XBox controller out endpoint, ex: 0x81 & 0x0f == 0x01
    if ((unit->o_pipe = cellUsbdOpenPipe(dev_id, edesc)) < 0) {
      unit_release(unit);
      return(CELL_USBD_ATTACH_FAILED);
    }

    // endpoint found; no pad slot or LDD pad is claimed yet - that only
    // happens when the receiver reports a controller connection (0x08 0x80)
    if (unit_submit_begin(unit) &&
        cellUsbdSetConfiguration(unit->c_pipe, 1, set_config_done, unit) != CELL_OK) {
      unit_submit_end(unit);
    }
  }
  return(CELL_USBD_ATTACH_SUCCEEDED);
}

static int32_t xpadw_probe(int32_t dev_id) {
  uint16_t idVendor, idProduct;
  uint32_t i;
  UsbDeviceDescriptor *ddesc;
  UsbInterfaceDescriptor *idesc;

  int32_t free_units = 0;

  // the receiver needs one endpoint unit per possible pad (no pad slots yet)
  block(xpad_mutex);
  for (i = 0; i < MAX_UNIT_NUM; i++) {
    if (XPAD.units[i] == NULL) {
      free_units++;
    }
  }
  unblock(xpad_mutex);
  if (free_units < MAX_XPADW_NUM) {
    return(CELL_USBD_PROBE_FAILED);
  }
  if ((ddesc = (UsbDeviceDescriptor *)cellUsbdScanStaticDescriptor(dev_id, NULL, USB_DESCRIPTOR_TYPE_DEVICE)) == NULL) {
    return(CELL_USBD_PROBE_FAILED);
  }
  idesc = (UsbInterfaceDescriptor *)ddesc;
  if ((idesc = (UsbInterfaceDescriptor *)cellUsbdScanStaticDescriptor(dev_id, idesc, USB_DESCRIPTOR_TYPE_INTERFACE)) == NULL) {
    return(CELL_USBD_PROBE_FAILED);
  }

  // make sure product id and vendor id are valid
  idVendor = SWAP16(ddesc->idVendor);
  idProduct = SWAP16(ddesc->idProduct);
  for (i = 0; i < MAX_XPADW_DEV_NUM; i++) {
    if (xpadw_info[i].vid == idVendor && xpadw_info[i].pid == idProduct) {
      return(CELL_USBD_PROBE_SUCCEEDED);
    }
  }
  for (i = 0; i < (uint32_t)extra_count; i++) {
    if (extra_info[i].xtype == XTYPE_XBOX360W && extra_info[i].vid == idVendor && extra_info[i].pid == idProduct) {
      return(CELL_USBD_PROBE_SUCCEEDED);
    }
  }
  return(CELL_USBD_PROBE_FAILED);
}

static int xpadw_attach(int32_t dev_id) {
  uint8_t* desc = 0;
  uint32_t i;
  UsbDeviceDescriptor *ddesc;
  uint16_t idVendor = 0, idProduct = 0;
  const char *rname;
  char msg[96], *m;

  if ((ddesc = (UsbDeviceDescriptor *)cellUsbdScanStaticDescriptor(dev_id, NULL, USB_DESCRIPTOR_TYPE_DEVICE)) != NULL) {
    idVendor = SWAP16(ddesc->idVendor);
    idProduct = SWAP16(ddesc->idProduct);
  }
  rname = find_device_name(idVendor, idProduct);
  m = msg;
  m = append_str(m, "XPAD Rev: ");
  m = append_str(m, rname ? rname : "Wireless receiver");
  m = append_str(m, " (");
  m = append_hex16(m, idVendor);
  m = append_str(m, ":");
  m = append_hex16(m, idProduct);
  m = append_str(m, ") connected");
  show_msg(msg);

  // Xbox 360 wireless receivers have 4 endpoints (1 per controller)
  // all 4 need to be listened to at all times in case of controller connection/disconnection
  // traverse through its usb device descriptor and find the endpoints
  while (1) {
    if ((desc = cellUsbdScanStaticDescriptor(dev_id, desc, 0)) == 0) {
        break;
    }
    for (i = 0; i < DESCRIPTOR_TABLE_SIZE; i++) {
      if (descriptor_table[i].bDescriptorType == desc[1]) {
        break;
      }
    }
    if (i != DESCRIPTOR_TABLE_SIZE) {
      descriptor_table[i].dump_descriptor(dev_id, desc);
    }
  }
  return(CELL_USBD_ATTACH_SUCCEEDED);
}

static int32_t xpadw_detach(int32_t dev_id) {
  int32_t r;

  // Xbox wireless receiver has been unplugged
  // disconnect all virtual controllers associated to it
  r = xpadw_detach_all();
  show_msg((char *)"XPAD Rev: Wireless receiver disconnected");
  return(r);
}

static int32_t xpadw_detach_all(void) {
  int32_t i;
  XPAD_UNIT_t *unit;

  // detach all wireless controllers
  block(xpad_mutex);
  for (i = 0; i < MAX_UNIT_NUM; i++) {
    if ((unit = XPAD.units[i]) != NULL && unit->xtype == XTYPE_XBOX360W) {
      XPAD.units[i] = NULL;
      unregister_ldd_controller(unit);
      unit_free(unit);
    }
  }
  unblock(xpad_mutex);
  return(CELL_USBD_DETACH_SUCCEEDED);
}

static void xpadw_read_report(int32_t id, uint8_t *readBuf) {
  uint16_t *digit0, *digit1, *digit2,
           *analog_rx, *analog_ry, *analog_lx, *analog_ly,
           *press_l2, *press_r2, *press_l1, *press_r1,
           *press_up, *press_down, *press_left, *press_right,
           *press_cross, *press_square, *press_circle, *press_triangle;
  XBOX360W_IN_REPORT *report;
  CellPadData data;

  report = (XBOX360W_IN_REPORT *)readBuf;
  viewer_update_x360w(id, report);
  memset(&data, 0, sizeof(CellPadData));
  data.len = 24;

  // map location of each button in virtual pad's data
  digit0 = &data.button[0];
  digit1 = &data.button[CELL_PAD_BTN_OFFSET_DIGITAL1];
  digit2 = &data.button[CELL_PAD_BTN_OFFSET_DIGITAL2];
  analog_rx = &data.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X];
  analog_ry = &data.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y];
  analog_lx = &data.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X];
  analog_ly = &data.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y];
  press_l2 = &data.button[CELL_PAD_BTN_OFFSET_PRESS_L2];
  press_r2 = &data.button[CELL_PAD_BTN_OFFSET_PRESS_R2];
  press_l1 = &data.button[CELL_PAD_BTN_OFFSET_PRESS_L1];
  press_r1 = &data.button[CELL_PAD_BTN_OFFSET_PRESS_R1];
  press_right = &data.button[CELL_PAD_BTN_OFFSET_PRESS_RIGHT];
  press_left = &data.button[CELL_PAD_BTN_OFFSET_PRESS_LEFT];
  press_up = &data.button[CELL_PAD_BTN_OFFSET_PRESS_UP];
  press_down = &data.button[CELL_PAD_BTN_OFFSET_PRESS_DOWN];
  press_cross = &data.button[CELL_PAD_BTN_OFFSET_PRESS_CROSS];
  press_square = &data.button[CELL_PAD_BTN_OFFSET_PRESS_SQUARE];
  press_circle = &data.button[CELL_PAD_BTN_OFFSET_PRESS_CIRCLE];
  press_triangle = &data.button[CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE];

  // set default controller values
  data.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] = 0x0080;
  data.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] = 0x0080;
  data.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X] = 0x0080;
  data.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y] = 0x0080;
  data.button[CELL_PAD_BTN_OFFSET_SENSOR_X] = 0x0200;
  data.button[CELL_PAD_BTN_OFFSET_SENSOR_Y] = 0x0200;
  data.button[CELL_PAD_BTN_OFFSET_SENSOR_Z] = 0x0200;
  data.button[CELL_PAD_BTN_OFFSET_SENSOR_G] = 0x0200;

  // read values from Xbox controller and map it to the virtual pad
  *digit0 = (report->buttons & btnXbox) ? *digit0 | CELL_PAD_CTRL_LDD_PS : *digit0 & ~CELL_PAD_CTRL_LDD_PS;
  *digit1 = (report->buttons & btnDigiLeft) ? *digit1 | CELL_PAD_CTRL_LEFT : *digit1 & ~CELL_PAD_CTRL_LEFT;
  *digit1 = (report->buttons & btnDigiDown) ? *digit1 | CELL_PAD_CTRL_DOWN : *digit1 & ~CELL_PAD_CTRL_DOWN;
  *digit1 = (report->buttons & btnDigiRight) ? *digit1 | CELL_PAD_CTRL_RIGHT : *digit1 & ~CELL_PAD_CTRL_RIGHT;
  *digit1 = (report->buttons & btnDigiUp) ? *digit1 | CELL_PAD_CTRL_UP : *digit1 & ~CELL_PAD_CTRL_UP;
  *digit1 = (report->buttons & btnStart) ? *digit1 | CELL_PAD_CTRL_START : *digit1 & ~CELL_PAD_CTRL_START;
  *digit1 = (report->buttons & btnHatRight) ? *digit1 | CELL_PAD_CTRL_R3 : *digit1 & ~CELL_PAD_CTRL_R3;
  *digit1 = (report->buttons & btnHatLeft) ? *digit1 | CELL_PAD_CTRL_L3 : *digit1 & ~CELL_PAD_CTRL_L3;
  *digit1 = (report->buttons & btnBack) ? *digit1 | CELL_PAD_CTRL_SELECT : *digit1 & ~CELL_PAD_CTRL_SELECT;
  *digit2 = (report->buttons & btnX) ? *digit2 | CELL_PAD_CTRL_SQUARE : *digit2 & ~CELL_PAD_CTRL_SQUARE;
  *digit2 = (report->buttons & btnA) ? *digit2 | CELL_PAD_CTRL_CROSS : *digit2 & ~CELL_PAD_CTRL_CROSS;
  *digit2 = (report->buttons & btnB) ? *digit2 | CELL_PAD_CTRL_CIRCLE : *digit2 & ~CELL_PAD_CTRL_CIRCLE;
  *digit2 = (report->buttons & btnY) ? *digit2 | CELL_PAD_CTRL_TRIANGLE : *digit2 & ~CELL_PAD_CTRL_TRIANGLE;
  *digit2 = (report->buttons & btnShoulderRight) ? *digit2 | CELL_PAD_CTRL_R1 : *digit2 & ~CELL_PAD_CTRL_R1;
  *digit2 = (report->buttons & btnShoulderLeft) ? *digit2 | CELL_PAD_CTRL_L1 : *digit2 & ~CELL_PAD_CTRL_L1;
  *digit2 = (report->trigL > 0) ? *digit2 | CELL_PAD_CTRL_L2 : *digit2 & ~CELL_PAD_CTRL_L2;
  *digit2 = (report->trigR > 0) ? *digit2 | CELL_PAD_CTRL_R2 : *digit2 & ~CELL_PAD_CTRL_R2;

  // emulate pressure values except for L2 and R2, button presses correspond to max sensitivity value
  *press_l2 = (report->trigL);
  *press_r2 = (report->trigR);
  *press_l1 = (report->buttons & btnShoulderLeft) ? 0xFF : 0;
  *press_r1 = (report->buttons & btnShoulderRight) ? 0xFF : 0;
  *press_up = (report->buttons & btnDigiUp) ? 0xFF : 0;
  *press_down = (report->buttons & btnDigiDown) ? 0xFF : 0;
  *press_left = (report->buttons & btnDigiLeft) ? 0xFF : 0;
  *press_right = (report->buttons & btnDigiRight) ? 0xFF : 0;
  *press_square = (report->buttons & btnX) ? 0xFF : 0;
  *press_circle = (report->buttons & btnB) ? 0xFF : 0;
  *press_cross = (report->buttons & btnA) ? 0xFF : 0;
  *press_triangle = (report->buttons & btnY) ? 0xFF : 0;

  // PS3 pads use 8 bit values for each axis while Xbox pads use 16 bit
  // convert Xbox analog values for PS3 compatibility 
  *analog_rx = (report->right.x-0x80) & 0x00FF;
  *analog_ry = ((report->right.y ^ 0xFF)-0x80) & 0x00FF;
  *analog_lx = (report->left.x-0x80) & 0x00FF;
  *analog_ly = ((report->left.y ^ 0xFF)-0x80) & 0x00FF;

  remap_apply(id, &data, 0);
  xpad_analog_apply(&analog_config, &data);
  // send pad data to virtual pad
  cellPadLddDataInsert(handle[id], &data);
}

static int32_t xpadw_read_input(XPAD_UNIT_t *unit, void *data) {
  unsigned char *p;
  unsigned char *pkt;
  unsigned char *xpadbuf;
  int32_t have_report = 0;
  int32_t pending = 0; /* +1 connect, -1 disconnect; last event wins */
  XBOX360W_IN_REPORT *report;

  p = (unsigned char *)data;
  if (unit == NULL) {
    return(-1);
  }

  // drain the whole ring buffer each tick; keep only the newest movement
  // report but honor every connect/disconnect event in order
  block(unit->rb_mutex);
  while (unit->rblen > 0) {
    xpadbuf = &unit->ringbuf[unit->rp][0];
    pkt = &xpadbuf[2];
    if (xpadbuf[1] >= 2 && pkt[0] == 0x08) {
      if (pkt[1] == 0x80) {
        pending = 1; // controller connected to receiver
      } else if (pkt[1] == 0x00) {
        pending = -1; // controller disconnected from receiver
        have_report = 0; // drop any report from before the disconnect
      }
    }
    report = (XBOX360W_IN_REPORT *)pkt;
    if ((xpadbuf[1] >= sizeof(XBOX360W_IN_REPORT)) && (pkt[1] == 0x01) &&
        (report->header.command == inReport) && (report->header.size == sizeof(XBOX360W_IN_REPORT))) {
      memcpy(p, pkt, xpadbuf[1]);
      have_report = 1;
    }
    if (++unit->rp >= RINGBUF_SIZE) {
      unit->rp = 0;
    }
    unit->rblen--;
  }
  unblock(unit->rb_mutex);

  // register/unregister outside the ring-buffer lock: registration sleeps
  // 10 ms and must not stall USB completion callbacks for other pads
  if (pending > 0) {
    if (unit->number < 0) { // newly connected pad
      if (register_ldd_controller(unit) == CELL_PAD_OK) {
        notify_pad_connected(unit);
      } else {
        show_msg((char *)"XPAD Rev: no free pad slot for wireless pad");
      }
    }
  } else if (pending < 0) {
    if (unit->number >= 0) { // pad was actually registered
      unregister_ldd_controller(unit);
      notify_pad_disconnected(unit);
    }
  }
  if (have_report && unit->number >= 0 && handle[unit->number] >= 0) {
    xpadw_read_report(unit->number, p);
  }
  return(0);
}

static int32_t xpadw_set_led(XPAD_UNIT_t *unit, uint8_t led) {
  uint8_t out[4] = {0x00, 0x00, 0x08, led | 0x40};

  if (write_xpad(unit, out, 4) < 0) {
    return(-1);
  }
  return(CELL_OK);
}

static int32_t xpadw_set_rumble(XPAD_UNIT_t *unit, uint8_t lval, uint8_t rval) {
  // full 12-byte wireless rumble command, matching the Linux xpad driver and
  // xboxdrv; a truncated payload leaves the motors unset on many receivers
  uint8_t out[12] = {0x00, 0x01, 0x0f, 0xc0, 0x00, lval, rval, 0x00, 0x00, 0x00, 0x00, 0x00};

  if (write_xpad(unit, out, 12) < 0) {
    return(-1);
  }
  return(CELL_OK);
}
// end of wireless controller specific methods

static int32_t check_pad_status(void) {
  int32_t i, cr, port, pad;
  XPAD_UNIT_t *unit;
  CellPadInfo2 pad_info2;

  cr = cellPadGetInfo2(&pad_info2);
  if (cr != CELL_PAD_OK) {
    return(-1);
  }
  for (pad = 0; pad < CELL_PAD_MAX_PORT_NUM; ++pad) {
    if (pad_info2.port_status[pad] & CELL_PAD_STATUS_ASSIGN_CHANGES) {

      // port assignments changed; re-send LEDs only for pads whose port
      // actually moved (only 4 LED patterns exist, so with more than 4
      // pads two controllers will show the same pattern)
      for (i = 0; i < MAX_UNIT_NUM; i++) {
        if ((unit = XPAD.units[i]) != NULL && unit->number >= 0 && handle[unit->number] >= 0) {
          port = cellPadLddGetPortNo(handle[unit->number]);
          if (port >= 0 && port != unit->last_port) {
            unit->last_port = port;
            unit->set_led(unit, xpad_led[port%4]);
          }
        }
      }
      break; // one refresh pass covers all pads
    }
  }
  return(0);
}

// Register one VID/PID with USBD, tolerating duplicates. Returns CELL_OK
// when registered or when this exact identity is already registered (a
// conflicting entry must not take the whole plugin down - the device it
// describes still works through whoever registered it first). Any other
// error is passed to the caller; CELL_USBD_ERROR_NOT_INITIALIZED means
// "retry the pass later", everything else is fatal.
static int32_t register_one(CellUsbdLddOps *ops, uint16_t vid, uint16_t pid, const char *name, uint8_t *lddreg) {
  int32_t r;

  if (*lddreg) {
    return(CELL_OK); // registered by an earlier pass; passes are idempotent
  }
  if (reg_budget_full) {
    // the LDD table already filled up earlier in this pass; every further
    // cellUsbdRegisterExtraLdd() would just return the same error, so skip
    // the call and record the entry as dropped
    reg_skipped_full++;
    return(CELL_OK);
  }
  ops->name = name;
  r = cellUsbdRegisterExtraLdd(ops, vid, pid);
  if (r == CELL_OK) {
    *lddreg = 1;
    xlog_dev("registered", vid, pid, 0);
    return(CELL_OK);
  }
  if (r == CELL_USBD_ERROR_LDD_ALREADY_REGISTERED) {
    reg_conflicts++;
    xlog_dev("skipped, LDD already registered", vid, pid, r);
    return(CELL_OK);
  }
  if (r == CELL_USBD_ERROR_LDD_TABLE_FULL) {
    // cellUsbd is out of LDD slots. This is non-fatal: keep every device
    // that already registered working and leave the plugin active. Latch a
    // flag so the rest of the pass stops hammering USBD, and count what we
    // had to drop for the summary line in init_usb().
    reg_budget_full = 1;
    reg_skipped_full++;
    xlog_dev("LDD table full, entry not registered", vid, pid, r);
    return(CELL_OK);
  }
  reg_fail_vid = vid;
  reg_fail_pid = pid;
  xlog_dev("register FAILED", vid, pid, r);
  return(r);
}

// Register every built-in and user-supplied controller VID/PID with USBD.
// Safe to call repeatedly: entries that already registered are skipped.
static int32_t register_devices(void) {
  int32_t r, i;

  // Sony pads first: their touchpad-capable HID path must fit in the small
  // extra-LDD table even when the long legacy XInput list is installed.
  for (i = 0; i < MAX_PSPAD_DEV_NUM; i++) {
    if ((r = register_one(&pspad_ops, pspad_info[i].vid, pspad_info[i].pid,
                          pspad_info[i].name, &pspad_lddreg[i])) != CELL_OK) {
      return(r);
    }
  }

  // register wired Xbox controller device types
  for (i = 0; i < MAX_XPAD_DEV_NUM; i++) {
    if ((r = register_one(&xpad_ops, xpad_info[i].vid, xpad_info[i].pid, xpad_info[i].name, &xpad_lddreg[i])) != CELL_OK) {
      return(r);
    }
  }

  // register wireless Xbox controller device types
  for (i = 0; i < MAX_XPADW_DEV_NUM; i++) {
    if ((r = register_one(&xpadw_ops, xpadw_info[i].vid, xpadw_info[i].pid, xpadw_info[i].name, &xpadw_lddreg[i])) != CELL_OK) {
      return(r);
    }
  }

  // register user-supplied device types
  for (i = 0; i < extra_count; i++) {
    if (extra_info[i].xtype == XTYPE_XBOX360W) {
      r = register_one(&xpadw_ops, extra_info[i].vid, extra_info[i].pid, extra_info[i].name, &extra_lddreg[i]);
    } else if (extra_info[i].xtype == PTYPE_PS4 || extra_info[i].xtype == PTYPE_PS5) {
      r = register_one(&pspad_ops, extra_info[i].vid, extra_info[i].pid, extra_info[i].name, &extra_lddreg[i]);
    } else {
      r = register_one(&xpad_ops, extra_info[i].vid, extra_info[i].pid, extra_info[i].name, &extra_lddreg[i]);
    }
    if (r != CELL_OK) {
      return(r);
    }
  }
  return(CELL_OK);
}

// undo every registration register_devices() made (and only those)
static void unregister_devices(void) {
  int32_t i;

  for (i = 0; i < MAX_PSPAD_DEV_NUM; i++) {
    if (pspad_lddreg[i]) {
      cellUsbdUnregisterExtraLdd(&pspad_ops);
      pspad_lddreg[i] = 0;
    }
  }
  for (i = 0; i < MAX_XPAD_DEV_NUM; i++) {
    if (xpad_lddreg[i]) {
      cellUsbdUnregisterExtraLdd(&xpad_ops);
      xpad_lddreg[i] = 0;
    }
  }
  for (i = 0; i < MAX_XPADW_DEV_NUM; i++) {
    if (xpadw_lddreg[i]) {
      cellUsbdUnregisterExtraLdd(&xpadw_ops);
      xpadw_lddreg[i] = 0;
    }
  }
  for (i = 0; i < extra_count; i++) {
    if (extra_lddreg[i]) {
      if (extra_info[i].xtype == XTYPE_XBOX360W) {
        cellUsbdUnregisterExtraLdd(&xpadw_ops);
      } else if (extra_info[i].xtype == PTYPE_PS4 || extra_info[i].xtype == PTYPE_PS5) {
        cellUsbdUnregisterExtraLdd(&pspad_ops);
      } else {
        cellUsbdUnregisterExtraLdd(&xpad_ops);
      }
      extra_lddreg[i] = 0;
    }
  }
}

// number of times to retry registration when USBD is not ready yet
// (CELL_USBD_ERROR_NOT_INITIALIZED, 0x80110001), and the delay between
// tries. The pre-init boot sleep usually makes USBD ready by the first try;
// the retry is a safety net for slow/cold boots where a single fixed delay
// is not enough (up to ~30 s of extra grace here). Retry passes only touch
// entries that have not registered yet.
#define USB_INIT_RETRIES 10
#define USB_INIT_RETRY_DELAY_SEC 3

static int32_t init_usb(void) {
  int32_t r, attempt;
  sys_mutex_attribute_t mutex_attr;

  sys_mutex_attribute_initialize(mutex_attr);
  if ((r = sys_mutex_create(&xpad_mutex, &mutex_attr)) != CELL_OK) {
    xlog_code("init_usb: mutex create failed", r);
    return(r);
  }
  if ((r = sys_mutex_create(&viewer_mutex, &mutex_attr)) != CELL_OK) {
    xlog_code("init_usb: viewer mutex create failed", r);
    sys_mutex_destroy(xpad_mutex);
    return(r);
  }
  memset(viewer_state, 0, sizeof(viewer_state));
  memset(viewer_sequence, 0, sizeof(viewer_sequence));

  // initialize all controller handlers
  memset(handle, -1, sizeof(int32_t) * CELL_PAD_MAX_PORT_NUM);

  // pick up user-supplied VID/PIDs before registering anything
  load_extra_devices();
  load_remap_config();
  load_analog_config();

  memset(&XPAD, 0, sizeof(XPAD));

  // Register with USBD. If the subsystem is still coming up at boot the
  // call returns CELL_USBD_ERROR_NOT_INITIALIZED (0x80110001); wait and
  // retry rather than depend on the pre-init delay being exactly long
  // enough. Any other error is a real failure and aborts. Note attach
  // callbacks for connected controllers start firing mid-pass, as soon as
  // their own VID/PID is registered.
  for (attempt = 0; ; attempt++) {
    r = register_devices();
    if (r != CELL_USBD_ERROR_NOT_INITIALIZED || attempt >= USB_INIT_RETRIES) {
      break;
    }
    xlog_code("init_usb: USBD not ready, retrying in 3s", r);
    sys_timer_sleep(USB_INIT_RETRY_DELAY_SEC);
  }
  if (r != CELL_OK) {
    // roll back: earlier entries may have registered - and a controller may
    // already have attached through one of them - before the failing entry
    xlog_code("init_usb: registration aborted", r);
    xpad_detach_all();
    xpadw_detach_all();
    pspad_detach_all();
    unregister_devices();
    sys_mutex_destroy(viewer_mutex);
    sys_mutex_destroy(xpad_mutex);
    return(r);
  }
  if (reg_conflicts > 0) {
    xlog_code("init_usb: device IDs skipped (already registered elsewhere)", reg_conflicts);
  }
  if (reg_skipped_full > 0) {
    // not an error: the cellUsbd LDD table ran out of room. Everything up to
    // that point registered and works; only the low-priority tail was dropped
    xlog_code("init_usb: LDD table full, low-priority device IDs skipped", reg_skipped_full);
  }

  // best-effort: a failure here only means rumble is unavailable, so it must
  // not abort USB init (input still works)
  create_rumble_queue();
  return(CELL_OK);
}

static int32_t shutdown_usb(void) {
  int32_t r;

  destroy_rumble_queue();

  // unregister exactly the registrations register_devices() made so
  // reloading the plugin neither leaks LDD registrations nor unregisters
  // entries that were never registered (e.g. skipped conflicts)
  unregister_devices();
  if ((r = sys_mutex_destroy(xpad_mutex)) != CELL_OK) {
    return(r);
  }
  if ((r = sys_mutex_destroy(viewer_mutex)) != CELL_OK) {
    return(r);
  }
  return(CELL_OK);
}

static void xpadd_thread(uint64_t arg) {
  unsigned char xpad_data[MAX_XPAD_DATA_LEN];
  int32_t i, r, status_tick = 0;
  XPAD_UNIT_t *unit;
#if XPAD_AUTO_INJECT
  uint64_t t0;
#endif

  xlog_init();

  // wait for the system to finish booting: when this plugin is loaded at
  // startup (via boot_plugins.txt), the USBD subsystem and VSH notification
  // service may not be ready yet. Without this delay, cellUsbdRegisterExtraLdd
  // can return CELL_USBD_ERROR_NOT_INITIALIZED (0x80110001). 10 seconds is
  // the established safe margin used by other webMAN-era VSH plugins; it also
  // lets the VSH notification service come up so the show_msg() calls below
  // are visible. This delay alone is not a guarantee, though (a cold/slow
  // boot can need longer), so init_usb() additionally retries registration
  // while USBD reports not-initialized.
  xlog("plugin thread started, waiting 10s for boot to settle");
  sys_timer_sleep(10);

  xlog("starting USB init");
  r = init_usb();

  if (r != CELL_OK) {
    // do not fail silently: without this the user just sees a plugin
    // that does nothing
    char msg[96], *m;
    m = msg;
    m = append_str(m, "XPAD Rev: USB init failed (0x");
    m = append_hex32(m, (uint32_t)r);
    if (reg_fail_vid != 0 || reg_fail_pid != 0) {
      m = append_str(m, " at ");
      m = append_hex16(m, reg_fail_vid);
      m = append_str(m, ":");
      m = append_hex16(m, reg_fail_pid);
    }
    m = append_str(m, "), plugin inactive");
    show_msg(msg);
    sys_ppu_thread_exit(0);
    return;
  }
  show_msg((char *)"XPAD Rev v1.0.0 Loaded!");
  xlog_code("USB init OK, entering poll loop; conflicts skipped", reg_conflicts);
  if (extra_count > 0 || extra_skipped > 0) {
    char msg[96], *m;
    m = msg;
    m = append_str(m, "XPAD Rev: ");
    m = append_int(m, extra_count);
    m = append_str(m, " custom device(s) loaded");
    if (extra_skipped > 0) {
      m = append_str(m, ", ");
      m = append_int(m, extra_skipped);
      m = append_str(m, " line(s) skipped");
    }
    show_msg(msg);
  }
  if (remap_config.loaded) {
    char msg[96], *m;
    m = msg;
    m = append_str(m, "XPAD Rev: remap profile ");
    m = append_int(m, remap_config.profile);
    m = append_str(m, remap_config.enabled ? " enabled (" : " ready, disabled (");
    m = append_int(m, remap_config.rules);
    m = append_str(m, " rule(s))");
    show_msg(msg);
  } else if (remap_config.profile > 0) {
    show_msg((char *)"XPAD Rev: selected remap profile has no valid rules");
  }
  if (analog_config.enabled) {
    char msg[80], *m;
    m = msg;
    m = append_str(m, "XPAD Rev: analog DS3 curve enabled (");
    m = append_int(m, analog_config.saturation);
    m = append_str(m, "% saturation)");
    show_msg(msg);
  }
  running = 1;
#if !XPAD_AUTO_INJECT
  xlog("HEN-safe build: automatic game injection disabled; explicit controller loader only");
  xlog("HEN-safe VSH remapper active; no DEX/game-process hook is used");
#endif
#if XPAD_MANUAL_GAME_LOADER
  xlog("XPAD Revolution v1.0.0 manual game loader armed: normalized USB + libpad BT, hold SELECT+L3+R3 for 0.8s");
#endif
  if (sys_ppu_thread_create(&viewer_thread_id, viewer_network_thread, NULL, 1000,
                            0x10000, SYS_PPU_THREAD_CREATE_JOINABLE,
                            VIEWER_THREAD_NAME) != CELL_OK) {
    viewer_thread_id = (sys_ppu_thread_t)-1;
    xlog("viewer: network thread create failed; controller input remains active");
  } else {
    xlog("viewer: automatic PC discovery listening on UDP 39001");
  }
#if XPAD_AUTO_INJECT
  // Auto-injection is opt-in (the LOADER-WEDGE FIX note above): a hung
  // LOAD_PROC_MODULE wedges the lv2 loader and freezes every subsequent
  // launch console-wide, so unless the user has explicitly created the opt-in
  // file the plugin must not issue PS3MAPI calls at all. When opted in, the
  // work still runs on its own expendable thread so a hang at least cannot
  // stall the input pump below (the INPUT-DEATH FIX note above). Low
  // priority: injection is a background convenience, input is not. If the
  // thread cannot be created, run without auto-injection rather than risk it.
  if (!auto_inject_opted_in()) {
    xlog("auto-inject: disabled (no /dev_hdd0/plugins/xpad_auto_inject.txt); use webMAN Game Plugins for rumble");
  } else if (sys_ppu_thread_create(&inject_thread_id, inject_thread, NULL, 1000, 0x2000,
                                   SYS_PPU_THREAD_CREATE_JOINABLE, INJECT_THREAD_NAME) != CELL_OK) {
    inject_thread_id = (sys_ppu_thread_t)-1;
    xlog("auto-inject: thread create failed, auto-injection disabled");
  } else {
    xlog("auto-inject: enabled (opt-in file present)");
  }
#endif
  while (running) {
    sys_timer_usleep(1000 * 2); /* 2 ms / 500 Hz input worker. */
    block(xpad_mutex);
    for (i = 0; i < MAX_UNIT_NUM; i++) {
      if ((unit = XPAD.units[i]) != NULL) {
        unit->read_input(unit, xpad_data);
      }
    }
    if (++status_tick >= 6) {
      check_pad_status(); /* Keep port/LED housekeeping near 12 ms. */
      status_tick = 0;
    }
    unblock(xpad_mutex);

    // forward rumble events from the game-process plugin to the USB pads
    // (route_rumble() takes xpad_mutex itself, so drain outside the lock)
    drain_rumble_queue();
    poll_game_ready_file();
#if XPAD_MANUAL_GAME_LOADER
    poll_manual_game_loader();
#endif

#if XPAD_AUTO_INJECT
    // watchdog the injection thread: if a load_module call has been in flight
    // far longer than any healthy call takes, it is hung in the kernel. Input
    // (this thread) is unaffected; log the breadcrumb once so the debug log
    // explains why rumble auto-setup went missing this session.
    t0 = inject_call_t0;
    if (!inject_hang_logged && t0 != 0 && time_now_usec() - t0 > XPAD_INJECT_HANG_USEC) {
      inject_hang_logged = 1;
      xlog("auto-inject: load_module stuck >15s (kernel hang); input unaffected, rumble auto-inject lost this session");
    }
#endif
  }

  // exiting...
  if (viewer_thread_id != (sys_ppu_thread_t)-1) {
    uint64_t viewer_exit_code;
    sys_ppu_thread_join(viewer_thread_id, &viewer_exit_code);
    viewer_thread_id = (sys_ppu_thread_t)-1;
  }
  xpad_detach_all();
  xpadw_detach_all();
  pspad_detach_all();
  shutdown_usb();
  sys_ppu_thread_exit(0);
}

int xpadd_start(uint64_t arg) {
  sys_ppu_thread_create(&thread_id, xpadd_thread, NULL, -0x1d8, 0x2000, SYS_PPU_THREAD_CREATE_JOINABLE, THREAD_NAME);
  _sys_ppu_thread_exit(0);
  return(SYS_PRX_RESIDENT);
}

static void xpadd_stop_thread(uint64_t arg) {
  uint64_t exit_code;
#if XPAD_AUTO_INJECT
  int32_t i;
#endif

  running = 0;
  sys_timer_usleep(500000);
#if XPAD_AUTO_INJECT
  if (inject_thread_id != (sys_ppu_thread_t)-1) {
    // the injection thread sees running == 0 on its next 2 s wake and exits.
    // Give an in-flight load_module call a bounded window to return first: if
    // it is hung in the kernel the thread can never exit, and joining it would
    // turn a lost-rumble session into a plugin unload that hangs forever.
    for (i = 0; i < 50 && inject_call_t0 != 0; i++) {
      sys_timer_usleep(100000);
    }
    if (inject_call_t0 == 0) {
      sys_ppu_thread_join(inject_thread_id, &exit_code);
    }
    inject_thread_id = (sys_ppu_thread_t)-1;
  }
#endif
  if (thread_id != (sys_ppu_thread_t)-1) {
    sys_ppu_thread_join(thread_id, &exit_code);
  }
  show_msg("XPAD Unloaded!");
  sys_ppu_thread_exit(0);
}

int xpadd_stop(void) {
  sys_ppu_thread_t t;
  uint64_t exit_code;

  sys_ppu_thread_create(&t, xpadd_stop_thread, 0, 0, 0x2000, SYS_PPU_THREAD_CREATE_JOINABLE, STOP_THREAD_NAME);
  sys_ppu_thread_join(t, &exit_code);
  _sys_ppu_thread_exit(0);
  return(SYS_PRX_STOP_OK);
}
