"""Decode distinct source frames from a UVC recording of the Windows motion meter."""
import argparse
import bisect
import csv
import json
import mmap
from pathlib import Path

import cv2 as cv
import numpy as np

# Marker and cell coordinates are in MotionMeter's 1440 x 900 drawing space.
MARKERS = {
    i: np.float32([(x, y), (x + 120, y), (x + 120, y + 120), (x, y + 120)])
    for i, x, y in ((21, 26, 26), (33, 1294, 26), (9, 26, 754), (46, 1294, 754))
}
GRID = np.float32([[
    (118 + 80 * col + dx, 211 + 82 * row + dy)
    for row in range(6) for col in range(16) for dx in (-15, 0, 15) for dy in (-10, 0, 10)
]])


def rows(path):
    with path.open(newline="") as source:
        return list(csv.DictReader(source))


def homography(quads):
    if quads.keys() != MARKERS.keys():
        return None
    src = np.concatenate(list(MARKERS.values()))
    dst = np.float32(np.concatenate([quads[i] for i in MARKERS]))
    matrix, _ = cv.findHomography(src, dst)
    if matrix is None:
        return None
    error = cv.perspectiveTransform(src[None], matrix)[0] - dst
    rms = np.sqrt(np.mean(np.sum(error ** 2, axis=1)))
    return (matrix, float(rms)) if rms <= 2 else None


def detect(detector, image, scale=1):
    quads, ids, _ = detector.detectMarkers(image)
    if ids is None:
        return {}
    return {int(i): q[0] / scale for i, q in zip(ids.ravel(), quads) if i in MARKERS}


def templates(frame, anchors, detector):
    for k in list(anchors)[:45]:
        image = frame(k)
        quads = detect(detector, image)
        if homography(quads) is None:
            quads = detect(detector, frame(k, full=True), scale=2)
        if homography(quads) is None:
            continue
        patches = []
        for marker, quad in quads.items():
            x, y = np.floor(quad.min(axis=0)).astype(int)
            right, bottom = np.ceil(quad.max(axis=0)).astype(int) + 1
            patches.append((marker, quad, image[y:bottom, x:right].copy(), x, y, right, bottom))
        return patches
    raise ValueError("The camera must see all four screen markers during calibration")


def track(image, patches):
    quads = {}
    pad = 15
    for marker, quad, patch, x, y, right, bottom in patches:
        if x < pad or y < pad or right + pad > image.shape[1] or bottom + pad > image.shape[0]:
            return {}
        region = image[y - pad:bottom + pad, x - pad:right + pad]
        scores = cv.matchTemplate(region, patch, cv.TM_CCOEFF_NORMED)
        _, peak, _, (dx, dy) = cv.minMaxLoc(scores)
        scores[max(0, dy - 2):dy + 3, max(0, dx - 2):dx + 3] = -1
        if peak < 0.70 or peak - cv.minMaxLoc(scores)[1] < 0.12:
            return {}
        quads[marker] = quad + [dx - pad, dy - pad]
    return quads


def extract_cells(directory, phases):
    index = rows(directory / "record.csv")
    offsets = [(int(r["offset"]), int(r["length"])) for r in index]
    qpc = np.array([int(r["arrival_qpc"]) for r in index], dtype=np.int64)
    times = np.array([int(r["sample_100ns"]) for r in index], dtype=np.int64)
    frequencies = {int(r["qpc_hz"]) for r in index}
    if len(frequencies) != 1 or not offsets or offsets[0][0] != 0:
        raise ValueError("Invalid camera index or QPC frequency")
    frequency = frequencies.pop()
    arrival = qpc / frequency
    start, end = np.searchsorted(arrival, [phases["start"] + 0.4, phases["closed"] + 0.2])
    anchors = range(start, end, 24)
    detector = cv.aruco.ArucoDetector(cv.aruco.getPredefinedDictionary(cv.aruco.DICT_4X4_50))
    with (directory / "record.bin").open("rb") as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as buf:
        if sum(offsets[-1]) != len(buf) or any(a + n != b for (a, n), (b, _) in zip(offsets, offsets[1:])):
            raise ValueError("Camera CSV offsets do not match the MJPEG recording")

        def frame(k, full=False):
            offset, size = offsets[k]
            jpeg = np.frombuffer(buf, np.uint8, count=size, offset=offset)
            image = cv.imdecode(jpeg, cv.IMREAD_GRAYSCALE if full else cv.IMREAD_REDUCED_GRAYSCALE_2)
            if image is None:
                raise ValueError(f"Invalid camera JPEG at frame {k}")
            return image

        patches = templates(frame, anchors, detector)
        poses = []
        direct = 0
        for k in anchors:
            image = frame(k)
            pose = homography(detect(detector, image))
            if pose is None:
                pose = homography(track(image, patches))
            else:
                direct += 1
            if pose is not None:
                poses.append((k, *pose))
        if not poses:
            raise ValueError("No reliable screen geometry")
        keys = [p[0] for p in poses]
        grids = [np.rint(cv.perspectiveTransform(GRID, h)[0]).astype(int).reshape(6, 16, 9, 2) for _, h, _ in poses]
        values = np.zeros((len(index), 6, 16), dtype=np.uint8)
        registered = np.zeros(len(index), dtype=bool)
        for k in range(start, end):
            i = bisect.bisect_left(keys, k)
            if i == len(keys) or (i and k - keys[i - 1] < keys[i] - k):
                i -= 1
            if abs(arrival[k] - arrival[keys[i]]) > 0.65:
                continue
            image = frame(k)
            x, y = grids[i][..., 0], grids[i][..., 1]
            if x.min() < 0 or y.min() < 0 or x.max() >= image.shape[1] or y.max() >= image.shape[0]:
                continue
            values[k] = np.median(image[y, x], axis=2)
            registered[k] = True
    data = dict(sampled_uvc_100ns=times, arrival_qpc=qpc, qpc_hz=np.int64(frequency),
                cell_luma=values, registered=registered, pose_frames=np.asarray(keys),
                poses=np.asarray([p[1] for p in poses]), geometry_checks=len(anchors),
                direct_aruco_checks=direct, max_rms_native_px=max(p[2] for p in poses) * 2)
    np.savez_compressed(directory / "camera-photometry.npz", **data)
    return data


