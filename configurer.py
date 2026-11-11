#!/usr/bin/env python3
#
# Copyright (c) 2026 Andrii Bilenko <bilenko.a.uni@gmail.com>.
# All rights reserved.
#

"""
card emulator PC tool - Kind-A FIME CardImage → ESP32 T6P5 profile, then write (simulate).

  listen   Own serial; print MCU logs; tee to logs/; serve via Unix socket.
  parse    Dry-run CardImage → T6P5 (no MCU); prints reject reason on fail.
  read     Parse local FIME XML, upload binary profile to MCU (no plastic).
  write    Sticky MODE WRITE - MCU emulates loaded FIME profile as PICC.
  catalog  Build card_images/kind_a_application/ from strict-parse successes.

Examples:
  tools/.venv/bin/python configurer.py listen --port /dev/ttyACM0
  tools/.venv/bin/python configurer.py read --port /dev/ttyACM0 \\
    --xml card_images/kind_a_application/.../OfflineCAM_04.xml
  tools/.venv/bin/python configurer.py write --port /dev/ttyACM0
"""

# BY ME TODO: read, understand/clean up

from __future__ import annotations

import argparse
import os
import select
import socket
import sys
import time
from datetime import datetime
from pathlib import Path

try:
    import serial
except ImportError:
    sys.exit("Need pyserial: pip install pyserial")

from fime_parse import MAX_REC, load_profile_bytes, summarize

HERE = Path(__file__).resolve().parent
DEFAULT_XML = HERE / "card.xml"
DEFAULT_LOG_DIR = HERE / "logs"
CHUNK = 48  # hex payload chars per LOAD line (~24 bytes)

# MCU log substrings we wait for after each host command (case-insensitive).
# MODE READ/WRITE: "STATE mode=…" from ceModeSet. LOAD: ce_mode.cpp tokens.
EXPECT_MODE_WRITE = "mode=write"
EXPECT_MODE_READ = "mode=read"
EXPECT_LOAD = "LOAD ack"  # CE_MSG_LOAD_ACK
EXPECT_PROFILE = "profile_ok"  # CE_MSG_PROFILE_OK


class Tee:
    """Print to stdout and append to a log file."""

    def __init__(self, path: Path | None):
        self._path = path
        self._fp = None
        if path is not None:
            path.parent.mkdir(parents=True, exist_ok=True)
            self._fp = path.open("a", encoding="utf-8")

    def write(self, s: str) -> None:
        print(s, flush=True)
        if self._fp is not None:
            self._fp.write(s + "\n")
            self._fp.flush()

    def close(self) -> None:
        if self._fp is not None:
            self._fp.close()
            self._fp = None


def cmd_parse(xml_path: Path) -> int:
    print(f"PARSE begin path={xml_path}", flush=True)
    if not xml_path.is_file():
        print(f"PARSE FAIL missing xml: {xml_path}", flush=True)
        print("STATUS FAIL", flush=True)
        return 1
    try:
        blob, prof = load_profile_bytes(xml_path)
    except ValueError as e:
        print(f"PARSE FAIL reason={e}", flush=True)
        print(f"REJECT: {e}", flush=True)
        print("STATUS FAIL", flush=True)
        return 1
    print("PARSE ok", flush=True)
    print("--- FIME parse ---", flush=True)
    print(summarize(prof), flush=True)
    print(f"profile_bytes={len(blob)} from {xml_path}", flush=True)
    print("STATUS OK", flush=True)
    return 0


def sock_path(port: str) -> str:
    base = os.path.basename(port).replace("/", "_")
    return f"/tmp/test6_{base}.sock"


def open_serial(port: str, baud: int) -> serial.Serial:
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.25
    ser.dsrdtr = False
    ser.rtscts = False
    ser.open()
    time.sleep(0.4)
    try:
        ser.reset_input_buffer()
    except (serial.SerialException, OSError):
        try:
            ser.close()
        except Exception:
            pass
        raise serial.SerialException("port opened but not ready") from None
    return ser


def wait_ready(ser: serial.Serial, timeout_s: float) -> bool:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        line = ser.readline()
        if not line:
            continue
        text = line.decode("utf-8", "replace").rstrip()
        print(text, flush=True)
        low = text.lower()
        if "err no_pn532" in low:
            return False
        if "ready" in low or "mode=idle" in low or "profile=" in low:
            return True
    return True


