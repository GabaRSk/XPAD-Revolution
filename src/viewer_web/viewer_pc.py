#!/usr/bin/env python3
import json, socket, sys, threading, time, webbrowser, mimetypes
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

UDP_PORT = 39000
DISCOVERY_PORT = 39001
HTTP_PORT = 8765
MAGIC = b"XPV3"
DISCOVERY_PACKET = b"XPVD\x01\x00\x00\x00"
PROTOCOL_VERSION = 1
PACKET_SIZE = 18
MAX_PADS = 7
TIMEOUT_SECONDS = 1.0

BUTTONS = {
    "a": 0x0001,
    "b": 0x0002,
    "x": 0x0004,
    "y": 0x0008,
    "lb": 0x0010,
    "rb": 0x0020,
    "back": 0x0040,
    "start": 0x0080,
    "l3": 0x0100,
    "r3": 0x0200,
    "up": 0x0400,
    "down": 0x0800,
    "left": 0x1000,
    "right": 0x2000,
    "guide": 0x4000,
}

def neutral_state(pad=0):
    return {"pad":pad,"connected":False,"buttons":0,"lt":0,"rt":0,"lx":128,"ly":128,"rx":128,"ry":128,"sequence":0,"ps3_ip":None,"last_seen":0.0}

class StateStore:
    def __init__(self):
        self.lock = threading.Lock()
        self.states = [neutral_state(i) for i in range(MAX_PADS)]
        self.versions = [0 for _ in range(MAX_PADS)]
    def update_packet(self, data, addr):
        if len(data) != PACKET_SIZE or data[:4] != MAGIC or data[4] != PROTOCOL_VERSION:
            return False
        pad = data[5]
        if pad >= MAX_PADS: return False
        now = time.monotonic()
        state = {
            "pad": pad, "connected": bool(data[6] & 1), "buttons": data[8] | (data[9] << 8),
            "lt": data[10], "rt": data[11], "lx": data[12], "ly": data[13], "rx": data[14], "ry": data[15],
            "sequence": data[16] | (data[17] << 8), "ps3_ip": addr[0], "last_seen": now,
        }
        with self.lock:
            old = self.states[pad]
            vis_old = {k: old[k] for k in ("connected","buttons","lt","rt","lx","ly","rx","ry","ps3_ip")}
            vis_new = {k: state[k] for k in ("connected","buttons","lt","rt","lx","ly","rx","ry","ps3_ip")}
            self.states[pad] = state
            if vis_old != vis_new: self.versions[pad] += 1
        return True
    def expire(self):
        now = time.monotonic()
        with self.lock:
            for pad, st in enumerate(self.states):
                if st["connected"] and st["last_seen"] and now - st["last_seen"] > TIMEOUT_SECONDS:
                    new = neutral_state(pad)
                    new["ps3_ip"] = st["ps3_ip"]
                    self.states[pad] = new
                    self.versions[pad] += 1
    def get(self, pad):
        with self.lock:
            pad = max(0, min(MAX_PADS-1, int(pad)))
            return dict(self.states[pad]), self.versions[pad]

STORE = StateStore(); STOP = threading.Event()
ROOT = Path(__file__).resolve().parent


def udp_receiver():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.bind(("0.0.0.0", UDP_PORT)); sock.settimeout(0.25)
    print(f"[UDP] Descoberta automatica ativa; ouvindo o PS3 na porta {UDP_PORT}...")
    announced = set()
    next_discovery = 0.0
    while not STOP.is_set():
        now = time.monotonic()
        if now >= next_discovery:
            try:
                sock.sendto(DISCOVERY_PACKET, ("255.255.255.255", DISCOVERY_PORT))
            except OSError:
                pass
            next_discovery = now + 1.0
        try:
            data, addr = sock.recvfrom(256)
        except socket.timeout:
            STORE.expire(); continue
        except OSError:
            break
        if STORE.update_packet(data, addr) and addr[0] not in announced:
            announced.add(addr[0]); print(f"[OK] PS3 detectado: {addr[0]}")
    sock.close()

def demo_generator():
    phases = [
        (BUTTONS["a"],128,128,128,128,0,0),(BUTTONS["b"],128,128,128,128,0,0),(BUTTONS["x"],128,128,128,128,0,0),(BUTTONS["y"],128,128,128,128,0,0),
        (BUTTONS["up"],128,128,128,128,0,0),(BUTTONS["right"],128,128,128,128,0,0),(BUTTONS["down"],128,128,128,128,0,0),(BUTTONS["left"],128,128,128,128,0,0),
        (BUTTONS["lb"]|BUTTONS["rb"],128,128,128,128,0,0),(BUTTONS["start"]|BUTTONS["back"],128,128,128,128,0,0),
        (BUTTONS["guide"],128,128,128,128,0,0),(0,35,128,220,128,0,0),(0,220,55,35,205,0,0),(BUTTONS["l3"]|BUTTONS["r3"],128,128,128,128,185,235),(0,128,128,128,128,0,0)
    ]
    seq=0; idx=0
    while not STOP.is_set():
        buttons,lx,ly,rx,ry,lt,rt = phases[idx % len(phases)]
        packet = bytearray(PACKET_SIZE)
        packet[:4] = MAGIC; packet[4] = PROTOCOL_VERSION; packet[5] = 0; packet[6] = 1
        packet[8] = buttons & 0xFF; packet[9] = (buttons >> 8) & 0xFF
        packet[10] = lt; packet[11] = rt; packet[12:16] = bytes((lx,ly,rx,ry))
        packet[16] = seq & 0xFF; packet[17] = (seq >> 8) & 0xFF
        STORE.update_packet(bytes(packet), ("DEMO", UDP_PORT))
        seq=(seq+1)&0xFFFF; idx += 1; time.sleep(0.45)

