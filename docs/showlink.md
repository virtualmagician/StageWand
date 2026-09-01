# showlink — the StageWand ↔ StageWizard link

As-built protocol note for `Simulator/Sources/SimCore/showui/showlink.h` (the
portable C interface StageWand's UI code calls; it compiles unchanged into
both the macOS simulator and the ESP32-C6 firmware). Rides StageWizard's
existing `dev`-branch (v1.5.x) remote hooks with no host changes required.

## What showlink speaks today

**Commands OUT** — OSC 1.0 over UDP, address-only messages (arguments, if
any, are ignored by the host), default port **53100**:

```
/stagewizard/go
/stagewizard/stopall
/stagewizard/next
/stagewizard/prev
/stagewizard/toggle
/stagewizard/panic
/stagewizard/cue/<number>/fire   (number: no slashes)
```

**State IN** — two complementary paths:

1. **HTTP `GET /status`**, polled at 2 Hz (500 ms), default port **53200**.
   Always available against a real `dev`-branch host today; this is the
   baseline path and needs no host changes.
2. **Passive OSC status feedback ingest** — the PROPOSED extension written
   up in [`docs/stagewizard-osc-requests.html`](stagewizard-osc-requests.html)
   (P1: not yet on the `dev` branch). showlink listens on its own send
   socket for `/stagewizard/status/standingby`, `/status/running`,
   `/status/panic`, `/status/showmode` and applies them the instant they
   arrive — no request round-trip.

   showlink **automatically prefers OSC over HTTP the moment feedback
   arrives**: as soon as any `/stagewizard/status/*` message is ingested,
   it becomes the freshest source of truth for `online` and the visible
   state, ahead of whatever the next scheduled HTTP poll would report. If
   OSC feedback then goes quiet, the HTTP poll keeps `online` alive on its
   own — the two paths are a fallback pair, not an either/or, so the host
   team can ship the OSC extension whenever it's ready without breaking
   anything in the meantime.

   Keepalive: whenever the link is enabled, showlink sends
   `/stagewand/ping` to the host once per second. On a host implementing
   the extension this is what registers/renews the subscription; on
   today's `dev` branch it's simply an address the OSC parser doesn't
   recognize and drops silently, so it's safe to send unconditionally.

## `showlink_state_t` fields

| Field | Type | Meaning |
|---|---|---|
| `enabled` | `bool` | Link switched on by the operator |
| `online` | `bool` | A good status (HTTP or OSC feedback) arrived within the last 2.5 s |
| `standing_by_number` | `char[16]` | Cue number standing by; `""` when none/unknown |
| `standing_by_name` | `char[64]` | Cue name standing by; `""` when none/unknown |
| `running_count` | `int32_t` | Number of cues currently running |
| `show_mode` | `bool` | Host is in show mode |
| `panicking` | `bool` | Host is in a panic state |
| `last_status_age_ms` | `uint32_t` | ms since the last good status; `UINT32_MAX` if never |

## Timing

| Interval | Value |
|---|---|
| `/stagewand/ping` keepalive | 1 s |
| HTTP `/status` poll | 500 ms (2 Hz) |
| `online` timeout | 2.5 s since the last good status |

## Host-side ask

The full, priority-ordered list of feedback StageWizard would need to add
for StageWand to become a first-class remote (OSC status push, cue-context
window, faders/discovery) lives in
[`docs/stagewizard-osc-requests.html`](stagewizard-osc-requests.html). P1 of
that list — OSC status feedback — is exactly what `--osc-feedback` on the
mock below simulates ahead of the host shipping it.

## Testing without a real host: the mock

`tools/mock_stagewizard.py` (Python 3 stdlib only, no `pip install`)
reproduces the StageWizard remote surface described above — OSC commands
in, HTTP `/status` out, plus the proposed OSC feedback extension behind
`--osc-feedback` (on by default) — so showlink's ingest path can be
exercised end-to-end before the real host implements P1. It carries its own
small hardcoded cue list and show state; it is not a StageWizard show file
reader.

```sh
# run it standalone, defaults matching the real host's ports
python3 tools/mock_stagewizard.py

# non-default ports, feedback extension off (HTTP-poll-only behavior)
python3 tools/mock_stagewizard.py --osc-port 55100 --http-port 55200 --no-osc-feedback

# print the default /status JSON and exit, no servers started (for scripting)
python3 tools/mock_stagewizard.py --once-status

# demo/timed command injection
python3 tools/mock_stagewizard.py --script "go@1.0,panic@3.0"
```