def ack_for_line(line: str) -> str:
    """MCU log substring that means this host line was accepted."""
    u = line.strip().upper()
    if u == "MODE READ":
        return EXPECT_MODE_READ
    if u == "MODE WRITE":
        return EXPECT_MODE_WRITE
    if u.startswith("LOAD BEGIN"):
        return EXPECT_LOAD
    if u == "LOAD END":
        return EXPECT_PROFILE
    if u.startswith("LOAD "):
        return EXPECT_LOAD
    return ""


def line_failed(text: str) -> bool:
    low = text.lower()
    if "err no_pn532" in low or "target prepare: no_pn532" in low:
        return True
    return "err " in low and (
        "load" in low or "profile" in low or "unknown" in low or "overflow" in low
    )


def send_paced(ser: serial.Serial, lines: list[str], final_expect: str,
               timeout_s: float) -> int:
    """Send one line at a time; wait for MCU ack so CDC cannot overrun."""
    deadline = time.time() + timeout_s
    for line in lines:
        if time.time() > deadline:
            print("STATUS FAIL timeout", flush=True)
            return 1
        expect = ack_for_line(line) or final_expect
        ser.write((line + "\n").encode())
        ser.flush()
        show = line if len(line) < 80 else line[:77] + "..."
        print(f"→ {show}", flush=True)

        got = False
        line_deadline = time.time() + min(8.0, timeout_s)
        while time.time() < line_deadline:
            raw = ser.readline()
            if not raw:
                continue
            text = raw.decode("utf-8", "replace").rstrip()
            if text:
                print(text, flush=True)
            if line_failed(text):
                print("STATUS FAIL", flush=True)
                return 1
            if expect.lower() in text.lower():
                got = True
                break
        if not got:
            print(f"STATUS FAIL timeout waiting for {expect!r}", flush=True)
            return 1

    # final_expect may already have been seen on LOAD END
    print(f"STATUS OK {final_expect}", flush=True)
    return 0


def send_via_socket(port: str, lines: list[str], expect: str, timeout_s: float) -> int:
    path = sock_path(port)
    if not os.path.exists(path):
        return -1
    try:
        cli = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        cli.settimeout(timeout_s)
        cli.connect(path)
        # Framing: one command block; listen paces with MCU acks.
        cli.sendall(("\n".join(lines) + "\n").encode())
        buf = b""
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            try:
                chunk = cli.recv(512)
            except socket.timeout:
                continue
            if not chunk:
                break
            buf += chunk
            text = buf.decode("utf-8", "replace")
            for line in text.splitlines():
                if line.strip():
                    print(line, flush=True)
            if "STATUS OK" in text and expect.lower() in text.lower():
                cli.close()
                return 0
            if "STATUS FAIL" in text:
                cli.close()
                return 1
        cli.close()
        print("STATUS FAIL timeout", flush=True)
        return 1
    except OSError as e:
        print(f"STATUS FAIL socket: {e}", flush=True)
        return 1


def send_direct(port: str, baud: int, lines: list[str], expect: str,
                timeout_s: float) -> int:
    try:
        ser = open_serial(port, baud)
    except serial.SerialException as e:
        print(f"STATUS FAIL open: {e}", flush=True)
        return 1
    try:
        wait_ready(ser, min(5.0, timeout_s))
        return send_paced(ser, lines, expect, timeout_s)
    finally:
        ser.close()


def cmd_mode(port: str, baud: int, mode_line: str, expect: str,
             timeout_s: float) -> int:
    # WRITE prepare can take a few recover probes after a wedged target session.
    timeout_s = max(timeout_s, 30.0)
    rc = send_via_socket(port, [mode_line], expect, timeout_s)
    if rc >= 0:
        return rc
    print("NOTE: no listen socket - talking to MCU directly", flush=True)
    return send_direct(port, baud, [mode_line], expect, timeout_s)