class Handler(BaseHTTPRequestHandler):
    server_version = "PS3xPADViewer/3.8"
    def log_message(self, fmt, *args): return
    def _headers(self, status=200, content_type="text/plain; charset=utf-8", length=None):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
        self.send_header("Access-Control-Allow-Origin", "*")
        if length is not None: self.send_header("Content-Length", str(length))
        self.end_headers()
    def _serve_file(self, path):
        if not path.exists() or not path.is_file():
            self._headers(404, length=3); self.wfile.write(b'404'); return
        ctype = mimetypes.guess_type(str(path))[0] or 'application/octet-stream'
        data = path.read_bytes(); self._headers(200, ctype, len(data)); self.wfile.write(data)
    def do_GET(self):
        parsed = urlparse(self.path); qs = parse_qs(parsed.query)
        try: pad = max(0, min(MAX_PADS-1, int(qs.get('pad', ['0'])[0])))
        except ValueError: pad = 0
        if parsed.path in ('/','/overlay.html'): return self._serve_file(ROOT/'overlay.html')
        if parsed.path == '/TESTAR_OVERLAY.html': return self._serve_file(ROOT/'TESTAR_OVERLAY.html')
        if parsed.path.startswith('/assets/'):
            rel = parsed.path[len('/assets/'):]
            return self._serve_file(ROOT/'assets'/rel)
        if parsed.path == '/state':
            state, version = STORE.get(pad); state['version']=version
            state['age_ms'] = int(max(0.0, time.monotonic()-state['last_seen'])*1000) if state['last_seen'] else None
            state.pop('last_seen', None)
            data=json.dumps(state,separators=(',',':')).encode('utf-8')
            self._headers(200,'application/json; charset=utf-8',len(data)); self.wfile.write(data); return
        if parsed.path == '/events':
            self.send_response(200); self.send_header('Content-Type','text/event-stream'); self.send_header('Cache-Control','no-cache'); self.send_header('Connection','keep-alive'); self.send_header('Access-Control-Allow-Origin','*'); self.end_headers()
            last_version=-1; last_ping=0.0
            try:
                while not STOP.is_set():
                    state, version = STORE.get(pad); now = time.monotonic()
                    if version != last_version:
                        payload = dict(state); payload.pop('last_seen',None)
                        raw = json.dumps(payload,separators=(',',':'))
                        self.wfile.write(("data: "+raw+"\n\n").encode('utf-8')); self.wfile.flush(); last_version=version; last_ping=now
                    elif now-last_ping >= 1.0:
                        self.wfile.write(b': ping\n\n'); self.wfile.flush(); last_ping=now
                    time.sleep(0.008)
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
            return
        if parsed.path == '/health':
            data=b'OK'; self._headers(200,'text/plain; charset=utf-8',len(data)); self.wfile.write(data); return
        self._headers(404, length=3); self.wfile.write(b'404')

class ViewerHTTPServer(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        # Browsers routinely cancel speculative/keep-alive localhost requests.
        # Python 3.14 reports that harmless close as a full WinError traceback.
        error = sys.exc_info()[1]
        harmless_windows_errors = (10053, 10054)
        if isinstance(error, (ConnectionAbortedError, ConnectionResetError, BrokenPipeError)):
            return
        if isinstance(error, OSError) and getattr(error, 'winerror', None) in harmless_windows_errors:
            return
        super().handle_error(request, client_address)

def main():
    import argparse
    ap = argparse.ArgumentParser(description='XPAD Revolution Viewer PC')
    ap.add_argument('--demo', action='store_true', help='gera input de demonstracao sem PS3')
    ap.add_argument('--no-browser', action='store_true', help='nao abre o navegador automaticamente')
    args = ap.parse_args()
    t = threading.Thread(target=demo_generator if args.demo else udp_receiver, daemon=True); t.start()
    httpd = ViewerHTTPServer(('127.0.0.1', HTTP_PORT), Handler)
    print(f'[HTTP] Viewer em http://127.0.0.1:{HTTP_PORT}/')
    if not args.no_browser:
        webbrowser.open(f'http://127.0.0.1:{HTTP_PORT}/?pad=0{"&debug=1" if args.demo else ""}')
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        STOP.set(); httpd.server_close()

if __name__ == '__main__':
    main()
