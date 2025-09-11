# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from sqlite3.dbapi2 import Timestamp
import redis.asyncio as redis
import asyncio
import logging
import json
from collections import defaultdict, deque, Counter
import os
from typing import Dict, Deque, Optional, Any, Tuple
from dataclasses import dataclass, field
from datetime import datetime, timezone
import mysql.connector
import numpy as np
import sys
import time
from types import SimpleNamespace
import uuid
import vehicle_analytics_core as va_core
import vehicle_analytics_database as va_db

from vehicle_analytics_core import (
    logger, VEHICLE_LABELS, DETECTION_CHANNEL_PREFIX, HeatmapStats, CountStats, Vehicle, HEATMAP_ROWS, HEATMAP_COLS, Parameter
)

HEATMAP_PROCESSING_INTERVAL = os.environ.get('HEATMAP_PROCESSING_INTERVAL', 2.0) # 2 seconds
STATISTICS_COMMIT_INTERVAL = os.environ.get('STATISTICS_COMMIT_INTERVAL', 60.0) # 60 seconds
ALERT_PERIOD = os.environ.get('ALERT_PERIOD', 1.0)
REGION_KEY = os.environ.get('REGION_KEY', 'VARegions')
TRIGGER_KEY = os.environ.get('TRIGGER_KEY', 'VATriggers')
ALERT_CHANNEL = os.environ.get('ALERT_CHANNEL', 'vehicle-analytics.alerts')
ANALYTICS_CHANNEL = os.environ.get('ANALYTICS_CHANNEL', 'vehicle-analytics.analytics')

class RegionHistory:
    # Tracks vehicles as Vehicle instances.
    # Maintains detection history.
    # Updates or removes vehicles based on presence in the region.
    # Determine occupancy based on MIN_IN_ZONE.

    def __init__(self, region):
        # polygon is list in format: [{'x': 0, 'y': 0}, {'x': 1, 'y': 0}, {'x': 0, 'y': 1}],
        # convert to [ (0,0), (1,0), etc.]
        polygon = region['coordinates']
        self._id = region['region_id']
        self._region = region
        self._polygon = [ (pt['x'], pt['y']) for pt in polygon]  # the region polygon
        self._current_vehicles = {} # key: vehicle_id, value: Vehicle instance
        self._removed_vehicles = {} # key: vehicle_id, value: Vehicle instance
        self._stats = CountStats()   # private CountStats instance
        self._last_update = 0

        # Statistics
        self.heatmap_statistics = HeatmapStats()
        self.count_statistics = CountStats()


    def count(self):
        return len(self._current_vehicles)

    def add_detections(self, msg_time, frame, check_region = True):
        """
        Adds detection records to the region's frame history and updates current vehicles.
        Each record is annotated with whether the vehicle is inside the region.
        """

        # no objects detected
        if not hasattr(frame, 'object_detection'):
            return

        # snapshot of previous vehicles
        previous_vehicles_snapshot = self._current_vehicles.copy()
        previous_ids = set(self._current_vehicles.keys())

        # Reset current vehicles for the new frame
        self._current_vehicles = {}

        # check all detected objects
        for vehicle in frame.object_detection:

            if vehicle.label not in VEHICLE_LABELS:
                continue # obj is not a vehicle, skip

            coords, bb = va_core.get_vehicle_coordinates(vehicle)

            if not coords:
                continue # rectanble founds is not valid
            x = coords['x']
            y = coords['y']
            in_region = va_core.point_in_polygon(self._polygon, x, y) if check_region else True
            attributes=[Parameter(name="type", value=vehicle.label).to_dict()]

            #logger.debug(f'{frame}')

            if in_region:
                vehicle_id = str(vehicle.tracking_id)
                #logger.info(vehicle)
                self._current_vehicles[vehicle_id] = Vehicle(
                    id=vehicle_id,
                    first_seen=msg_time,
                    x=x,
                    y=y,
                    bounding_box=bb,
                    attributes = attributes
                 )

                if vehicle_id in previous_ids:
                    self._current_vehicles[vehicle_id].update(timestamp=msg_time, x=x, y=y, bounding_box = bb, attributes=attributes)
                    self._current_vehicles[vehicle_id].first_seen = previous_vehicles_snapshot[vehicle_id].first_seen

                    #logger.info(self._current_vehicles[vehicle_id])
                    #else:
                    #    logger.info(f"vehicle[{vehicle_id}] NOT in region")

        # Removed
        current_ids = set(self._current_vehicles.keys())
        missing_ids = previous_ids - current_ids
        self._removed_vehicles = {
            oid: previous_vehicles_snapshot[oid]
            for oid in missing_ids
        }

        self._last_update = msg_time

        # New only
        #new_ids = set(self._current_vehicles.keys()) - previous_ids
        #new_vehicles = {
        #    vid: self._current_vehicles[vid]
        #    for vid in new_ids
        #}
        for vehicle_id, vehicle in self._removed_vehicles.items():
            vehicle.last_seen = msg_time

        return self._current_vehicles, self._removed_vehicles