def calibrate(samples, registered, arrival, timeline):
    # Each of the 16 sweep steps lights one cell in every row. Measure each cell
    # independently so that camera exposure and spatial shading do not bias the code.
    winners = np.argmax(samples, axis=2)
    hits = []
    delays = []
    for col in range(16):
        paint = timeline["cal", col]
        times = (arrival >= paint) & (arrival <= paint + 0.48)
        indices = np.flatnonzero(times & registered & (np.sum(winners == col, axis=1) >= 5))
        if len(indices) < 3:
            raise ValueError(f"Insufficient sweep coverage for cell {col}")
        hits.append(indices)
        delays.append(arrival[indices[0]] - paint)
    dark = np.median(samples[np.concatenate(hits)], axis=0)
    bright = np.column_stack([np.percentile(samples[indices, :, col], 80, axis=0)
                              for col, indices in enumerate(hits)])
    contrast = bright - dark
    if contrast.min() < 35:
        raise ValueError("Insufficient camera contrast across the timing window")
    return (samples - dark) / contrast, float(np.median(delays)), contrast


def decode(normalized, samples, registered, peak, margin):
    slots = np.argmax(normalized, axis=2)
    leaders = np.sort(normalized, axis=2)[:, :, -2:]
    raw_peak = np.take_along_axis(samples, slots[:, :, None], axis=2)[:, :, 0]
    row_ok = ((leaders[:, :, 1] >= peak) & (leaders[:, :, 1] - leaders[:, :, 0] >= margin)
              & (raw_peak - np.median(samples, axis=2) >= 25))

    # Inverses modulo 16 of the two independent nibble encodings in MotionMeter.Slots.
    low = (13 * (slots[:, 0] - 3)) % 16
    mid = (11 * (slots[:, 2] - 1)) % 16
    high = (7 * (slots[:, 4] - 2)) % 16
    check_high = (11 * (slots[:, 5] - 4)) % 16
    check_mid = (13 * (slots[:, 3] - 3 * check_high - 6)) % 16
    check_low = (7 * (slots[:, 1] - 5 * check_mid - 3 * check_high - 9)) % 16
    first = low + 16 * mid + 256 * high
    second = check_low + 16 * check_mid + 256 * check_high
    return first, registered & (first == second) & np.all(row_ok, axis=1), row_ok


