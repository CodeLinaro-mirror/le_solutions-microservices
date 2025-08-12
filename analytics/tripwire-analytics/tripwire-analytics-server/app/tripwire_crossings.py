# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear
import math
import numpy as np
from collections import deque
from dataclasses import dataclass
import logging

logger = logging.getLogger(__file__)

class Frame:
    def __init__(self, objects, timestamp):
        self.objects = objects
        self.timestamp = timestamp

@dataclass(init=False)
class Tripwire:
    direction: list[list[float]]
    wire: list[list[float]]
    tripwire_id: str
    name: str

    def __init__(self, direction, wire, tripwire_id, name):
        # Safely handle tripwires that don't have expected structure
        if not isinstance(direction, list) or len(direction) < 2:
            # Provide a fallback if direction is missing or too short
            direction = [[0, 0], [1, 0]]
        if not isinstance(wire, list) or len(wire) < 2:
            # Provide a fallback if wire is missing or too short
            wire = [[0, 0], [1, 1]]
        self.direction = direction
        self.wire = wire
        self.tripwire_id = tripwire_id
        self.name = name if name else f"Tripwire {tripwire_id}"

def calculate_tripwire_crossings(raw_data, filter_size, window_size, tripwires):
    """
    1) Applies ankle-average temporal smoothing.
    2) Calculates tripwire crossings for the final window of frames.
    """
    # Early exit if no data
    if not raw_data:
        return [(0, 0) for _ in tripwires]

    frames = []
    smoothing_queues = {}

    # 1) Build frames with ankle-average smoothing
    for entry in raw_data:
        # Safely get timestamp (could be None, skip if missing)
        parameters = entry.get("parameters", {})
        timestamp = parameters.get("timestamp")
        if timestamp is None:
            continue

        objects_data = []
        # Safely get list of detected objects
        object_detection = entry.get("object_detection", [])
        if not isinstance(object_detection, list):
            continue

        for obj in object_detection:
            # Safely access the "landmarks" dictionary
            landmarks = obj.get("landmarks", {})
            if ("left_ankle" in landmarks and "right_ankle" in landmarks):
                lx = landmarks["left_ankle"].get("x")
                rx = landmarks["right_ankle"].get("x")
                ly = landmarks["left_ankle"].get("y")
                ry = landmarks["right_ankle"].get("y")

                # Check for valid numeric data
                if any(v is None for v in [lx, rx, ly, ry]):
                    continue  # skip if any coordinate is None

                # Compute averages
                x_avg = (lx + rx) / 2.0
                y_avg = (ly + ry) / 2.0

                # Insert into smoothing queue
                obj_id = obj.get("tracking_id", -1)
                if obj_id not in smoothing_queues:
                    smoothing_queues[obj_id] = deque(maxlen=filter_size)
                smoothing_queues[obj_id].append((x_avg, y_avg))

                # Average last 'filter_size' positions
                smoothed_vals = np.mean(smoothing_queues[obj_id], axis=0)
                smoothed_x, smoothed_y = smoothed_vals[0], smoothed_vals[1]

                # Build object record
                objects_data.append({
                    "id": obj_id,
                    "ankles_center": {"x": smoothed_x, "y": smoothed_y}
                })

        frames.append(Frame(objects_data, timestamp))

    # 2) Perform tripwire crossing calculations
    def vector(p1, p2):
        return (p2[0] - p1[0], p2[1] - p1[1])

    def angle_between(v1, v2):
        dot = v1[0] * v2[0] + v1[1] * v2[1]
        mag1 = math.hypot(*v1)
        mag2 = math.hypot(*v2)
        if mag1 < 1e-9 or mag2 < 1e-9:
            return 0.0  # No valid angle if one vector is near zero
        cos_theta = max(-1.0, min(1.0, dot / (mag1 * mag2)))
        return math.degrees(math.acos(cos_theta))

    def ccw(a, b, c):
        return (c[1] - a[1]) * (b[0] - a[0]) > (b[1] - a[1]) * (c[0] - a[0])

    def intersect(p1, q1, p2, q2):
        return (ccw(p1, p2, q2) != ccw(q1, p2, q2) and 
                ccw(p1, q1, p2) != ccw(p1, q1, q2))

    def crosses_tripwire(trajectory, twire):
        seg_count = len(twire.wire) - 1
        # If wire is invalid, no crossing
        if seg_count < 1:
            return 0
        d_vec = vector(twire.direction[0], twire.direction[1])
        for i in range(seg_count):
            w_start, w_end = twire.wire[i], twire.wire[i + 1]
            for j in range(len(trajectory) - 1):
                p1, p2 = trajectory[j], trajectory[j + 1]
                if intersect(p1, p2, w_start, w_end):
                    angle = angle_between(vector(p1, p2), d_vec)
                    return 1 if angle < 90 else -1
        return 0

    # If insufficient frames, we can't compute tripwire events
    if len(frames) < window_size:
        return [(0, 0) for _ in tripwires]

    # Use all the frames 
    obj_trajectories = {}

    frame_idx = -1
    for f in frames:
        frame_idx += 1
        if not f.objects:
            continue
        for o in f.objects:
            pid = o["id"]
            pos_dict = o["ankles_center"]
            # Ensure numeric
            if not isinstance(pos_dict, dict):
                continue
            pos_x, pos_y = pos_dict.get("x"), pos_dict.get("y")
            if pos_x is None or pos_y is None:
                continue
            pos = (pos_x, pos_y)
            trajectory = obj_trajectories.setdefault(pid, [])
            if frame_idx < filter_size:
                if len(trajectory) > 0:
                    trajectory.pop()
                trajectory.append(pos)
            else:
                if len(trajectory) > 1:
                    trajectory.pop()
                trajectory.append(pos)

    # Count crossing events
    results = []
    for twire in tripwires:
        entries = exits = 0
        for traj in obj_trajectories.values():
            if len(traj) < 2:
                # Need at least 2 points to form a segment
                continue
            outcome = crosses_tripwire(traj, twire)
            if outcome == 1:
                entries += 1
            elif outcome == -1:
                exits += 1
        results.append((entries, exits))

    return results
