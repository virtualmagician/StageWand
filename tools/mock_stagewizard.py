#!/usr/bin/env python3
"""mock_stagewizard.py -- a stand-in for the real StageWizard remote surface.

Reproduces, on non-default ports if you like, exactly the surface StageWand's
showlink.[ch] talks to (dev branch, v1.5.x):

  OSC / UDP  (--osc-port, default 53100)   commands IN, address-only:
    /stagewizard/go
    /stagewizard/stopall
    /stagewizard/next
    /stagewizard/prev
    /stagewizard/toggle
    /stagewizard/panic
    /stagewizard/cue/<number>/fire         (number: no slashes)
    Type tags i/f/s may be present; all arguments are ignored.
    A leading "#bundle\\0" datagram is recognized and silently ignored (the
    real client only ever sends bare messages).

  HTTP  (--http-port, default 53200)
    GET  /status  -> {"standingByNumber", "standingByName", "notes",
                       "runningCount", "showMode", "panicking"}
                      application/json, Connection: close (one response per
                      connection, matching the real host).
    POST /go /stopall /panic /next /prev    same effect as the OSC command.

  --osc-feedback (default ON; disable with --no-osc-feedback) implements the
  PROPOSED feedback extension from docs/stagewizard-osc-requests.html (P1):
  any UDP datagram's source address becomes a "subscriber" (dropped after 5s
  of silence). /stagewand/ping is the client's ~1s keepalive. On first
  packet from an address, and on every state change, push these OSC
  messages to every live subscriber:
    /stagewizard/status/standingby   s number, s name
    /stagewizard/status/running      i count
    /stagewizard/status/panic        i 0|1
    /stagewizard/status/showmode     i 0|1

Python 3 stdlib only -- no pip install required.

Usage:
    tools/mock_stagewizard.py [--osc-port 53100] [--http-port 53200]
                               [--osc-feedback | --no-osc-feedback]
                               [--bind 127.0.0.1]
                               [--once-status]
                               [--script "go@1.0,panic@3.0"]

Log line formats (stdout, one line per event, flushed immediately -- see the
module-level LOG_FORMATS comment below for the full list; scripts such as
tools/test_link.sh grep these):
    MOCK listening: osc=<bind>:<osc_port> http=<bind>:<http_port> feedback=<on|off>
    OSC RECV <address> from <ip>:<port>
    OSC UNKNOWN <address> from <ip>:<port>
    OSC MALFORMED from <ip>:<port>
    OSC BUNDLE ignored from <ip>:<port>
    CMD <address> from <ip>:<port> -> <summary>
    CMD <address> from SCRIPT -> <summary>
    FEEDBACK SUBSCRIBE <ip>:<port> (total subscribers=<n>)
    FEEDBACK EXPIRE <ip>:<port> (total subscribers=<n>)
    FEEDBACK PUSH -> <ip>:<port> standingby=(<num>,<name>) running=<n> panic=<0|1> showmode=<0|1>
    HTTP GET /status from <ip>:<port> -> 200
    HTTP GET <path> from <ip>:<port> -> 404
    HTTP POST <path> from <ip>:<port> -> <summary>
    PANIC auto-clear panicking=false
    SCRIPT INJECT <cmd> scheduled_at=<seconds>s
    MOCK signal <signum> received, shutting down
    MOCK shutting down
"""
import argparse
import json
import re
import signal
import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ---------------------------------------------------------------------------
# Show state
# ---------------------------------------------------------------------------

CUES = [
    ("1", "Preset - house open"),
    ("2", "Blackout"),
    ("3", "Reveal - center spot"),
    ("4", "Video wall up"),
    ("5", "Drone lift"),
]
DEFAULT_INDEX = 2  # cue "3" -- matches the /status example in the spec
RUNNING_CAP = 8

# Demo notes for the P2 /stagewizard/status/notes feedback.
NOTES = {
    "3": "Followspot pickup stage left. Wait for applause to settle.",
    "4": "Video wall: confirm HDMI 2 live before GO.",
}
DEMO_CUE_DURATION = 30.0  # seconds; drives the P2 elapsed stream
PANIC_CLEAR_SECS = 3.0
SUBSCRIBER_TIMEOUT_SECS = 5.0
PERIODIC_TICK_SECS = 0.2

CUE_FIRE_RE = re.compile(r"^/stagewizard/cue/([^/]+)/fire$")


def log(msg):
    print(msg, flush=True)