def measure(ids, valid, row_ok, arrival, pts, phases, timeline, lag, output=None):
    controls = []
    for (phase, code), paint in timeline.items():
        if phase == "slow":
            controls.append(np.any(valid & (ids == code) & (arrival >= paint) & (arrival <= paint + 0.50)))
    static = (arrival >= phases["static"] + lag + 0.5) & (arrival <= phases["motion"] + lag - 0.2)
    moving = (arrival >= phases["motion"]) & (arrival <= phases["hold"] + lag + 0.28)
    candidates = np.flatnonzero(moving & valid & (ids > 0) & (ids <= 3600))
    paints = {code: stamp for (phase, code), stamp in timeline.items() if phase == "motion"}
    good = [k for k in candidates if ids[k] in paints and -0.005 <= arrival[k] - paints[ids[k]] <= 0.50]
    observed = {}
    for k in good:
        observed.setdefault(int(ids[k]), k)
    if len(observed) < 2:
        raise ValueError("Too few independently corroborated moving frames")
    seen = sorted(observed)
    sensor_start = float(np.interp(phases["motion"] + lag, arrival, pts))
    relative = pts[list(observed.values())] - sensor_start
    per_second = [int(np.sum((relative >= sec) & (relative < sec + 1))) for sec in range(60)]
    if output is not None:
        with output.open("w", newline="") as file:
            writer = csv.writer(file)
            writer.writerow(["source_id", "first_camera_frame", "camera_sample_s_from_motion", "paint_to_camera_arrival_s"])
            for (code, k), stamp in zip(observed.items(), relative):
                writer.writerow([code, k, round(stamp, 6), round(arrival[k] - paints[code], 6)])
    return dict(
        validated_source_ids=len(seen),
        distinct_visible_ids_per_second_lower_bound=round(len(seen) / (phases["hold"] - phases["motion"]), 3),
        first_last_visible_ids=[seen[0], seen[-1]], reverse_transitions=int(np.count_nonzero(np.diff(ids[good]) < 0)),
        accepted_camera_samples=len(good), corroborated_slow_control_ids=int(sum(controls)), slow_control_ids=len(controls),
        static_confident_samples=int(np.sum(valid & static)), static_total_camera_samples=int(np.sum(static)),
        static_wrong_ids=int(np.sum(valid & static & (ids != 0))), static_observed_ids=np.unique(ids[valid & static]).tolist(),
        motion_camera_samples=int(np.sum(moving)), all_six_rows_confident=int(np.sum(moving & np.all(row_ok, axis=1))),
        invalid_timing_or_unpainted_id=len(candidates) - len(good),
        visible_in_each_source_second=np.bincount([(v - 1) // 60 for v in seen], minlength=60).tolist(),
        visible_in_each_camera_second=per_second,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="capture, paint events and result directory")
    parser.add_argument("--replay", action="store_true", help="reuse extracted cell luminance from camera-photometry.npz")
    args = parser.parse_args()
    root = args.directory
    cv.setNumThreads(2)
    events = rows(root / "events.csv")
    hz = int(events[0]["qpc_hz"])
    phases = {r["phase"]: int(r["qpc"]) / hz for r in events}
    timeline = {}
    for r in rows(root / "paint.csv"):
        timeline.setdefault((r["phase"], int(r["id"])), int(r["qpc"]) / hz)
    if args.replay:
        with np.load(root / "camera-photometry.npz", allow_pickle=False) as saved:
            data = dict(saved)
    else:
        data = extract_cells(root, phases)
    arrival = data["arrival_qpc"] / data["qpc_hz"]
    pts = data["sampled_uvc_100ns"] / 1e7
    if (np.any(np.diff(pts) <= 0) or np.any(np.diff(arrival) < 0)
            or not arrival[0] < phases["start"] < phases["motion"] < phases["hold"] < phases["closed"] < arrival[-1]):
        raise ValueError("Camera timestamps do not bracket the complete motion window")
    samples = data["cell_luma"].astype(np.float32)
    registered = data["registered"]
    norm, lag, contrast = calibrate(samples, registered, arrival, timeline)
    results = {}
    for name, peak, margin in (("optical", 0.50, 0.12), ("higher_thresholds_sensitivity", 0.66, 0.23)):
        decoded = decode(norm, samples, registered, peak, margin)
        output = root / "optical-frames.csv" if name == "optical" else None
        results[name] = measure(*decoded, arrival, pts, phases, timeline, lag, output)
    report = dict(
        camera_frames=len(pts), camera_delivery_fps=round((len(pts) - 1) / (pts[-1] - pts[0]), 3),
        camera_capture_s=round(pts[-1] - pts[0], 4), actual_motion_duration_s=round(phases["hold"] - phases["motion"], 5),
        source_ids_painted=sum(phase == "motion" for phase, _ in timeline), possible_source_slots=3600,
        four_marker_geometry_checks_passed=len(data["pose_frames"]), calibration_slots=16,
        calibration_cell_luma_contrast_min_median=[round(float(contrast.min()), 2), round(float(np.median(contrast)), 2)],
        measured_median_calibration_paint_to_first_camera_arrival_s=round(lag, 4), **results,
    )
    for field in ("geometry_checks", "direct_aruco_checks", "max_rms_native_px"):
        if field in data:
            report[field] = float(data[field]) if field == "max_rms_native_px" else int(data[field])
    (root / "optical.json").write_text(json.dumps(report, indent=2) + "\n")
    print("Optically verified unique source IDs:", results["optical"]["validated_source_ids"])


if __name__ == "__main__":
    main()
