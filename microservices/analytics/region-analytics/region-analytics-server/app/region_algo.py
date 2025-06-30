# Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear
from collections import Counter, deque

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


class RegionHistory:
    _region = None # the region polygon
    _frame_history = None
    _current_occupants = None

    def __init__(self, polygon):
        # polygon is list in format: [{'x': 0, 'y': 0}, {'x': 1, 'y': 0}, {'x': 0, 'y': 1}],
        # convert to [ (0,0), (1,0), etc.]
        self._region = [ (pt['x'], pt['y']) for pt in polygon]
        self._frame_history = deque(maxlen=MAX_LOOKBACK)
        self._current_occupants = set()
        
    def get_max_lookback(self):
        return MAX_LOOKBACK

    def add_detections(self, detection_records):
        # If more than MAX_LOOKBACK new records, look at last MAX_LOOKBACK records
        detection_records = detection_records[-MAX_LOOKBACK:]

        # records are list by frame of dict with k: id (person id), v: record
        for frame in detection_records:
            # add distance and in_region calculations to record
            for record in frame.values():
                record['in_region'] = point_in_polygon(self._region, record['x'], record['y'])
                
            self._frame_history.append(frame)

        # count number of times each person was found in region
        occupants_in_lookback = []
        for frame in self._frame_history:
            for person in frame.values():
                if person['in_region']:
                    occupants_in_lookback.append(person['id'])
        
        in_region_counts = Counter(occupants_in_lookback) # number of times each person appears in history

        # if a person is not in the history at all, remove them from occupants
        self._current_occupants &= {*in_region_counts.keys()} # keys: set of ids seen at least once

        # If a person is found >= MIN_IN_ZONE times, they are an occupant
        new_occupants = {id for id, id_count in in_region_counts.items() if id_count >= MIN_IN_ZONE}
        self._current_occupants |= new_occupants # add new occupants to set

        return len(self._current_occupants)


    def get_occupant_data_for_alert(self):
        alert_data = []
        for id in sorted(self._current_occupants):
            # look backward in history for the last instance of each occupant, just
            # in case they disappeared in the final frame; probably unlikely, but
            # should be somewhere in history
            for frame in reversed(self._frame_history):
                if id in frame:
                    # The alert data per occupant is just the bounding box
                    alert_data.append({
                        'bounding_box': frame[id]['bounding_box']
                        })
                    break
        return alert_data