class ShowState:
    """The hardcoded cue list + a playhead, mutated exactly like a show would be."""

    def __init__(self):
        self.lock = threading.RLock()
        self.index = DEFAULT_INDEX
        self.adhoc = None  # (number, name) override, set when an unlisted cue is fired
        self.running_count = 1
        self.show_mode = False
        self.panicking = False
        self.panic_clear_at = None
        self.elapsed = 0.0  # of the most recently fired cue, for the P2 stream

    def _standing(self):
        return self.adhoc if self.adhoc is not None else CUES[self.index]

    def _advance(self):
        self.index = (self.index + 1) % len(CUES)

    def status_dict(self):
        with self.lock:
            num, name = self._standing()
            # Contract (D22): index is 0-based in the GO sequence, -1 when the
            # standing-by cue is not part of it (or nothing stands by).
            if self.adhoc is not None:
                window = (-1, len(CUES), "", "", "", "")
            else:
                prev_num, prev_name = CUES[self.index - 1] if self.index > 0 else ("", "")
                nxt = self.index + 1
                next_num, next_name = CUES[nxt] if nxt < len(CUES) else ("", "")
                window = (self.index, len(CUES), prev_num, prev_name, next_num, next_name)
            return {
                "standingByNumber": num,
                "standingByName": name,
                "notes": NOTES.get(num, ""),
                "runningCount": self.running_count,
                "showMode": self.show_mode,
                "panicking": self.panicking,
                "_window": window,
            }

    def cmd_go(self):
        with self.lock:
            num, name = self._standing()
            self.running_count = min(RUNNING_CAP, self.running_count + 1)
            self.adhoc = None
            self.elapsed = 0.0
            self._advance()
            nnum, nname = self._standing()
            return (
                f'go: fired #{num} "{name}", runningCount={self.running_count}, '
                f'next standby #{nnum} "{nname}"'
            )

    def cmd_stopall(self):
        with self.lock:
            self.running_count = 0
            return "stopall: runningCount=0"

    def cmd_next(self):
        with self.lock:
            self.adhoc = None
            self._advance()
            num, name = self._standing()
            return f'next: standby #{num} "{name}"'

    def cmd_prev(self):
        with self.lock:
            self.adhoc = None
            self.index = (self.index - 1) % len(CUES)
            num, name = self._standing()
            return f'prev: standby #{num} "{name}"'

    def cmd_toggle(self):
        return "toggle: no-op (state unchanged)"

    def cmd_panic(self):
        with self.lock:
            self.panicking = True
            self.running_count = 0
            self.panic_clear_at = time.monotonic() + PANIC_CLEAR_SECS
            return (
                f"panic: panicking=true runningCount=0 "
                f"(auto-clears in {PANIC_CLEAR_SECS}s)"
            )

    def cmd_fire(self, number):
        with self.lock:
            idx = next((i for i, (n, _) in enumerate(CUES) if n == number), None)
            if idx is not None:
                self.adhoc = None
                self.index = idx
                note = ""
            else:
                # Unknown cue number: stand it by verbatim (name unknown), per spec.
                self.adhoc = (number, "")
                note = " (unlisted cue number)"
            prefix = f"cue/{number}/fire: standby set to #{number}{note}"
            go_summary = self.cmd_go()
            return f"{prefix}; {go_summary}"

    def maybe_clear_panic(self):
        with self.lock:
            if self.panicking and self.panic_clear_at is not None:
                if time.monotonic() >= self.panic_clear_at:
                    self.panicking = False
                    self.panic_clear_at = None
                    return True
            return False


def dispatch_address(address, state):
    """Run the command named by an OSC address (sans /stagewand/ping). Returns
    a human-readable summary string, or None if the address is unrecognized."""
    if address == "/stagewizard/go":
        return state.cmd_go()
    if address == "/stagewizard/stopall":
        return state.cmd_stopall()
    if address == "/stagewizard/next":
        return state.cmd_next()
    if address == "/stagewizard/prev":
        return state.cmd_prev()
    if address == "/stagewizard/toggle":
        return state.cmd_toggle()
    if address == "/stagewizard/panic":
        return state.cmd_panic()
    m = CUE_FIRE_RE.match(address)
    if m:
        return state.cmd_fire(m.group(1))
    return None


# ---------------------------------------------------------------------------
# OSC encoding (proposed feedback extension)
# ---------------------------------------------------------------------------


def osc_string(s):
    b = s.encode("utf-8") + b"\x00"
    while len(b) % 4 != 0:
        b += b"\x00"
    return b