def profile_load_lines(blob: bytes) -> list[str]:
    lines = [f"LOAD BEGIN {len(blob)}"]
    off = 0
    while off < len(blob):
        n = min(CHUNK // 2, len(blob) - off)
        chunk = blob[off : off + n]
        lines.append(f"LOAD {off:04X} {chunk.hex().upper()}")
        off += n
    lines.append("LOAD END")
    return lines


def cmd_read(port: str, baud: int, xml_path: Path, timeout_s: float) -> int:
    print(f"PARSE begin path={xml_path}", flush=True)
    if not xml_path.is_file():
        print(f"PARSE FAIL missing xml: {xml_path}", flush=True)
        print("STATUS FAIL", flush=True)
        return 1
    try:
        blob, prof = load_profile_bytes(xml_path)
    except ValueError as e:
        print(f"PARSE FAIL reason={e}", flush=True)
        print("STATUS FAIL", flush=True)
        return 1
    print("PARSE ok", flush=True)
    print("--- FIME parse ---", flush=True)
    print(summarize(prof), flush=True)
    print(f"profile_bytes={len(blob)} from {xml_path}", flush=True)
    # load_profile_bytes already enforces ce_profile.h caps (incl. MAX_REC=253)
    max_rec = 0
    for a in prof.apps:
        for r in a.records:
            max_rec = max(max_rec, len(r.data))
    for r in prof.shared_records:
        max_rec = max(max_rec, len(r.data))
    if max_rec > MAX_REC:
        print(
            f"PARSE FAIL reason=record len {max_rec} > {MAX_REC} "
            "(CE_MAX_APDU_REC); pick a smaller CardImage",
            flush=True,
        )
        print("STATUS FAIL", flush=True)
        return 1
    lines = ["MODE READ"] + profile_load_lines(blob)
    expect = EXPECT_PROFILE
    # LOAD needs long timeout (ack per chunk)
    timeout_s = max(timeout_s, 60.0)
    rc = send_via_socket(port, lines, expect, timeout_s)
    if rc >= 0:
        return rc
    print("NOTE: no listen socket - talking to MCU directly", flush=True)
    return send_direct(port, baud, lines, expect, timeout_s)


def cmd_listen(port: str, baud: int, log_file: Path | None) -> int:
    if log_file is None:
        stamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
        log_file = DEFAULT_LOG_DIR / f"{stamp}.log"
    tee = Tee(log_file)
    tee.write(f"LOG listen file={log_file}")

    path = sock_path(port)
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass

    try:
        ser = open_serial(port, baud)
    except serial.SerialException as e:
        tee.write(f"STATUS FAIL open: {e}")
        tee.close()
        return 1

    # Drain any pending boot/log lines (DTR reset is off - chip may already be up).
    deadline = time.time() + 2.5
    while time.time() < deadline:
        line = ser.readline()
        if not line:
            continue
        text = line.decode("utf-8", "replace").rstrip()
        if text:
            tee.write(text)

    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(path)
    srv.listen(4)
    srv.setblocking(False)
    os.chmod(path, 0o666)

    tee.write(f"listening on {port}")

    pending: socket.socket | None = None
    pending_expect: str | None = None
    queue: list[str] = []
    waiting_ack: str | None = None

    def fail_pending(msg: bytes = b"STATUS FAIL\n") -> None:
        nonlocal pending, pending_expect, queue, waiting_ack
        if pending is not None:
            try:
                pending.sendall(msg)
            except OSError:
                pass
            try:
                pending.close()
            except OSError:
                pass
        pending = None
        pending_expect = None
        queue = []
        waiting_ack = None

    def reconnect_serial() -> None:
        """Close dead CDC handle and wait until port opens again. One log line."""
        nonlocal ser
        fail_pending(b"STATUS FAIL serial reconnect\n")
        try:
            ser.close()
        except Exception:
            pass
        tee.write("LOG serial dropped; waiting to reopen…")
        while True:
            time.sleep(1.0)
            try:
                ser = open_serial(port, baud)
                tee.write("LOG serial reconnected")
                return
            except (serial.SerialException, OSError):
                continue

    def send_next() -> None:
        nonlocal waiting_ack
        if waiting_ack is not None or not queue:
            return
        line = queue.pop(0)
        try:
            ser.write((line + "\n").encode())
            ser.flush()
        except serial.SerialException:
            queue.insert(0, line)
            reconnect_serial()
            return
        show = line if len(line) < 80 else line[:77] + "..."
        tee.write(f"→ {show} (from configurer)")
        waiting_ack = ack_for_line(line) or pending_expect

    try:
        while True:
            try:
                rlist = [ser.fileno(), srv.fileno()]
                readable, _, _ = select.select(rlist, [], [], 0.2)
            except (OSError, serial.SerialException, ValueError):
                reconnect_serial()
                continue

            if srv.fileno() in readable:
                conn, _ = srv.accept()
                conn.setblocking(True)
                conn.settimeout(2.0)
                try:
                    data = b""
                    while True:
                        try:
                            chunk = conn.recv(8192)
                        except socket.timeout:
                            break
                        if not chunk:
                            break
                        data += chunk
                        if data.endswith(b"\n") and (
                            b"LOAD END" in data.upper()
                            or data.upper().strip() in (b"MODE WRITE", b"MODE READ")
                        ):
                            # For LOAD batches, keep reading until idle
                            if b"LOAD END" in data.upper():
                                break
                            if data.upper().strip() in (b"MODE WRITE", b"MODE READ"):
                                break
                        # multi-line LOAD: stop when no more data shortly
                        if b"LOAD BEGIN" in data.upper() and b"LOAD END" in data.upper():
                            break
                except OSError:
                    conn.close()
                    continue

                text = data.decode("utf-8", "replace")
                cmds = [ln.strip() for ln in text.splitlines() if ln.strip()]
                if not cmds:
                    conn.close()
                    continue

                if pending is not None or queue or waiting_ack:
                    try:
                        conn.sendall(b"STATUS FAIL busy\n")
                    except OSError:
                        pass
                    conn.close()
                    continue

                pending = conn
                last = cmds[-1].upper()
                if last == "MODE WRITE":
                    pending_expect = EXPECT_MODE_WRITE
                elif last == "MODE READ" and len(cmds) == 1:
                    pending_expect = EXPECT_MODE_READ
                else:
                    pending_expect = EXPECT_PROFILE
                queue = cmds
                send_next()

            if ser.fileno() not in readable:
                continue

            while True:
                try:
                    line = ser.readline()
                except serial.SerialException:
                    reconnect_serial()
                    break
                if not line:
                    break
                text = line.decode("utf-8", "replace").rstrip()
                if not text:
                    continue
                tee.write(text)
                low = text.lower()

                if waiting_ack is not None:
                    if line_failed(text):
                        fail_pending()
                        continue
                    if waiting_ack.lower() in low:
                        # Finished this host line
                        done_ack = waiting_ack
                        waiting_ack = None
                        if queue:
                            send_next()
                        elif pending is not None and pending_expect is not None:
                            # Last line's ack may be the final expect
                            if pending_expect.lower() in low or (
                                done_ack.lower() == pending_expect.lower()
                            ):
                                try:
                                    pending.sendall(
                                        f"STATUS OK {pending_expect}\n".encode()
                                    )
                                except OSError:
                                    pass
                                pending.close()
                                pending = None
                                pending_expect = None
                            elif pending_expect.lower() in low:
                                try:
                                    pending.sendall(
                                        f"STATUS OK {pending_expect}\n".encode()
                                    )
                                except OSError:
                                    pass
                                pending.close()
                                pending = None
                                pending_expect = None
                        continue

                if pending is not None and pending_expect is not None and not queue and waiting_ack is None:
                    if pending_expect in low:
                        try:
                            pending.sendall(
                                f"STATUS OK {pending_expect}\n".encode()
                            )
                        except OSError:
                            pass
                        pending.close()
                        pending = None
                        pending_expect = None
                    elif line_failed(text):
                        fail_pending()
    except KeyboardInterrupt:
        tee.write("bye")
        return 0
    finally:
        fail_pending(b"")
        srv.close()
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
        try:
            ser.close()
        except Exception:
            pass
        tee.close()


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "action", choices=("listen", "read", "write", "parse", "catalog")
    )
    p.add_argument("--port", default="/dev/ttyACM0")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--xml", type=Path, default=DEFAULT_XML)
    p.add_argument("--timeout", type=float, default=60.0)
    p.add_argument(
        "--log-file",
        type=Path,
        default=None,
        help=f"listen log path (default: {DEFAULT_LOG_DIR}/YYYY-MM-DD_HH-MM-SS.log)",
    )
    args = p.parse_args()

    if args.action == "parse":
        return cmd_parse(args.xml)
    if args.action == "catalog":
        from catalog_kind_a import main as catalog_main

        return catalog_main()
    if args.action == "listen":
        return cmd_listen(args.port, args.baud, args.log_file)
    if args.action == "read":
        return cmd_read(args.port, args.baud, args.xml, args.timeout)
    return cmd_mode(
        args.port, args.baud, "MODE WRITE", EXPECT_MODE_WRITE, args.timeout
    )


if __name__ == "__main__":
    raise SystemExit(main())