`tools/test_link.sh` drives the mock end-to-end against the built
simulator: starts the mock on non-default ports 55100/55200, runs the
headless `AmoledSim` binary with `--link 127.0.0.1 --link-ports
55100,55200` and a synthetic tap on the GO button, then checks the mock's
log for evidence of a subscribe, the `/stagewizard/go` fired by the tap,
and status coming back (HTTP or OSC feedback), printing `PASS`/`FAIL` for
each and exiting non-zero on any failure.

```sh
tools/test_link.sh
```

(This script assumes the simulator's `--link`/`--link-ports`/`--tap` flags
are present; if they land after this doc, build the simulator first.)

## Update — StageWizard dev D21 (same day)

The host shipped P1+P2 feedback and `/cue/{number}/select`; showlink now
ingests the full set: `standingby`, `running`, `panic`, `showmode`,
`window` (i,i,s,s,s,s), `notes` (s), `elapsed` (f elapsed, f duration;
duration < 0 = indefinite). New API: `showlink_send_select_cue()`. The
cues page shows the host GO-sequence window (tap PREV/NEXT to arm via
select); the GO page gains a progress bar; the setup page shows network +
OSC health (transport, status age, host, show mode).

Freshness: OSC feedback is change-only (elapsed streams only while
something runs), so OSC staleness allows 10 s before falling back to HTTP
(2.5 s); a host-side idle heartbeat has been requested to shrink this.
macOS note: sockets set `SO_NOSIGPIPE` — a refused `/status` connect
otherwise kills the process via SIGPIPE (lwIP has no SIGPIPE).

Headless: `--tile N` (0 cues / 1 GO / 2 faders / 3 setup) captures any
page; the mock now also pushes window/notes and a 30 s demo elapsed
stream.

## Update — contract closed out (StageWizard v1.6.0 + dev D22, 2026-09-01)

The handoff doc is now the as-built contract: v1.6.0 shipped P1+P2+select+
Bonjour; dev D22 added the ~2 s `/status/running` liveness heartbeat and the
full `cuelist` begin/item/end burst (first 64 cues, re-burst on edits).
Client alignment: OSC staleness window tightened to 5.5 s (two missed
heartbeats); window `index` handled as 0-based with −1 = nothing standing
by (displayed 1-based); the mock emulates the heartbeat and 0-based index.
Faders were scrapped per the contract — page 3 is the transport page.

## Update — StageWizard family palette + cue color tags

ShowUI now wears StageWizard's Theme (dev): MagicLab steel-blue accent
`#7A9EB6` — which GO wears, matching the host's own `Theme.go = accent` —
standby green `#59D959` for the link dot, panic `#E64D33`, and the host's
six cue-tag swatches as row tints (~22% over black, standing-by deepened).
`cuelist/item` accepts an OPTIONAL 4th arg `s colorTag` (red/crimson/rose/
sky/steel/navy + legacy aliases per CueListView.tagColor); absent = untagged.
Host-side this is a one-line addition to the item encoder; the mock sends it.

## Planned — BLE fallback transport (spec agreed, wand side awaits hardware)

The handoff doc now carries the BLE fallback spec: the wand advertises GATT
service 8B0F4F44-5A5B-4EC1-A0E9-77616E640001 (RX write ...0002, TX notify
...0003) ONLY while Wi-Fi is down (single-radio coexistence is unstable per
Espressif); StageWizard connects as a CoreBluetooth central with standing
auto-reconnect. Same OSC messages byte-for-byte, framed as u16-BE length +
payload (macOS central MTU is ~182 usable; status/window can exceed it).
Connection = subscription; snapshot burst on connect; 1 Hz ping and 2 s
heartbeat unchanged. BLE-MIDI was evaluated and rejected (macOS manual
reconnect, no host MIDI output for feedback, reduced verb set). The wand's
showlink gains this as a second transport during hardware bring-up — NimBLE
RAM (~40-80 KB) must be measured on the real board first.

### BLE as-built correction (host shipped: dev D28, in v1.7.0 era)

BLEWandLink.swift implements the central side. Direction correction vs the
original ask (GATT roles fix this): the central can only WRITE, so the host
writes its FEEDBACK frames to ...0002, and the wand NOTIFIES its COMMANDS
and pings on ...0003. Same framing both ways; host dispatches anything
inbound exactly like a UDP datagram. Wand-side NimBLE bring-up is the last
open item (needs hardware).