def osc_int(i):
    return struct.pack(">i", int(i))


def encode_osc_message(address, *args):
    """args: sequence of (type_char, value); type_char in {'s', 'i', 'f'}."""
    type_tag = "," + "".join(t for t, _ in args)
    out = osc_string(address) + osc_string(type_tag)
    for t, v in args:
        if t == "s":
            out += osc_string(v)
        elif t == "i":
            out += osc_int(v)
        elif t == "f":
            out += struct.pack(">f", v)
        else:
            raise ValueError(f"unsupported OSC type {t!r}")
    return out


def build_feedback_messages(status):
    """P1 status set plus the P2 window/notes messages (StageWizard dev D21)."""
    window = status.get("_window", (0, 0, "", "", "", ""))
    idx, total, prev_num, prev_name, next_num, next_name = window
    return [
        encode_osc_message(
            "/stagewizard/status/standingby",
            ("s", status["standingByNumber"]),
            ("s", status["standingByName"]),
        ),
        encode_osc_message("/stagewizard/status/running", ("i", status["runningCount"])),
        encode_osc_message("/stagewizard/status/panic", ("i", 1 if status["panicking"] else 0)),
        encode_osc_message("/stagewizard/status/showmode", ("i", 1 if status["showMode"] else 0)),
        encode_osc_message(
            "/stagewizard/status/window",
            ("i", idx), ("i", total),
            ("s", prev_num), ("s", prev_name),
            ("s", next_num), ("s", next_name),
        ),
        encode_osc_message("/stagewizard/status/notes", ("s", status["notes"])),
    ]


def build_elapsed_message(elapsed, duration):
    return encode_osc_message(
        "/stagewizard/status/elapsed", ("f", elapsed), ("f", duration)
    )


# ---------------------------------------------------------------------------
# Feedback subscribers
# ---------------------------------------------------------------------------


class Subscribers:
    def __init__(self):
        self.lock = threading.Lock()
        self.last_seen = {}  # (ip, port) -> monotonic timestamp

    def touch(self, addr):
        now = time.monotonic()
        with self.lock:
            is_new = addr not in self.last_seen
            self.last_seen[addr] = now
        return is_new

    def count(self):
        with self.lock:
            return len(self.last_seen)

    def live_and_prune(self):
        now = time.monotonic()
        with self.lock:
            alive, expired = [], []
            for addr, seen in list(self.last_seen.items()):
                if now - seen <= SUBSCRIBER_TIMEOUT_SECS:
                    alive.append(addr)
                else:
                    expired.append(addr)
                    del self.last_seen[addr]
        return alive, expired


def build_cuelist_messages():
    """PROPOSED full cue-list feedback: begin(i count) / item(i, s, s) / end(i count)."""
    msgs = [encode_osc_message("/stagewizard/cuelist/begin", ("i", len(CUES)))]
    for i, (num, name) in enumerate(CUES):
        msgs.append(encode_osc_message(
            "/stagewizard/cuelist/item", ("i", i), ("s", num), ("s", name)))
    msgs.append(encode_osc_message("/stagewizard/cuelist/end", ("i", len(CUES))))
    return msgs


def push_feedback_to(sock, addr, state):
    status = state.status_dict()
    for msg in build_feedback_messages(status):
        sock.sendto(msg, addr)
    for msg in build_cuelist_messages():
        sock.sendto(msg, addr)
    log(
        f"FEEDBACK PUSH -> {addr[0]}:{addr[1]} "
        f'standingby=({status["standingByNumber"]},{status["standingByName"]}) '
        f'running={status["runningCount"]} panic={int(status["panicking"])} '
        f'showmode={int(status["showMode"])}'
    )


def push_feedback_to_all(sock, subscribers, state):
    alive, _ = subscribers.live_and_prune()
    for addr in alive:
        push_feedback_to(sock, addr, state)


# ---------------------------------------------------------------------------
# UDP (OSC) loop
# ---------------------------------------------------------------------------


