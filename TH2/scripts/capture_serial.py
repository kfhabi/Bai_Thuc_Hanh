"""Save actual TH02 Serial measurements and experiment logs to separate files.

python scripts/capture_serial.py --port COMx --out-dir data --samples 40
Close other Serial monitors first. Press ESP32 EN after capture starts if needed.
"""
from __future__ import annotations
import argparse
import csv
from datetime import datetime
from pathlib import Path
import re
import time

SAMPLE_FIELDS = "boot_id session_id mode seq t_ms dt_ms temp_c rh_pct valid error adc_raw adc_percent adc_mv temp_ma5 rh_ma5 read_ms".split()
EVENT_FIELDS = "boot_id session_id mode id t_ms type detail".split()


def parse_line(line):
    """Return only recognizable measurement/event records; preserve raw elsewhere."""
    if line.startswith("#EVENT,"):
        row = next(csv.reader([line[len("#EVENT,"):]]))
        return ("event", row) if len(row) == len(EVENT_FIELDS) else None
    if not line or line.startswith("#") or line.startswith("boot_id,"):
        return None
    row = next(csv.reader([line]))
    if (len(row) == len(SAMPLE_FIELDS) and row[2] in ("millis", "delay")
            and row[3].isdigit() and row[8] in ("0", "1")):
        return "sample", row
    return None


class CaptureFiles:
    def __init__(self, directory, stamp):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.stamp = stamp
        self.streams = {}
        self.writers = {}
        self.counts = {}

    def write(self, kind, row):
        identity = tuple(row[:3])
        key = identity + (kind,)
        if key not in self.writers:
            safe = lambda text: re.sub(r"[^A-Za-z0-9_.-]", "_", text)
            suffix = "samples" if kind == "sample" else "events"
            stem = f"TH02-{safe(row[2])}-{safe(row[1])}-{self.stamp}-{suffix}.csv"
            path = self.directory / stem
            stream = path.open("x", encoding="utf-8", newline="")
            writer = csv.writer(stream)
            writer.writerow(SAMPLE_FIELDS if kind == "sample" else EVENT_FIELDS)
            self.streams[key], self.writers[key] = stream, writer
            print(f"Saving: {path}", flush=True)
        self.writers[key].writerow(row)
        self.streams[key].flush()
        if kind == "sample":
            self.counts[identity] = self.counts.get(identity, 0) + 1
        return self.counts.get(identity, 0)

    def close(self):
        for stream in self.streams.values():
            stream.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="Actual ESP32 USB Serial port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--out-dir", type=Path, default=Path("data"))
    parser.add_argument("--samples", type=int, default=40, help="Per-session rows; 0 means until Ctrl+C")
    parser.add_argument("--timeout", type=float, default=300, help="Total capture seconds; 0 means no timeout")
    args = parser.parse_args()
    if args.samples < 0 or args.timeout < 0:
        parser.error("samples and timeout must be nonnegative")
    try:
        import serial
    except ImportError:
        parser.error("Install dependencies: python -m pip install -r scripts/requirements.txt")
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    files = CaptureFiles(args.out_dir, stamp)
    raw_path = args.out_dir / f"TH02-{stamp}-serial.log"
    completed = False
    started = time.monotonic()
    try:
        with serial.Serial(args.port, args.baud, timeout=1) as device, raw_path.open("x", encoding="utf-8") as raw:
            print(f"Connected {args.port}. Raw log: {raw_path}. Press EN now to include boot/I2C logs.", flush=True)
            while not args.timeout or time.monotonic() - started < args.timeout:
                payload = device.readline()
                if not payload:
                    continue
                line = payload.decode("utf-8", errors="replace").rstrip("\r\n")
                raw.write(line + "\n")
                raw.flush()
                print(line, flush=True)
                try:
                    record = parse_line(line)
                except csv.Error:
                    record = None
                if record:
                    count = files.write(*record)
                    if record[0] == "sample" and args.samples and count >= args.samples:
                        completed = True
                        break
    except KeyboardInterrupt:
        print("Capture stopped; received data has been kept.")
    except serial.SerialException as exc:
        raise SystemExit(f"Cannot use {args.port}: {exc}. Close Serial Monitor and verify the USB port.")
    finally:
        files.close()
    if args.samples and not completed:
        raise SystemExit("Target count not reached. Received rows remain saved; inspect the raw log.")


if __name__ == "__main__":
    main()
