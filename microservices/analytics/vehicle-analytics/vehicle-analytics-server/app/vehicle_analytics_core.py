# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from dataclasses import dataclass, field
from typing import Dict, Deque, Optional, Any, Tuple
import os
import logging
import sys
from typing import Dict,Deque, Optional, Any, Tuple, List
import numpy as np
import uuid

VEHICLE_LABELS = ['truck', 'car', 'bus', 'bicycle', 'motorcycle']

DETECTION_CHANNEL_PREFIX = os.environ.get('REDIS_DETECTION_CHANNEL_PREFIX', 'detection.vehicle') + ':'
HEATMAP_ROWS = int(os.environ.get('DEFAULT_HEATMAP_ROWS', 64))
HEATMAP_COLS = int(os.environ.get('DEFAULT_HEATMAP_COLS', 64))

logging.basicConfig(level=logging.INFO, stream=sys.stdout)
logger = logging.getLogger()


@dataclass
class Parameter:
    name: str
    value: str

    def to_dict(self):
        return { "name": self.name, "value": self.value }

@dataclass
class Vehicle:
    id: str
    first_seen: float
    x: float
    y: float
    bounding_box: Dict[str, Dict[str, float]]
    attributes: Optional[List[Parameter]] = field(default_factory=list)
    last_seen: float = field(init=False)

    def __post_init__(self):
        self.last_seen = self.first_seen

    def update(self, timestamp: float, x: float, y: float, bounding_box: Dict[str, Dict[str, float]], attributes: Optional[List[Parameter]] = None):

        self.last_seen= timestamp
        self.x = x
        self.y = y
        self.bounding_box = bounding_box
        if attributes is not None:
            self.attributes = attributes


    def to_dict(self):
        return {
            'vehicle_id': self.id,
            'enter_time': self.first_seen,
            'exit_time': 0,
            'dwell_time': self.last_seen - self.first_seen,
            'bounding_box': self.bounding_box,
            'attributes': [attr.to_dict() for attr in self.attributes] if self.attributes else []
        }

# Class to store count stats
@dataclass
class CountStats:
    count_min: int = field(default_factory=lambda: float('inf'))
    count_max: int = field(default_factory=lambda: float('-inf'))
    count_current: int = 0
    count_current_frame: Optional[Any] = None
    count_avg: float = 0.0
    count_avg_count: int = 0
    count_avg_sum: int = 0

    def __post_init__(self):
        # Ensure consistency if initialized with values
        if self.count_avg_count > 0:
            self.count_avg = self.count_avg_sum / self.count_avg_count
        else:
            self.count_avg = 0.0

    def update(self, new_count: int, frame: Optional[Any] = None):
        self.count_current = new_count
        self.count_current_frame = frame

        self.count_min = min(self.count_min, new_count)
        self.count_max = max(self.count_max, new_count)

        self.count_avg_sum += new_count
        self.count_avg_count += 1
        self.count_avg = 0 if self.count_avg_count == 0 else self.count_avg_sum / self.count_avg_count

    def reset(self, full: bool = True):
        self.count_min = float('inf')
        self.count_max = float('-inf')
        self.count_current = 0
        self.count_current_frame = None

        if full:
            self.count_avg = 0.0
            self.count_avg_count = 0
            self.count_avg_sum = 0

# Class to store heatmap stats
@dataclass
class HeatmapStats:
    rows: int = HEATMAP_ROWS
    cols: int = HEATMAP_COLS
    heatmap: list = field(init=False)

    def __post_init__(self):
        # Initialize heatmap to 0s
        self.heatmap = np.zeros((self.rows, self.cols), dtype=int)

    def reset(self):
        self.heatmap =  np.zeros((self.rows, self.cols), dtype=int)

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