def udp_loop(sock, state, subscribers, feedback_enabled, stop_event):
    sock.settimeout(0.5)
    while not stop_event.is_set():
        try:
            data, addr = sock.recvfrom(65535)
        except socket.timeout:
            continue
        except OSError:
            break

        ip, port = addr

        if feedback_enabled:
            is_new = subscribers.touch(addr)
            if is_new:
                log(f"FEEDBACK SUBSCRIBE {ip}:{port} (total subscribers={subscribers.count()})")
                push_feedback_to(sock, addr, state)

        if data.startswith(b"#bundle\x00"):
            log(f"OSC BUNDLE ignored from {ip}:{port}")
            continue

        try:
            address = data.split(b"\x00", 1)[0].decode("utf-8", errors="replace")
        except Exception:
            log(f"OSC MALFORMED from {ip}:{port}")
            continue

        if not address.startswith("/"):
            log(f"OSC MALFORMED from {ip}:{port}")
            continue

        log(f"OSC RECV {address} from {ip}:{port}")

        if address == "/stagewand/ping":
            continue  # keepalive only; subscribe bookkeeping already happened above

        summary = dispatch_address(address, state)
        if summary is not None:
            log(f"CMD {address} from {ip}:{port} -> {summary}")
            if feedback_enabled:
                push_feedback_to_all(sock, subscribers, state)
        else:
            log(f"OSC UNKNOWN {address} from {ip}:{port}")


# ---------------------------------------------------------------------------
# Periodic housekeeping: panic auto-clear, subscriber expiry
# ---------------------------------------------------------------------------


def periodic_loop(state, subscribers, sock, feedback_enabled, stop_event):
    while not stop_event.is_set():
        time.sleep(PERIODIC_TICK_SECS)

        if state.maybe_clear_panic():
            log("PANIC auto-clear panicking=false")
            if feedback_enabled:
                push_feedback_to_all(sock, subscribers, state)

        if feedback_enabled:
            alive, expired = subscribers.live_and_prune()
            for addr in expired:
                log(f"FEEDBACK EXPIRE {addr[0]}:{addr[1]} (total subscribers={len(alive)})")

            # D22 liveness heartbeat: re-send running every ~2 s unconditionally.
            now_mono = time.monotonic()
            if not hasattr(state, "_last_heartbeat"):
                state._last_heartbeat = 0.0
            if alive and now_mono - state._last_heartbeat >= 2.0:
                state._last_heartbeat = now_mono
                hb = encode_osc_message(
                    "/stagewizard/status/running", ("i", state.running_count))
                for addr in alive:
                    sock.sendto(hb, addr)

            # P2 elapsed stream: like the real host, only while anything runs.
            with state.lock:
                running = state.running_count > 0
                if running:
                    state.elapsed = min(state.elapsed + PERIODIC_TICK_SECS, DEMO_CUE_DURATION)
                    elapsed = state.elapsed
            if running and alive:
                msg = build_elapsed_message(elapsed, DEMO_CUE_DURATION)
                for addr in alive:
                    sock.sendto(msg, addr)


# ---------------------------------------------------------------------------
# HTTP
# ---------------------------------------------------------------------------


class StatusHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"  # no keep-alive: one response per connection

    def log_message(self, fmt, *args):
        pass  # we do our own logging

    def _send_json(self, obj, code=200):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def _drain_body(self):
        length = int(self.headers.get("Content-Length", 0) or 0)
        if length:
            self.rfile.read(length)

    def do_GET(self):
        ip, port = self.client_address
        if self.path == "/status":
            status = self.server.show_state.status_dict()
            self._send_json(status)
            log(f"HTTP GET /status from {ip}:{port} -> 200")
        else:
            self.send_response(404)
            self.send_header("Connection", "close")
            self.end_headers()
            log(f"HTTP GET {self.path} from {ip}:{port} -> 404")

    def do_POST(self):
        self._drain_body()
        ip, port = self.client_address
        state = self.server.show_state
        mapping = {
            "/go": state.cmd_go,
            "/stopall": state.cmd_stopall,
            "/panic": state.cmd_panic,
            "/next": state.cmd_next,
            "/prev": state.cmd_prev,
        }
        fn = mapping.get(self.path)
        if fn is None:
            self.send_response(404)
            self.send_header("Connection", "close")
            self.end_headers()
            log(f"HTTP POST {self.path} from {ip}:{port} -> 404")
            return
        summary = fn()
        log(f"HTTP POST {self.path} from {ip}:{port} -> {summary}")
        self._send_json(state.status_dict())
        if self.server.feedback_enabled:
            push_feedback_to_all(self.server.udp_sock, self.server.subscribers, state)


# ---------------------------------------------------------------------------
# --script timed command injection
# ---------------------------------------------------------------------------


