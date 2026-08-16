#ifndef __XPAD_RUMBLE_H__
#define __XPAD_RUMBLE_H__

// Shared rumble IPC contract between the two plugins that make up ps3xpad's
// rumble support. Keeping it in one header means the VSH driver and the
// game-process sender can never drift out of sync on the key or wire format.
//
//   xpad.sprx       (VSH)  - USB/LDD driver; owns the USB pads. Creates the
//                            named lv2 event queue below, drains it every
//                            poll tick and forwards each event to the USB
//                            rumble command of the pad on that port.
//   xpad_game.sprx  (game) - loaded into the game process; hooks the pad
//                            actuator call (cellPadSetActDirect) and, on
//                            change, connects an event port to RUMBLE_IPC_KEY
//                            and sends one event per actuator update.
//
// This split is required because the OS never delivers cellPadSetActDirect()
// actuator values to an LDD (virtual) pad, so the driver has no way to see
// them from VSH; the game-side hook is the only place they are visible.
// See TODO.md section 8 for the full design and rationale.

// lv2 event-queue / event-port IPC key, ASCII "xpadrumb". Non-zero so the
// queue is an IPC (cross-process) queue that the game side can reach with
// sys_event_port_connect_ipc().
#define RUMBLE_IPC_KEY   0x7870616472756d62ULL // "xpadrumb"

// Reserved data1 value used once by xpad_game.sprx after its hooks are
// installed. The VSH half turns this acknowledgement into an on-screen toast,
// so the user sees confirmation only after the game module really started.
#define XPAD_EV_GAME_READY 0x5850414447414D45ULL // "XPADGAME"

// Filesystem fallback for the ready acknowledgement. Some HEN/Cobra builds
// allow the game SPRX to start normally but reject cross-process event-port
// connections. The game writes this tiny marker after installing its hooks;
// the VSH plugin consumes and removes it. Rumble still prefers the event
// queue, while the activation toast remains reliable on those firmwares.
#define XPAD_GAME_READY_PATH  "/dev_hdd0/plugins/ps3xpad/xpad_game.ready"
#define XPAD_GAME_READY_MAGIC 0x58505244U // "XPRD"

typedef struct xpad_game_ready_file {
  uint32_t magic;
  uint32_t hook_count;
  uint32_t process_id;
} XPAD_GAME_READY_FILE_t;

/* Size written by v4.0.2. The VSH reader accepts it so replacing the two
   modules in either order cannot turn an old ready marker into an error. */
#define XPAD_GAME_READY_V1_SIZE 8U

// Queued events the VSH side buffers before the game side's send blocks/drops.
// Rumble updates are low-rate (a handful per second per pad), so a shallow
// queue is plenty and keeps latency low.
#define RUMBLE_QUEUE_DEPTH 8

// Wire format: each sys_event carries three data words.
//   data1 = pad port   (libpad port number, 0..6). This is the game's
//                       cellPadSetActDirect() port_no on the sender side and
//                       equals the driver's cellPadLddGetPortNo() value - a
//                       single system-wide libpad namespace, so both agree.
//   data2 = left  / large / low-frequency motor value (0..255)
//   data3 = right / small / high-frequency motor value (0..255)
//
// The one non-rumble event uses:
//   data1 = XPAD_EV_GAME_READY
//   data2 = number of successfully installed game hooks (expected: 4)
//   data3 = PID of the game process that installed them
//
// PS3's CellPadActParam exposes motor[0] = small motor (on/off, 0 or 1) and
// motor[1] = large motor (0..255). The game side maps:
//   left  (data2) <- motor[1]                (large, already 0..255)
//   right (data3) <- motor[0] ? 0xFF : 0x00  (small is on/off on PS3)
// which lines up with the Xbox 360 rumble command byte order used by both
// xpad_set_rumble() (wired) and xpadw_set_rumble() (wireless).
#define RUMBLE_EV_PORT(ev)  ((int32_t)((ev) & 0xFF))
#define RUMBLE_EV_LVAL(ev)  ((uint8_t)((ev) & 0xFF))
#define RUMBLE_EV_RVAL(ev)  ((uint8_t)((ev) & 0xFF))

#endif // __XPAD_RUMBLE_H__