def visualize_heatmap(heatmap):

    if np.all(heatmap==0):
        #logger.info("Heatmap is empty. Nothing to visualize.")
        return

    # Define the intensity levels using Unicode block characters
    intensity_levels = " ░▒▓█"

    # Function to map the matrix values to intensity levels
    def map_to_intensity(matrix):
        max_val = matrix.max()
        min_val = matrix.min()
        range_val = max_val - min_val
        step = range_val / (len(intensity_levels) - 1)

        def get_intensity(value):
            index = int((value - min_val) / step)
            return intensity_levels[index]

        return np.vectorize(get_intensity)(heatmap)

    # Map the matrix to intensity levels
    intensity_matrix = map_to_intensity(heatmap)

    # Print the heatmap to stdout
    for row in intensity_matrix:
        logger.info("".join(row))

def get_vehicle_coordinates(vehicle):
    '''
    To calculate position within the grid:
        - Use the midpoint of the bottom of the bounding box
        - otherwise not a valid vehicle
    '''
    bb = None
    coords = {}

    #logger.info(vehicle)

    rectangle = vehicle.rectangle
    #logger.info(rectangle)
    bb = {
        'top_left': {
            'x': rectangle.x,
            'y': rectangle.y
        },
        'bottom_right': {
            'x': rectangle.x + rectangle.width,
            'y': rectangle.y + rectangle.height
        }
    }

    if rectangle:
         #logger.info('using rectangle')
         coords['x'] = rectangle.x + rectangle.width / 2.0
         coords['y'] = rectangle.y + rectangle.height
    else:
         logger.info(f'{vehicle}')
         logger.error(f'Vehicle detected without bounding box!')
         coords = None

    return coords, bb

def make_alert(trigger, monitor_id, vehicles, alert_id, alert_time, end_time=0):

    if not alert_id:
        alert_id = str(uuid.uuid4())

    alert = {
        'monitor_id': monitor_id,
        'source_trigger': trigger,
        'alert_id': alert_id,
        'time': alert_time,
        'end_time': end_time,
        'vehicles': vehicles
    }

    logger.info(f"        -> {alert_id} time={alert_time}, end={end_time}")

    #logger.info(f'           {alert}')
    return alert

def convert_attributes(attributes):
    converted = {}
    for item in attributes:
        if isinstance(item, dict):
            for key, value in item.items():
                if hasattr(value, 'to_dict') and callable(value.to_dict):
                    converted[key] = value.to_dict()
                else:
                    converted[key] = value
        elif hasattr(item, 'to_dict') and callable(item.to_dict):
            converted.update(item.to_dict())
        else:
            # Handle unexpected formats gracefully
            converted[str(item)] = item
    return converted


def make_occupancy_alert(channel, trigger, alert_id, alert_time, end_time, current_occupants, removed_occupants):
    '''
    Create a new occupancy alert.
    '''

    trigger_id = trigger['trigger_id']
    trigger_condition = trigger['trigger_condition']
    vehicles = []

    if not alert_id:
        alert_id = str(uuid.uuid4())

    if current_occupants:
        for occupant in current_occupants.values():
            new_occupant = {
                'bounding_box': occupant.bounding_box,
                'enter_time': occupant.first_seen,
                'exit_time': 0,
                'dwell_time':  alert_time - occupant.first_seen,
                'vehicle_id': occupant.id,
                'attributes': convert_attributes(occupant.attributes)
            }
            vehicles.append(new_occupant)
    elif not current_occupants and removed_occupants:
        # Sending the cleared alert
        for occupant in removed_occupants.values():
            old_occupant = {
                'bounding_box': occupant.bounding_box,
                'enter_time': occupant.first_seen,
                'exit_time': alert_time,
                'dwell_time':  alert_time - occupant.first_seen,
                'vehicle_id': occupant.id,
                'attributes': convert_attributes(occupant.attributes)
            }
            vehicles.append(old_occupant)
    else:
        return None # no previous or current

    alert = {
        'source_trigger': trigger,
        'alert_id': alert_id,
        'time': alert_time,
        'end_time': end_time,
        'vehicles': vehicles,
        'type': 'occupancy'
        }

    #logger.debug(f'ALERT - {alert}')
    logger.info(f"        -> {alert_id} time={alert_time}, end={end_time}")
    #logger.debug(f"           {alert}")
    return alert