def schedule_script(spec, state, subscribers, udp_sock, feedback_enabled, stop_event):
    entries = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "@" not in part:
            log(f"SCRIPT bad entry '{part}' (expected cmd@seconds), skipping")
            continue
        cmd, t = part.rsplit("@", 1)
        cmd = cmd.strip()
        try:
            delay = float(t)
        except ValueError:
            log(f"SCRIPT bad delay in '{part}', skipping")
            continue
        entries.append((cmd, delay))
        log(f"SCRIPT INJECT {cmd} scheduled_at={delay}s")

    entries.sort(key=lambda e: e[1])

    def runner():
        start = time.monotonic()
        for cmd, delay in entries:
            remaining = delay - (time.monotonic() - start)
            if remaining > 0:
                if stop_event.wait(remaining):
                    return
            if stop_event.is_set():
                return
            address = cmd if cmd.startswith("/") else "/stagewizard/" + cmd
            summary = dispatch_address(address, state)
            if summary is not None:
                log(f"CMD {address} from SCRIPT -> {summary}")
                if feedback_enabled:
                    push_feedback_to_all(udp_sock, subscribers, state)
            else:
                log(f"SCRIPT unknown command '{cmd}'")

    threading.Thread(target=runner, daemon=True, name="script-runner").start()


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------


def build_arg_parser():
    p = argparse.ArgumentParser(
        description="Mock StageWizard OSC + HTTP server for StageWand end-to-end testing."
    )
    p.add_argument("--osc-port", type=int, default=53100, help="UDP OSC listen port (default 53100)")
    p.add_argument("--http-port", type=int, default=53200, help="HTTP listen port (default 53200)")
    p.add_argument("--bind", default="127.0.0.1", help="bind address (default 127.0.0.1)")
    p.add_argument(
        "--osc-feedback",
        dest="osc_feedback",
        action="store_true",
        default=True,
        help="enable the proposed OSC status-feedback extension (default: on)",
    )
    p.add_argument(
        "--no-osc-feedback",
        dest="osc_feedback",
        action="store_false",
        help="disable the OSC feedback extension (HTTP /status polling only)",
    )
    p.add_argument(
        "--once-status",
        action="store_true",
        help="print the current /status JSON (default show state) and exit, no servers started",
    )
    p.add_argument(
        "--script",
        default=None,
        metavar="SPEC",
        help='timed command injection, e.g. "go@1.0,panic@3.0" (command names without /stagewizard/ prefix, or cue/<n>/fire)',
    )
    return p


def main(argv=None):
    args = build_arg_parser().parse_args(argv)

    state = ShowState()

    if args.once_status:
        print(json.dumps(state.status_dict()))
        return 0

    subscribers = Subscribers()

    udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        udp_sock.bind((args.bind, args.osc_port))
    except OSError as e:
        log(f"MOCK failed to bind UDP {args.bind}:{args.osc_port}: {e}")
        return 1

    try:
        httpd = ThreadingHTTPServer((args.bind, args.http_port), StatusHandler)
    except OSError as e:
        log(f"MOCK failed to bind HTTP {args.bind}:{args.http_port}: {e}")
        udp_sock.close()
        return 1

    httpd.show_state = state
    httpd.feedback_enabled = args.osc_feedback
    httpd.udp_sock = udp_sock
    httpd.subscribers = subscribers

    stop_event = threading.Event()

    def handle_signal(signum, _frame):
        log(f"MOCK signal {signum} received, shutting down")
        stop_event.set()

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    threads = [
        threading.Thread(
            target=udp_loop,
            args=(udp_sock, state, subscribers, args.osc_feedback, stop_event),
            daemon=True,
            name="udp-loop",
        ),
        threading.Thread(
            target=periodic_loop,
            args=(state, subscribers, udp_sock, args.osc_feedback, stop_event),
            daemon=True,
            name="periodic",
        ),
        threading.Thread(target=httpd.serve_forever, daemon=True, name="http"),
    ]
    for t in threads:
        t.start()

    log(
        f"MOCK listening: osc={args.bind}:{args.osc_port} http={args.bind}:{args.http_port} "
        f"feedback={'on' if args.osc_feedback else 'off'}"
    )

    if args.script:
        schedule_script(args.script, state, subscribers, udp_sock, args.osc_feedback, stop_event)

    try:
        while not stop_event.is_set():
            time.sleep(PERIODIC_TICK_SECS)
    except KeyboardInterrupt:
        stop_event.set()

    log("MOCK shutting down")
    try:
        httpd.shutdown()
    except Exception:
        pass
    udp_sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
