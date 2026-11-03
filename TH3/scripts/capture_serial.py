"""Capture TH3 device evidence without inventing or altering measurement results."""
import argparse
import csv
from datetime import datetime, timedelta, timezone
from pathlib import Path
import sys
import threading
import time

try:
    import serial
except ImportError:
    raise SystemExit("Missing pyserial. Use the PlatformIO Python environment or install scripts/requirements.txt.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="Actual device port, e.g. COM3")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=180, help="0 = until Ctrl+C")
    parser.add_argument("--out", type=Path, default=Path("data"))
    parser.add_argument("--interactive", action="store_true", help="Type firmware commands while capturing")
    parser.add_argument("--quiet-timing", action="store_true", help="Hide timer/millis rows on screen while still saving every row")
    args = parser.parse_args()
    if args.seconds < 0:
        parser.error("--seconds must be >= 0")
    args.out.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone(timedelta(hours=7))).strftime("%Y%m%d-%H%M%S-%f")
    stem = args.out / ("TH03-" + stamp)
    stop = threading.Event()
    allowed = {"HELP", "STATUS", "START", "STOP", "RESET", "FAULT ON", "FAULT OFF",
               "PWM 0", "PWM 50", "PWM 100", "LOAD ON", "LOAD OFF",
               "WIFI CONFIG", "WIFI CANCEL", "WIFI FORGET"}
    try:
        device = serial.Serial(args.port, args.baud, timeout=0.1, write_timeout=1)
    except serial.SerialException as exc:
        raise SystemExit(f"Cannot open {args.port}: {exc}. Close any other Serial Monitor first.")

    def read_commands():
        while not stop.is_set():
            try:
                command = input().strip().upper()
            except (EOFError, KeyboardInterrupt):
                return
            if stop.is_set():
                return
            if command not in allowed:
                print("Unknown command; type HELP. Do not type Wi-Fi passwords here.", flush=True)
                continue
            try:
                device.write((command + "\n").encode("ascii"))
            except (serial.SerialException, OSError):
                return

    with device, open(str(stem) + "-serial.log", "w", encoding="utf-8", newline="") as raw, \
            open(str(stem) + "-events.csv", "w", encoding="utf-8", newline="") as events_file, \
            open(str(stem) + "-timer.csv", "w", encoding="utf-8", newline="") as timer_file:
        events, timer = csv.writer(events_file), csv.writer(timer_file)
        events.writerow(["t_ms", "type", "detail"])
        timer.writerow(["t_ms", "type", "detail"])
        print(f"Connected: {args.port}, {args.baud}. Press EN/RESET once to capture the boot.", flush=True)
        print(f"Files: {stem}-serial.log / -events.csv / -timer.csv", flush=True)
        if args.quiet_timing:
            print("Timer/millis rows are hidden on screen and fully recorded in the files.", flush=True)
        if args.interactive:
            print("Type HELP or other firmware commands and press Enter. Ctrl+C stops capture.", flush=True)
            threading.Thread(target=read_commands, daemon=True).start()
        pending = b""
        started = time.monotonic()
        try:
            while not args.seconds or time.monotonic() - started < args.seconds:
                chunk = device.read(max(1, min(device.in_waiting, 4096)))
                if not chunk:
                    continue
                pending += chunk
                while b"\n" in pending:
                    line_bytes, pending = pending.split(b"\n", 1)
                    line = line_bytes.decode("utf-8", errors="replace").rstrip("\r")
                    raw.write(line + "\n")
                    try:
                        row = next(csv.reader([line]))
                    except csv.Error:
                        row = []
                    timing_row = len(row) == 3 and row[0].isdigit() and row[1] in {"timer", "millis"}
                    if not (args.quiet_timing and timing_row):
                        print(line, flush=True)
                    if len(row) == 3 and row[0].isdigit():
                        events.writerow(row)
                        if row[1] in {"timer", "millis", "timer_config"}:
                            timer.writerow(row)
                raw.flush(); events_file.flush(); timer_file.flush()
                if len(pending) > 65536:
                    raw.write(pending.decode("utf-8", errors="replace"))
                    pending = b""
        except KeyboardInterrupt:
            print("\nCapture stopped.", flush=True)
        except (serial.SerialException, OSError) as exc:
            print(f"\nDevice disconnected: {exc}", file=sys.stderr)
        finally:
            stop.set()
            if pending:
                raw.write(pending.decode("utf-8", errors="replace"))
    print("Saved actual device output; fill in test results from observation and these logs.")


if __name__ == "__main__":
    main()
