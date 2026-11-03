"""Summarize TH3 loop-handling intervals, with explicit session/transition exclusions."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import statistics


def fields(detail):
    return dict(re.findall(r"(\w+)=([^ ]+)", detail))


def stats(samples):
    values = [s["dt_us"] / 1000 for s in samples]
    return {
        "samples": len(values),
        "min_ms": min(values) if values else None,
        "mean_ms": statistics.mean(values) if values else None,
        "max_ms": max(values) if values else None,
    }


def analyze(source):
    with source.open(encoding="utf-8", newline="") as stream:
        rows = list(csv.DictReader(stream))
    boots = [i for i, row in enumerate(rows) if row["type"] == "boot"]
    if not boots:
        raise ValueError("No boot marker; capture a fresh boot before comparing timing.")
    session = rows[boots[-1]:]
    clock_rows = [fields(r["detail"]) for r in session if r["type"] == "timer_config"]
    phases = []
    for row in session:
        if row["type"] == "load":
            label = row["detail"].split()[0]
            if label not in {"on", "off"}:
                continue
            if phases:
                phases[-1]["end_ms"] = int(row["t_ms"])
            phases.append({"load": int(label == "on"), "start_ms": int(row["t_ms"]),
                           "end_ms": None, "timer": [], "millis": []})
        elif phases and row["type"] in {"timer", "millis"}:
            data = fields(row["detail"])
            if "loop_dt_us" not in data or "load" not in data:
                raise ValueError("A timing row lacks interval or load fields.")
            if int(data["load"]) != phases[-1]["load"]:
                raise ValueError("Timing row condition disagrees with LOAD marker.")
            phases[-1][row["type"]].append({
                "t_ms": int(row["t_ms"]), "dt_us": int(data["loop_dt_us"]),
                "pending": int(data["pending"]) if row["type"] == "timer" else None,
            })
    if not phases:
        raise ValueError("No explicit LOAD markers found.")
    phases[-1]["end_ms"] = int(session[-1]["t_ms"])
    kept = {(load, kind): [] for load in (0, 1) for kind in ("timer", "millis")}
    phase_reports = []
    all_pending = []
    for number, phase in enumerate(phases, 1):
        report = {key: phase[key] for key in ("load", "start_ms", "end_ms")}
        report["phase"] = number
        report["duration_ms"] = phase["end_ms"] - phase["start_ms"]
        report["counts"] = {}
        for kind in ("timer", "millis"):
            samples = phase[kind]
            # An interval in the first row may start before this condition began.
            candidates = samples[1:]
            eligible = [s for s in candidates if kind != "timer" or s["pending"] == 1]
            kept[(phase["load"], kind)].extend(eligible)
            report["counts"][kind] = {
                "raw": len(samples), "first_row_excluded": int(bool(samples)),
                "pending_not_one_excluded": len(candidates) - len(eligible),
                "kept": len(eligible),
            }
        all_pending.extend(s["pending"] for s in phase["timer"])
        phase_reports.append(report)
    summary = {
        "source": source.name,
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "input_rows": len(rows), "boot_markers": len(boots),
        "selected_boot": len(boots), "selected_boot_row_index": boots[-1],
        "clock_config": clock_rows[-1] if clock_rows else {},
        "start_ms": phases[0]["start_ms"], "end_ms": phases[-1]["end_ms"],
        "method": "Last boot; begin at first explicit LOAD event; split at every LOAD event; exclude first timer/millis row of each phase; interval statistics require pending=1 for timer.",
        "measurement": "loop_dt_us measures loop handling intervals, not ISR timestamps or hardware waveform jitter.",
        "load_on_condition": "Bounded busy work configured as 3000 us per loop; do not infer button operations without logged button events.",
        "button_events_in_selected_boot": sum(r["type"] == "button" for r in session),
        "pending_max": max(all_pending) if all_pending else None,
        "pending_gt_one_rows": sum(p > 1 for p in all_pending),
        "phases": phase_reports,
        "groups": [{"load": load, "kind": kind, **stats(kept[(load, kind)])}
                   for load in (0, 1) for kind in ("timer", "millis")],
    }
    return summary


def markdown(summary):
    source = summary["source"]
    lines = [
        "# TH03 - Kết quả phân tích thời gian xử lý",
        "",
        f"Nguồn: [{source}](../data/{source}). Các số dưới đây được tính từ log thực tế đã thu.",
        "",
        f"Tệp có {summary['boot_markers']} lần khởi động. Chọn phiên cuối, từ mốc LOAD đầu tiên {summary['start_ms']} ms đến {summary['end_ms']} ms; tách {len(summary['phases'])} đoạn LOAD. Loại dòng timer và millis đầu mỗi đoạn vì khoảng đo có thể bắt đầu trước khi đổi điều kiện. Thống kê timer chỉ dùng dòng pending=1; vẫn báo riêng các dòng có tick dồn.",
        "",
        "**Các khoảng đo là thời điểm loop xử lý sự kiện, không phải thời điểm ISR hay phép đo dạng sóng timer.**",
        "",
        "| Điều kiện | Loại | Số khoảng giữ lại | Min (ms) | Trung bình (ms) | Max (ms) |",
        "|---|---|---:|---:|---:|---:|",
    ]
    for group in summary["groups"]:
        label = "LOAD ON" if group["load"] else "LOAD OFF"
        if not group["samples"]:
            lines.append(f"| {label} | {group['kind']} | 0 | — | — | — |")
            continue
        lines.append(f"| {label} | {group['kind']} | {group['samples']} | {group['min_ms']:.3f} | {group['mean_ms']:.6f} | {group['max_ms']:.3f} |")
    lines.extend([
        "",
        f"Trong vùng phân tích: pending lớn nhất = {summary['pending_max']}; số dòng pending > 1 = {summary['pending_gt_one_rows']}.",
        f"Số sự kiện button trong phiên đã chọn: {summary['button_events_in_selected_boot']}. Với log hiện tại, mô tả LOAD ON là tác vụ phụ có giới hạn 3000 microsecond mỗi vòng; không nhận là đã có thao tác nút nếu không có sự kiện tương ứng.",
        "",
        "## Các đoạn đã dùng",
        "",
        "| Đoạn | LOAD | Từ (ms) | Đến (ms) | Timer giữ | Millis giữ |",
        "|---:|---|---:|---:|---:|---:|",
    ])
    for phase in summary["phases"]:
        lines.append(f"| {phase['phase']} | {'ON' if phase['load'] else 'OFF'} | {phase['start_ms']} | {phase['end_ms']} | {phase['counts']['timer']['kept']} | {phase['counts']['millis']['kept']} |")
    lines.extend([
        "",
        "## Diễn giải",
        "",
        "Timer phần cứng được cấu hình từ APB 80 MHz, chia 80 thành bộ đếm 1 MHz, ngắt tự nạp sau 100000 tick (100 ms). ISR tăng bộ đếm, loop nhận bộ đếm và ghi thời gian xử lý. millis cũng được kiểm với mục tiêu 100 ms trong TH3.",
        "",
        "Khi LOAD ON, tác vụ phụ làm thời điểm xử lý ở loop thay đổi. Không diễn giải khoảng xử lý ngắn hơn 100 ms thành chu kỳ ngắt phần cứng bị đổi. Khi một tick được xử lý trễ, khoảng đến lần xử lý tick sau có thể ngắn lại.",
        "",
        "Không dùng riêng các kết quả này để khẳng định đáp ứng thời gian thực trong mọi tình huống. Các phép so sánh với TH2 phải ghi rõ TH2 có chu kỳ lấy mẫu 3000 ms và tác vụ DHT/LCD khác TH3.",
        "",
    ])
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="TH3 events.csv")
    parser.add_argument("--out-dir", type=Path)
    args = parser.parse_args()
    source = args.source.resolve()
    summary = analyze(source)
    out_dir = args.out_dir or source.parent.parent / "figures"
    out_dir.mkdir(parents=True, exist_ok=True)
    stem = source.stem.removesuffix("-events") + "-timing-summary"
    json_path, md_path = out_dir / (stem + ".json"), out_dir / (stem + ".md")
    json_path.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    md_path.write_text(markdown(summary), encoding="utf-8")
    print(json.dumps({"boot_markers": summary["boot_markers"], "phases": len(summary["phases"]),
                      "pending_max": summary["pending_max"], "pending_gt_one_rows": summary["pending_gt_one_rows"],
                      "button_events": summary["button_events_in_selected_boot"], "groups": summary["groups"]}, indent=2))
    print("Saved", json_path)
    print("Saved", md_path)


if __name__ == "__main__":
    main()