class VehicleAnalytics():
    # TODO: filter_size & overlap_size overridden in env
    def __init__(self, r: redis.Redis, filter_len = 8):
        self._r = r
        self._message_list = []
        self.region_histories: Dict[str, RegionHistory] = {}
        self.monitor_histories: Dict[str, RegionHistory] = {}
        self.last_trigger_occupancy: Dict[str, Dict[str, Tuple[float, int, str]]] = {}
        self.last_trigger_loitering: Dict[str, Tuple[Dict[str, Vehicle], float, str]] = {}
        self.monitor_histories: Dict[str, RegionHistory] = {}

        # Configuration
        self.previous_regions = {}
        self._last_raw_regions = None
        self._regions = None
        self._last_raw_triggers = None
        self._triggers = None

        # Timing
        self.last_statistics_commit = 0
        self.last_heatmap_processing = 0

        # Concurrency
        self.count_lock = asyncio.Lock()

    def enqueue_message(self, timestamp, message):
        self._message_list.append((timestamp, message))

        # check if there's more than 1 second worth of messages
        # TODO: should it be from last alert?
        oldest_message_time, _ = self._message_list[0]
        if timestamp - oldest_message_time >= ALERT_PERIOD:
            asyncio.ensure_future(self.check_for_alerts())

    async def update_regions(self):
        # Get regions from Redis
        raw_regions = await self._r.hgetall(REGION_KEY)

        if not raw_regions:
            return

        logger.debug(f'Raw regions from redis: {raw_regions}')

        # redis HGETALL returns in format {k : v} where k = region_id, v = full region in JSON string
        # Decode region JSON info
        # Skip processing if data hasn't changed
        if raw_regions != self._last_raw_regions:
            self._last_raw_regions = raw_regions
            self._regions = {id: json.loads(region_str) for id, region_str in raw_regions.items()}
            logger.info(f"Regions updated: {self._regions}")
        else:
            logger.debug("No change in regions; skipping update.")

        return self._regions

    async def update_triggers(self):
        # Get triggers from Redis
        raw_triggers = await self._r.hgetall(TRIGGER_KEY)

        if not raw_triggers:
            return

        logger.debug(f'Raw triggers from redis: {raw_triggers}')

        # redis HGETALL returns in format {k : v} where k = trigger_id, v = full trigger in JSON string
        # Convert to list of
        trigger_objs = {}
        if raw_triggers != self._last_raw_triggers:
            self._last_raw_triggers = raw_triggers
            trigger_objs = [json.loads(trigger) for trigger in raw_triggers.values()]
        else:
            logger.debug("No change in triggers; skipping update.")
            return self._triggers

        supported_trigger_conditions = [
            'occupancy_changed',
            'occupancy_over',
            'occupancy_under',
            'loitering_over',
            'vehicle_count_changed',
            'vehicle_count_over',
            'vehicle_count_under',
            'vehicle_dwell_over'
        ]

        triggers = []
        for trigger in trigger_objs:
            if trigger['trigger_condition'] not in supported_trigger_conditions:
                logger.warning(f'Ignoring trigger with unsupported condition: "{trigger}"')
                continue
            triggers.append(trigger)

        if triggers == []:
            logger.debug(f'No triggers found at {TRIGGER_KEY}; adding default')
            triggers = [
                 {
                    "monitor_id": "0",
                    "trigger_id": "0xDEADBEEF",
                    "trigger_name": "more than 0 vehicle",
                    "trigger_condition": "occupancy_over",
                    "params": [{"name":"threshold","value":"0"}]
                }
            ]

        logger.debug(f'Parsed & validated triggers: {triggers}')
        self._triggers = triggers
        logger.info(f"Triggers updated: {self._triggers}")
        return triggers

    async def get_triggers(self):
        if not self._triggers:
            await self.update_triggers()
        return self._triggers

    async def get_regions(self):
        if not self._regions:
            await self.update_regions()
        return self._regions

    async def check_for_alerts(self):
        try:
            if len(self._message_list) == 0:
                return # no messages

            current_messages = self._message_list
            self._message_list = [] # empty the message list

            current_regions = await self.get_regions()
            triggers = await self.get_triggers()

            # Process count before checking alerts
            recent_history = self.parse_messages(current_messages)
            await self.update_count_statistics(recent_history, current_regions)

            # Process count before checking alerts
            alerts = self.apply_triggers(triggers, current_regions, current_messages)

            if alerts is None:
                logger.debug("apply_triggers returned None — skipping alert processing.")
                return

            for alert in alerts:
                logger.info(f'Publishing to channel {ALERT_CHANNEL}: {alert}')
                # TODO: add this to task list instead of awaiting?
                await self._r.publish(ALERT_CHANNEL, json.dumps(alert))

            now = time.time()

            #if self.heatmap_processing_interval_elapsed(now):
            #    await self.update_heatmap_statistics(recent_history)
            #    last_heatmap_processing = now

        except Exception as exc:
            logger.error('Exception in check_for_alerts:')
            logger.exception(exc)
            logger.error('Database or received messages likely corrupted, will keep trying..')

    def parse_messages(self, messages):
        '''
        Parse incoming messages and group by monitor
        '''

        messages_by_monitor = defaultdict(list) # list of messages by monitor, in received order

        try:
            for recv_time, message in messages:
                # first element is time message arrived, second element is Redis payload
                msg_obj = json.loads(message['data'], object_hook=lambda d: SimpleNamespace(**d))
                monitor = message['channel'][len(DETECTION_CHANNEL_PREFIX):] # monitor follows prefix
                messages_by_monitor[monitor].append((recv_time, msg_obj))
        except Exception as exc:
            logger.error(f'Error parsing message: {message}')
            logger.exception(exc)
            return None

        return messages_by_monitor

    def get_trigger_param(self, trigger, name):
        for param in trigger['params']:
            if param['name'] == name:
                return param['value']

        raise ValueError(f'{name} not in params for trigger {trigger}')


    def alert_period_has_elapsed(self, now):
        if len(self.message_list) == 0:
            return False # no messages
        oldest_message_time, _ = self.message_list[0]
        return (now - oldest_message_time) >= ALERT_PERIOD

    def statistics_commit_interval_elapsed(self, now):
        return (now - self.last_statistics_commit) >= STATISTICS_COMMIT_INTERVAL


    def heatmap_processing_interval_elapsed(self, now):
        return (now - self.last_heatmap_processing) >= HEATMAP_PROCESSING_INTERVAL

    def get_vehicle_count(self, frame) -> int:
        '''
        Count all vehicle-type objects in the object_detection list
        '''
        #logger.info(f'get_vehicle_count - {frame}')
        if not hasattr(frame, 'object_detection'):
            logger.info("no object detection")
            return 0
        else:
            return sum(1 for obj in frame.object_detection if obj.label.lower() in VEHICLE_LABELS)

    def group_triggers_by_region(self, triggers):
        '''
        Group triggers by region_id
        '''
        triggers_by_region = defaultdict(list)

        for trigger in triggers:
            triggers_by_region[trigger['region_id']].append(trigger)

        return triggers_by_region

    def get_loitering_alert(self, trigger, alert_time, occupants, previous_occupants, threshold):
        '''
        Create the loitering alerts.

        Expected behavior:
        - Send alert when loitering starts (first vehicles exceed threshold)
        - Send alert when loitering clears (no more vehicles)
        - Send alert when composition changes (vehicles added or removed from loitering set)
        - Do NOT send alert if same vehicles are still loitering with no changes
        '''

        trigger_id = trigger[f'trigger_id']
        loitering_occupants = []
        monitor_id = trigger['monitor_id']

        logger.info(f'- Loitering Check - {trigger["trigger_id"]}')
        
        # Identify which occupants are currently loitering
        for occupant in occupants.values():
            dwell_time = alert_time - occupant.first_seen
            occupant.loitering = dwell_time > threshold

            if occupant.loitering:
                loitering_occupants.append(occupant)
                logger.info(f'   ({occupant.id}): {alert_time-occupant.first_seen:.2f} seconds...is loitering > {threshold} secs')
            else:
                dwell_str = "Entered the region" if dwell_time == 0 else f"{dwell_time:.2f} seconds"
                logger.info(f'   ({occupant.id}): {dwell_str}')

        # Log any vehicles that exited the region
        if previous_occupants:
            for occupant_id in previous_occupants:
                logger.info(f'   ({occupant_id}): Exited the region')

        # Get current and previous loitering vehicle sets
        current_loitering_ids = {o.id for o in loitering_occupants}
        previous_loitering_ids = set()
        
        if trigger_id in self.last_trigger_loitering:
            previous_occupants_dict, _, _ = self.last_trigger_loitering[trigger_id]
            previous_loitering_ids = set(previous_occupants_dict.keys())

        # Check if composition has changed
        composition_changed = current_loitering_ids != previous_loitering_ids
        
        # Case 1: No loitering occupants - clear alert if one exists
        if not loitering_occupants:
            if trigger_id in self.last_trigger_loitering:
                alert_occupants = []
                occupants_dict, last_timestamp, alert_id = self.last_trigger_loitering[trigger_id]

                # Mark all previous occupants as exited
                for occupant_id, occupant in occupants_dict.items():
                    alert_occupants.append({
                        'bounding_box': occupant.bounding_box,
                        'enter_time': occupant.first_seen,
                        'exit_time': alert_time,
                        'dwell_time': alert_time - occupant.first_seen,
                        'vehicle_id': occupant.id,
                        'attributes': getattr(occupant, 'attributes', [])
                    })

                del self.last_trigger_loitering[trigger_id]
                logger.info(f"*** LOITERING_OVER {threshold} CLEARED ***")
                return va_core.make_alert(trigger, monitor_id, alert_occupants, alert_id, last_timestamp, alert_time)
            else:
                return None

        # Case 2: There are loitering occupants
        alert_occupants = []
        
        # Create alert occupants list for current loitering vehicles
        for occupant in loitering_occupants:
            alert_occupants.append({
                'bounding_box': occupant.bounding_box,
                'enter_time': occupant.first_seen,
                'exit_time': 0,
                'dwell_time': alert_time - occupant.first_seen,
                'vehicle_id': occupant.id,
                'attributes': getattr(occupant, 'attributes', [])
            })

        # Add exited vehicles to alert (vehicles that were loitering but no longer are)
        if trigger_id in self.last_trigger_loitering:
            previous_occupants_dict, _, alert_id = self.last_trigger_loitering[trigger_id]
            
            for prev_id, prev_occupant in previous_occupants_dict.items():
                if prev_id not in current_loitering_ids:
                    alert_occupants.append({
                        'bounding_box': prev_occupant.bounding_box,
                        'enter_time': prev_occupant.first_seen,
                        'exit_time': alert_time,
                        'dwell_time': alert_time - prev_occupant.first_seen,
                        'vehicle_id': prev_occupant.id,
                        'attributes': getattr(prev_occupant, 'attributes', [])
                    })
        else:
            # First time - generate new alert ID
            alert_id = str(uuid.uuid4())

        # Update the tracking state
        current_occupants_dict = {occupant.id: occupant for occupant in loitering_occupants}
        self.last_trigger_loitering[trigger_id] = (current_occupants_dict, alert_time, alert_id)

        # Determine if we should send an alert
        should_send_alert = False
        alert_message = ""

        if not previous_loitering_ids:
            # First loitering alert
            should_send_alert = True
            alert_message = f"*** LOITERING_OVER > {threshold} STARTED ***"
        elif composition_changed:
            # Composition changed - vehicles added or removed
            added_vehicles = current_loitering_ids - previous_loitering_ids
            removed_vehicles = previous_loitering_ids - current_loitering_ids
            
            should_send_alert = True
            changes = []
            if added_vehicles:
                changes.append(f"added: {list(added_vehicles)}")
            if removed_vehicles:
                changes.append(f"removed: {list(removed_vehicles)}")
            alert_message = f"*** LOITERING_OVER > {threshold} COMPOSITION CHANGED *** ({', '.join(changes)})"
        else:
            # Same vehicles still loitering - no alert needed
            alert_message = f"*** LOITERING_OVER > {threshold} ACTIVE *** : No composition change"

        logger.info(alert_message)

        if should_send_alert:
            return va_core.make_alert(trigger, monitor_id, alert_occupants, alert_id, alert_time, 0)
        else:
            return None

    def apply_triggers(self, triggers, regions, messages):

        if triggers is None:
            logger.debug("apply_triggers returned None — skipping alert processing.")
            return []

        try:
            #logger.debug(f'apply_triggers {triggers}')
            logger.info(f'\r\n---Checking Alerts---')
            all_alerts = []
            for trigger in triggers:
                current_occupancy = 0
                monitor_id = trigger['monitor_id']
                region_id = trigger.get('region_id', 'missing')
                trigger_condition = trigger['trigger_condition']
                trigger_id = trigger['trigger_id']
                alert_id = str(uuid.uuid4())
                alert_time = 0
                vehicles = []
                removed_vehicles = []

                if region_id is None or region_id == 'missing':
                    #logger.info(f"[{trigger['trigger_id']}]")
                    # FOV trigger
                    current_occupancy = self.monitor_histories[monitor_id].count()
                    alert_time = self.monitor_histories[monitor_id]._last_update
                    vehicles = self.monitor_histories[monitor_id]._current_vehicles
                    removed_vehicles = self.monitor_histories[monitor_id]._removed_vehicles
                else:
                    #logger.info(f"[{trigger['trigger_id']}]")
                    # region trigger
                    current_occupancy = self.region_histories[region_id].count()
                    alert_time = self.region_histories[region_id]._last_update
                    vehicles = self.region_histories[region_id]._current_vehicles
                    removed_vehicles = self.region_histories[region_id]._removed_vehicles

                trigger_data = self.last_trigger_occupancy.get(trigger_id, {}).get(trigger_condition, (0.0, 0, alert_id))
                timestamp, last_occupancy, alert_id = trigger_data
                occupancy_alerts = False

                match trigger_condition:

                    case 'occupancy_changed' | 'vehicle_count_changed':
                        occupancy_alerts = last_occupancy != current_occupancy
                        alert_log = f'OCCUPANCY_CHANGED {last_occupancy}->{current_occupancy}: ({current_occupancy})'
                    case 'occupancy_over' | 'vehicle_count_over':
                        threshold = int(self.get_trigger_param(trigger, 'threshold'))
                        occupancy_alerts = current_occupancy > threshold
                        alert_log = f'OCCUPANCY_OVER > {threshold}:  ({current_occupancy})'
                    case 'occupancy_under' | 'vehicle_count_under':
                        threshold = int(self.get_trigger_param(trigger, 'threshold'))
                        occupancy_alerts = current_occupancy < threshold
                        alert_log = f'OCCUPANCY_UNDER < {threshold}:  ({current_occupancy})'
                    case 'loitering_over' | 'vehicle_dwell_over':
                        threshold = int(self.get_trigger_param(trigger, 'threshold')) / 1000.0 # ms to sec
                        loitering_alert = self.get_loitering_alert(trigger, alert_time, vehicles, removed_vehicles, threshold)
                    case _:
                        raise RuntimeError(f'Unexpected trigger_condition \'{trigger_condition}\' in {trigger}')

                if loitering_alert:
                    all_alerts.append(loitering_alert)                            
                elif occupancy_alerts:
                    alert_log = f"*** {alert_log} {'STARTED' if timestamp == 0.0 else 'ACTIVE'} *** : {trigger_id} - {region_id}"
                    logging.info(alert_log)
                    end_time = 0
                    if not trigger_id in self.last_trigger_occupancy:
                        self.last_trigger_occupancy[trigger_id] = {}
                    self.last_trigger_occupancy[trigger_id][trigger_condition] = (alert_time, current_occupancy, alert_id)
                    alert = va_core.make_occupancy_alert(monitor_id, trigger, alert_id, alert_time, end_time, vehicles, removed_vehicles)
                    all_alerts.append(alert)
                    #logger.info(f"        -> {alert_id} time={alert_time}, end={end_time}")
                elif timestamp != 0.0:
                    alert_log = f"*** {alert_log} CLEARED ***  : {trigger_id} - {region_id}"
                    end_time = alert_time
                    logging.info(alert_log)
                    alert = va_core.make_occupancy_alert(monitor_id, trigger, alert_id, alert_time, end_time, None, removed_vehicles)
                    all_alerts.append(alert)
                    del self.last_trigger_occupancy[trigger_id][trigger_condition] # no more active alert


            logger.info(f'Filtered {len(messages)} Detection messages through {len(triggers)} trigger(s), generating {len(all_alerts)} alert(s)')

            return all_alerts
        except Exception as exc:
           # logger.error(f"Exception while applying trigger '{trigger}' to redis message '{message}': {str(exc)}")
            logger.exception(exc)
            # raise(exc) # : remove in release
            return []

    async def update_count_statistics(self, recent_history, regions):
        '''
        Update counts on last frames using the majority vote analytics method
        since objects may not be detected in each frame
        '''

        logger.debug(f"update_count_statistics()")

        # recent_frames: each channel maps to a deque of (recv_time, msg_obj) tuples
        def count_vehicles_in_frames_by_channel(recent_frames: Dict[str, Deque]) -> Dict[str, Deque[Tuple[int, Tuple[Any, Any]]]]:
            # Extracts vehicle counts from frames for each channel
            count_in_frames: Dict[str, Deque[Tuple[int, Tuple[Any, Any]]]] = defaultdict(deque)

            for channel, frames in recent_frames.items():
                #logger.debug(f"count_vehicle_in_frames_by_channel {channel} {len(frames)}")
                if not frames:
                    logger.error(f'[{channel}]: no frames found!')
                    continue

                #frames = [msg_obj for _, msg_obj in items]

                for frame in frames:
                    msg_time, msg_obj = frame
                    #logger.debug(f"[{channel}]: {frame}")
                    count = self.get_vehicle_count(msg_obj)
                    logger.debug(f'...[{channel}]: {count}')
                    count_in_frames[channel].append((count, frame))

            return count_in_frames

        # counts_by_channel: each channel maps to a deque of count and frame
        #   count: the number of vehicles detected in a frame.
        #   frame: the original frame data (which itself is a tuple of msg_time and msg_obj).
        def get_majority_vote(counts_by_channel: Dict[str, Deque[Tuple[int, Tuple[Any, Any]]]]) -> Dict[str, Optional[Tuple[int, Tuple[Any, Any]]]]:
            '''
            Returns the most common vehicle count per channel
            '''

            majority_vote_result: Dict[str, Optional[Tuple[int, Any]]] = {}

            # Apply majority vote
            # (1)[0]: the top-most/most common value
            for channel, count_frame_pairs in counts_by_channel.items():
                logger.debug(f"get_majority_vote {channel} {len(count_frame_pairs)}")
                if count_frame_pairs:
                    count_freq = Counter([count for count, _ in count_frame_pairs])
                    most_common_count, _ = count_freq.most_common(1)[0]

                    logger.debug(f'{count_freq} {most_common_count}')

                    # Find the first frame with the most common count
                    for count, frame in count_frame_pairs:
                        if count == most_common_count:
                            majority_vote_result[channel] = (most_common_count, frame)
                            break
                        else:
                            majority_vote_result[channel] = None

            return majority_vote_result


        # For each channel, get the vehicles count detected in each frame.
        # Apply majority vote across frames per channel to get count.
        count_in_frames = count_vehicles_in_frames_by_channel(recent_history)
        majority_vote_result = get_majority_vote(count_in_frames)

        for channel, result in majority_vote_result.items():

            if result is None:
                continue

            count, frame = result
            msg_time, frame_obj = frame

            monitor_id = channel.replace(DETECTION_CHANNEL_PREFIX, "")
            #logger.info(f'[ch:{monitor_id}]: {count} vehicles')

            async with self.count_lock:
                logger.info(f'\r\n---Vehicle Detection---')
                if monitor_id not in self.monitor_histories:
                    coords = [{"x": 0, "y": 0}, {"x": 0, "y": 0}, {"x": 0, "y": 0}, {"x": 0, "y": 0}]
                    fov = {
                        "monitor_id": monitor_id,
                        "region_id": None,
                        "region_name": "fov" + monitor_id,
                        "coordinates": coords
                    }
                    logger.info(f"New monitor history for {monitor_id}")
                    self.monitor_histories[monitor_id] = RegionHistory(fov)

                # Update the stats for the channel
                self.monitor_histories[monitor_id].count_statistics.update(count, frame)
                current, removed = self.monitor_histories[monitor_id].add_detections(msg_time, frame_obj, False)

                logger.info(f'[ch:{monitor_id}]: {len(current)} vehicles')


                for vehicle_id, vehicle in current.items():
                    logger.info(f"  ({vehicle_id}) Enter {vehicle.first_seen} ({vehicle.x:.2f}, {vehicle.y:.2f})")

                for vehicle_id, vehicle in removed.items():
                    vehicle.last_seen = msg_time
                    logger.info(f"  ({vehicle_id}) Exit dwell= {(vehicle.last_seen - vehicle.first_seen):.2f} ({vehicle.first_seen:.2f} {vehicle.last_seen:.2f})")


                if regions is None:
                    return

                # Update the region_histories, initialize if necessary
                for region in regions.values():
                    #logger.info(f'{region}')
                    region_id = region['region_id']

                    if region_id not in self.region_histories:
                        logging.info(f"New region history for {region_id}")
                        self.region_histories[region_id] = RegionHistory(region)

                    # Update the stats for the region
                    self.region_histories[region_id].count_statistics.update(count, frame)
                    current, removed = self.region_histories[region_id].add_detections(msg_time, frame_obj)


                    logger.info(f'    [{region_id}]: {len(current)} vehicles detected')


                    for vehicle_id, vehicle in current.items():
                        logger.info(f"          ({vehicle_id}) Enter {vehicle.first_seen} dwell= {(vehicle.last_seen - vehicle.first_seen):.2f}")

                    for vehicle_id, vehicle in removed.items():
                        vehicle.last_seen = msg_time
                        logger.info(f"          ({vehicle_id}) Exit dwell= {(vehicle.last_seen - vehicle.first_seen):.2f} ({vehicle.first_seen:.2f} {vehicle.last_seen:.2f})")

        # Update the heatmap
        await self.update_heatmap_statistics()

    async def update_heatmap_statistics(self):
        '''
        Updates the current heatmap statistics.
        The heatmap is a histogram table representing a snapshot of the vehicle_count in the fov.
        '''
        #logger.debug("update_heatmap_statistics()")
        for monitor_id, history in self.monitor_histories.items():
            vehicles = history._current_vehicles

            for vehicle_id, vehicle in vehicles.items():
                x_idx = min(max(int(float(vehicle.x) * (HEATMAP_COLS - 1)), 0), HEATMAP_COLS - 1)
                y_idx = min(max(int(float(vehicle.y) * (HEATMAP_ROWS - 1)), 0), HEATMAP_ROWS - 1)

                # Update heatmap for the channel
                if monitor_id not in self.monitor_histories:
                    coords = [{"x": 0, "y": 0}, {"x": 0, "y": 0}, {"x": 0, "y": 0}, {"x": 0, "y": 0}]
                    fov = {
                        "monitor_id": monitor_id,
                        "region_id": None,
                        "region_name": "fov" + monitor_id,
                        "coordinates": coords
                    }
                    logger.info(f"New monitor history for {monitor_id}")
                    self.monitor_histories[monitor_id] = RegionHistory(fov)

                try:
                    self.monitor_histories[monitor_id].heatmap_statistics.heatmap[y_idx, x_idx] += 1
                except Exception as exc:
                    logger.exception(exc)
                    logger.info(f'{x_idx}, {y_idx}')

    def get_trigger_param(self, trigger, name):
        for param in trigger['params']:
            if param['name'] == name:
                return param['value']

        raise ValueError(f'{name} not in params for trigger {trigger}')

    async def init(self):
        logger.info("Initializing vehicle-analytics-server...")
        await va_db.connect_to_db()
        await self.update_triggers()
        await self.update_regions()

    async def statistics_task(self):
        '''
        Task that saves the current statistics to the database at a fixed interval.
        '''

        logger.info(f'Statistics Task Started...')
        while True:
            now = time.time()
            if self.statistics_commit_interval_elapsed(now):
                for monitor_id, history in self.monitor_histories.items():
                    await va_db.save_heatmap_statistics(self.count_lock, monitor_id, self.heatmap_statistics, visualize=True)

                for monitor_id, history in self.monitor_histories.items():
                    await va_db.save_count_statistics(self.count_lock, monitor_id, history.count_statistics)

                for region_id, history in self.region_histories.items():
                    await va_db.save_regions_count_statistics(self.count_lock, region_id, history.count_statistics)

                self.last_statistics_commit = time.time()

            await asyncio.sleep(STATISTICS_COMMIT_INTERVAL)

        logger.info(f'Statistics Task Stopped...')

    def start_statistics_task(self):
        statistics_process_task = None
        statistics_process_task = asyncio.create_task(self.statistics_task())
        return statistics_process_task

    def deInit(self):
        if self.statistics_task:
            self.statistics_task.cancel()

        if db_connection:
            db_connection.close()
            db_connection = None

    async def run_count_query(self, r : redis.Redis, token, monitor_id, region_id, start_time, end_time):
        # Save the current count statistics in case it needs to be included
        for monitor_id, history in self.monitor_histories.items():
            await va_db.save_count_statistics(self.count_lock, monitor_id, history.count_statistics)

        for region_id, history in self.region_histories.items():
            await va_db.save_regions_count_statistics(self.count_lock, region_id, history.count_statistics)

        await va_db.run_count_query(r, token, monitor_id, region_id, start_time, end_time, ANALYTICS_CHANNEL)

    async def run_heatmap_query(self, r : redis.Redis, token, monitor_id, start_time, end_time):
        for monitor_id, history in self.monitor_histories.items():
            await va_db.save_heatmap_statistics(self.count_lock, monitor_id, history.heatmap_statistics, visualize=False)

        await va_db.run_heatmap_query(r, token, monitor_id, start_time, end_time, ANALYTICS_CHANNEL)
