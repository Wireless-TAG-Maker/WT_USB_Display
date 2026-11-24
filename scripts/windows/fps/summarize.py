"""Compare presentation receipts, board counters and optically read frame IDs."""
import argparse
import csv
import json
from pathlib import Path
import statistics

ERROR_COUNTERS = ("crc_errors", "header_errors", "decode_errors", "present_errors", "dropped", "timeouts", "ack_timeouts")


def rows(path):
    with path.open(newline="") as source:
        return list(csv.DictReader(source))


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def bounds(values):
    return [round(v, 3) for v in (min(values), statistics.median(values), max(values))]


def rate(records, timestamp, counter):
    stamps = [float(r[timestamp]) for r in records]
    counts = [int(r[counter]) for r in records]
    elapsed = [b - a for a, b in zip(stamps, stamps[1:])]
    increments = [b - a for a, b in zip(counts, counts[1:])]
    if not elapsed or min(elapsed) <= 0 or min(increments) < 0:
        raise ValueError(f"Insufficient samples or a discontinuity in {timestamp}/{counter}")
    intervals = [n / dt for n, dt in zip(increments, elapsed)]
    span = stamps[-1] - stamps[0]
    frames = counts[-1] - counts[0]
    result = dict(seconds=round(span, 5), frames=frames, fps=round(frames / span, 4), intervals=len(intervals),
                  sample_interval_s_min_median_max=bounds(elapsed), interval_fps_min_median_max=bounds(intervals))
    return result, list(zip(stamps[1:], intervals))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    root = parser.parse_args().directory
    events = {r["phase"]: (int(r["utc_filetime"]) - 116444736000000000) / 1e7 for r in rows(root / "events.csv")}
    phases = [
        ("current", "current", 0, float("inf")),
        ("static", "controlled", events["static"], events["motion"]),
        ("motion", "controlled", events["motion"], events["hold"]),
    ]
    scene, chart, workers = {}, [], set()
    for name, trace, start, end in phases:
        ack = [r for r in rows(root / (trace + "-ack.csv")) if start <= float(r["windows_unix_s"]) < end]
        uart = [r for r in rows(root / (trace + "-uart.csv")) if start <= float(r["reader_unix_s"]) < end]
        workers.update((r["worker_pid"], r["capture_pid"]) for r in ack)
        a, intervals = rate(ack, "windows_unix_s", "ack_ok")
        b, _ = rate(uart, "reader_unix_s", "total_present")
        # Both snapshots used for a measured interval must lie inside the same phase.
        epoch = float(ack[0]["windows_unix_s"]) if name == "current" else start
        chart.extend((name, round(stamp - epoch, 4), round(value, 4)) for stamp, value in intervals)
        scene[name] = dict(
            presentation_ack=a, board_uart_presented=b,
            board_reported_refresh_hz_min_median_max=bounds([float(r["refresh_hz"]) for r in uart]),
            max_board_error_counters={key: max(int(r[key]) for r in uart) for key in ERROR_COUNTERS},
            first_last_ack_time_unix=[float(ack[0]["windows_unix_s"]), float(ack[-1]["windows_unix_s"])],
        )
    data = read_json(root / "optical.json")
    optical = data["optical"]
    chart.extend(("camera", sec + 0.5, n) for sec, n in enumerate(optical["visible_in_each_camera_second"]))
    with (root / "timeline.csv").open("w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(["series", "elapsed_s", "fps"])
        writer.writerows(chart)
    observed = rows(root / "optical-frames.csv")
    times = [float(r["camera_sample_s_from_motion"]) for r in observed]
    if len(times) < 2 or times[-1] <= times[0]:
        raise ValueError("Too few independently observed updates")
    in_window = sum(0 <= t < 60 for t in times)
    if (len(times) != optical["validated_source_ids"] or len(times) != sum(optical["visible_in_each_source_second"])
            or in_window != sum(optical["visible_in_each_camera_second"])):
        raise ValueError("Inconsistent optical frame counts")
    if (optical["slow_control_ids"] != 12 or optical["corroborated_slow_control_ids"] != 12
            or optical["static_wrong_ids"] or optical["static_observed_ids"] != [0]
            or optical["reverse_transitions"] or optical["invalid_timing_or_unpainted_id"]):
        raise ValueError("Optical controls failed. See optical.json for the affected frame counts")
    before, after = (read_json(root / n) for n in ("before.json", "after.json"))
    last_ack = rows(root / "controlled-ack.csv")[-1]
    last_presentation = float(last_ack["windows_unix_s"])
    if len(workers) != 1 or (last_ack["worker_pid"], last_ack["capture_pid"]) not in workers:
        raise ValueError("Streaming process changed during measurement")
    if before != after or last_presentation < events["closed"]:
        raise ValueError("Display layout changed or streaming stopped before the test window closed")
    span = times[-1] - times[0]
    report = {
        "display_layout_preserved": before == after,
        "same_streaming_process_before_and_after": last_presentation >= events["closed"],
        "streaming_still_active_at_unix": last_presentation,
        "scenes": scene,
        "optical": {
            "camera_delivery_fps": data["camera_delivery_fps"], "full_motion_duration_s": data["actual_motion_duration_s"],
            "source_ids_painted": data["source_ids_painted"], "camera_verified_unique_ids": len(times),
            "distinct_ids_per_second_of_source_sequence": optical["distinct_visible_ids_per_second_lower_bound"],
            "first_to_last_optical_sample_s": round(span, 6),
            "visible_new_ids_per_s_first_to_last_lower_bound": round((len(times) - 1) / span, 4),
            "unique_ids_in_aligned_60s_camera_window": in_window,
            "unique_ids_outside_aligned_60s_camera_window": len(times) - in_window,
            "verified_per_camera_second_min_median_max": bounds(optical["visible_in_each_camera_second"]),
            "slow_check_ids_seen": optical["corroborated_slow_control_ids"],
            "slow_check_ids_painted": optical["slow_control_ids"],
            "static_samples_all_showing_one_id": optical["static_confident_samples"], "static_wrong_ids": optical["static_wrong_ids"],
            "reverse_transitions": optical["reverse_transitions"],
            "higher_confidence_threshold_lower_bound": data["higher_thresholds_sensitivity"]["distinct_visible_ids_per_second_lower_bound"],
            "detail": "optical.json", "individual_ids": "optical-frames.csv",
        },
    }
    (root / "fps-result.json").write_text(json.dumps(report, indent=2) + "\n")
    for name, detail in scene.items():
        if any(detail["max_board_error_counters"].values()):
            raise ValueError(f"P4 reported errors in the {name} sample. See fps-result.json")
    for name, detail in scene.items():
        ack = detail["presentation_ack"]
        print(f"{name}: {ack['fps']:.2f} presentation ACK/s over {ack['seconds']:.2f} s")
    print(f"Optical lower bound: {report['optical']['visible_new_ids_per_s_first_to_last_lower_bound']:.2f} updates/s")


if __name__ == "__main__":
    main()
