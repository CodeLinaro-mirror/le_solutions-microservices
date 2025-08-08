# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear
from collections import Counter, deque
from dataclasses import dataclass, field
from typing import Dict
from venv import logger


MAX_LOOKBACK = 5
MIN_IN_ZONE = 4


def point_in_polygon(poly, x, y):

    # Get the number of points in the polygon
    polygon=poly
    point = (x, y)
    n = len(polygon)

    # Initialize the counter for intersections
    intersection_count = 0

    # Iterate over all edges of the polygon
    for i in range(n):
        # Get the current vertex and the next one
        p1 = polygon[i]
        p2 = polygon[(i + 1) % n]

        # Check if the edge intersects with the ray cast from the point
        if (p1[1] <= point[1] and p2[1] > point[1]) or (p1[1] > point[1] and p2[1] <= point[1]):
            # Calculate the x-coordinate of the intersection
            intersection_x = (point[1] - p1[1]) * (p2[0] - p1[0]) / (p2[1] - p1[1]) + p1[0]

            # Check if the intersection is to the right of the point
            if intersection_x >= point[0]:
                # Increment the intersection count
                intersection_count += 1

    # If there are odd number of intersections, the point is inside the polygon
    return intersection_count % 2 != 0

@dataclass
class Occupant:
    id: str
    first_seen: float
    x: float
    y: float
    bounding_box: Dict[str, Dict[str, float]]
    last_seen: float = field(init=False)

    def __post_init__(self):
        self.last_seen = self.first_seen

    def update(self, timestamp: float, x: float, y: float, bounding_box: Dict[str, Dict[str, float]]):
        self.last_seen = timestamp
        self.x = x
        self.y = y
        self.bounding_box = bounding_box

    def to_dict(self):
        return {
            'bounding_box': self.bounding_box,
            'enter_time': self.first_seen,
            'exit_time': 0,
            'dwell_time': self.last_seen - self.first_seen,
            'person_id': self.id
        }

class RegionHistory:
    # Tracks occupants as Occupant instances.
    # Maintains detection history.
    # Updates or removes occupants based on presence in the region.
    # Determine occupancy based on MIN_IN_ZONE.

    def __init__(self, polygon):
        # polygon is list in format: [{'x': 0, 'y': 0}, {'x': 1, 'y': 0}, {'x': 0, 'y': 1}],
        # convert to [ (0,0), (1,0), etc.]
        self._region = [ (pt['x'], pt['y']) for pt in polygon]  # the region polygon
        self._frame_history = deque(maxlen=MAX_LOOKBACK)
        self._current_occupants = {} # key: occupant_id, value: Occupant instance

    def get_max_lookback(self):
        return MAX_LOOKBACK

    def add_detections(self, detection_records, current_time):
        """
        Adds detection records to the region's frame history and updates current occupants.
        Each record is annotated with whether the person is inside the region.
        """

        # If more than MAX_LOOKBACK new records, look at last MAX_LOOKBACK records
        detection_records = detection_records[-MAX_LOOKBACK:]

        # snapshot of previous occupants
        previous_occupants_snapshot = self._current_occupants.copy()
        previous_ids = set(self._current_occupants.keys())
        current_ids = set()

        # records are list by frame of dict with k: id (person id), v: record
        for frame in detection_records:
            updated_frame = {}
            for person_id, record in frame.items():
                person_id = str(person_id)
                #logger.debug(f"processing {person_id} {record['x']}, {record['y']}")
                in_region = point_in_polygon(self._region, record['x'], record['y'])
                record['in_region'] = in_region
                record['id'] = person_id
                updated_frame[person_id] = record

                if in_region:
                    #logger.info(f"person[{person_id}] is in region")
                    if person_id not in self._current_occupants:
                        self._current_occupants[person_id] = Occupant(
                            id=person_id,
                            first_seen=current_time,
                            x=record['x'],
                            y=record['y'],
                            bounding_box=record['bounding_box'])
                    else:
                        self._current_occupants[person_id].update(
                            timestamp=current_time,
                            x=record['x'],
                            y=record['y'],
                            bounding_box=record['bounding_box'])
                #else:
                #    logger.info(f"person[{person_id}] NOT in region")

            self._frame_history.append(updated_frame)

        # count number of times each person was found in region
        occupants_in_lookback = []
        for frame in self._frame_history:
            for person in frame.values():
                if person['in_region']:
                    occupants_in_lookback.append(person['id'])

        in_region_counts = Counter(occupants_in_lookback) # number of times each person appears in history

        # Track removed occupants
        # Update current occupants and remove those who no longer meet the MIN_IN_ZONE threshold
        #previous_occupants = set(self._current_occupants.keys())
        self._current_occupants = {
            k: v for k, v in self._current_occupants.items()
            if k in in_region_counts and in_region_counts[k] >= MIN_IN_ZONE
        }

        current_ids.update(self._current_occupants.keys())
        missing_ids = previous_ids - current_ids
        removed_occupants = {
            oid: previous_occupants_snapshot[oid]
            for oid in missing_ids
        }
        #removed_occupants = previous_occupants - set(self._current_occupants.keys())

        # Add new occupants who meet the MIN_IN_ZONE threshold
        for person_id, count in in_region_counts.items():
            if count >= MIN_IN_ZONE and person_id not in self._current_occupants:
                latest_record = None
                for frame in reversed(self._frame_history):
                    if person_id in frame:
                        latest_record = frame[person_id]
                        break

                if latest_record:
                    self._current_occupants[person_id] = Occupant(
                        id=str(person_id),
                        first_seen=current_time,
                        x=latest_record['x'],
                        y=latest_record['y'],
                        bounding_box=latest_record['bounding_box'] )

        #logger.debug(f"Frame history length: {len(self._frame_history)}")
        #logger.debug(f"In-region counts: {in_region_counts}")
        #logger.debug(f"Current occupants after filtering: {list(self._current_occupants.keys())}")


        return self._current_occupants, removed_occupants

    def get_occupant_data_for_alert(self, current_time, exit_time):
        alert_data = []
        for person_id in sorted(self._current_occupants):
            occupant = self._current_occupants[person_id]
            for frame in reversed(self._frame_history):
                if person_id in frame:
                    alert_data.append({
                        'bounding_box': frame[person_id]['bounding_box'],
                        'enter_time': occupant.first_seen,
                        'exit_time': exit_time,
                        'dwell_time': current_time - occupant.first_seen,
                        'person_id': occupant.id
                    })
                    break
        return alert_data
