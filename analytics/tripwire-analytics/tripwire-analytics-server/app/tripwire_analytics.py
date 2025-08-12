# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear
import redis.asyncio as redis
import asyncio
import logging
import json
from collections import defaultdict, deque
import os

from tripwire_crossings import Tripwire, calculate_tripwire_crossings

ALERT_CHANNEL = os.environ.get('ALERT_CHANNEL', 'tripwire-analytics.alerts')
ALERT_PERIOD = os.environ.get('ALERT_PERIOD', 1.0)
TRIPWIRES_KEY = os.environ.get('TRIPWIRE_KEY', 'TATripwires')
TRIGGER_KEY = os.environ.get('TRIGGER_KEY', 'TATriggers')
DETECTION_CHANNEL_PREFIX = os.environ.get('REDIS_DETECTION_CHANNEL_PREFIX', 'detection.rz') + ':'

logger = logging.getLogger(__file__)


class TripwireAnalytics():
    # TODO: filter_size & overlap_size overridden in env
    def __init__(self, r: redis.Redis, filter_len = 8):
        self._r = r
        self._message_list = []
        self._frame_metadata_by_monitor = defaultdict(deque) # deque of messages by monitor, in received order
        self._filter_len = filter_len
        self._overlap_len = 1
        self._calculate_tripwires_impl = calculate_tripwire_crossings


    def enqueue_message(self, timestamp, message):
        self._message_list.append((timestamp, message))

        # check if there's more than 1 second worth of messages
        # TODO: should it be from last alert?
        oldest_message_time, _ = self._message_list[0]
        if timestamp - oldest_message_time >= ALERT_PERIOD:
            asyncio.ensure_future(self.check_for_alerts())

    async def check_for_alerts(self):
        try:
            if len(self._message_list) == 0:
                return # no messages
            
            current_messages = self._message_list
            self._message_list = [] # empty the message list

            tripwires = await self.get_tripwires()
            triggers = await self.get_triggers()
            alerts = self.apply_triggers(triggers, tripwires, current_messages)
        
            for alert in alerts:
                logger.debug(f'Publishing to channel {ALERT_CHANNEL}: {alert}')
                # TODO: add this to task list instead of awaiting?
                await self._r.publish(ALERT_CHANNEL, json.dumps(alert))

        except Exception as exc:
            logger.error('Exception in check_for_alerts:')
            logger.exception(exc)
            logger.error('Database or received messages likely corrupted, will keep trying..')


    async def get_tripwires(self):
        # Get tripwires from Redis
        raw_tripwires = await self._r.hgetall(TRIPWIRES_KEY)
        logger.debug(f'Raw tripwires from redis: {raw_tripwires}')

        # redis HGETALL returns in format {k : v} where k = region_id, v = full region in JSON string
        # Decode region JSON info
        tripwires = {id: json.loads(tripwire_str) for id, tripwire_str in raw_tripwires.items()}

        logger.debug(f'Parsed & validated tripwires: {tripwires}')
        return tripwires 


    async def get_triggers(self):
        # Get triggers from Redis
        raw_triggers = await self._r.hgetall(TRIGGER_KEY)
        logger.debug(f'Raw triggers from redis: {raw_triggers}')

        # redis HGETALL returns in format {k : v} where k = trigger_id, v = full trigger in JSON string
        # Convert to list of triggers
        triggers = [json.loads(trigger) for trigger in raw_triggers.values()]
        
        supported_trigger_conditions = ['flowrate']
        triggers = [t for t in triggers if t['trigger_condition'] in supported_trigger_conditions]
        
        logger.debug(f'Parsed & validated triggers: {triggers}')
        return triggers 


    def calculate_crossings(self, ts_frame_tuples, filter_len, window_len, tripwires):
        # convert to implementation's tripwire objects in item order
        def flatten_direction(d):
            entry = d['entry']
            exit = d['exit']
            return [
                [entry['x'], entry['y']],
                [exit['x'], exit['y']]
            ]
        def flatten_wire(wire):
            return [ [coord['x'], coord['y']] for coord in wire]
        
        tripwire_objs = [
            Tripwire(
                tripwire_id=t['tripwire_id'],
                name=t['tripwire_name'],
                direction=flatten_direction(t['direction']),
                wire=flatten_wire(t['wire'])
            ) for t in tripwires.values()]
        
        # extract frame metadata, drop timestamps
        _, frames = zip(*ts_frame_tuples) 

        # call algorithm
        try:
            logger.debug(
                f'Calling calculate_tripwires_impl: raw_data={frames!r}, '
                f'filter_size={filter_len!r}, '
                f'window_size={window_len!r}, '
                f'tripwires={tripwire_objs!r}'
                )
            counts = self._calculate_tripwires_impl(
                raw_data=frames,
                filter_size=filter_len,
                window_size = window_len,
                tripwires=tripwire_objs)
            logger.debug(f'Result of calculate_tripwires_impl: {counts}')
        except Exception:
            logger.exception('Exception in crossing calculation algorithm')
            counts = [(0, 0)] * len(tripwire_objs)
        
        # counts is list of (entries, exits) tuples for each tripwire in item order
        results = {}
        for counts, id in zip(counts, tripwires.keys()):
            results[id] = {
                'entries': counts[0],
                'exits': counts[1]
            }

        return results


    def make_alerts(self, triggers, tripwires, crossings, alert_time):
        alerts = []

        for trigger in triggers:
            tripwire_id = trigger['tripwire_id']
            crossing = crossings.get(tripwire_id, None)
            if crossing:
                cross_key = 'entries' if trigger['trigger_direction'] == 'entry' else 'exits'
                cross_count = crossing[cross_key]

                # TODO: calculate for limit and duration of trigger; just trigger on anything for now
                if cross_count > 0:
                    tripwire = tripwires[tripwire_id]
                    monitor_id = tripwire['monitor_id']
                    alert = {
                        'monitor_id': monitor_id,
                        'source_trigger': trigger,
                        'time': alert_time,
                        'crossings': {
                            'count': cross_count,
                            'duration': 1 # TODO: make it a real window
                        }
                    }
                    alerts.append(alert)

        return alerts

    def apply_triggers(self, triggers, tripwires, ts_frames):

        # Sort messages by monitor
        try:
            for recv_time, message in ts_frames:
                # first element is time message arrived, second element is Redis payload containing frame metadata
                frame_metadata = json.loads(message['data'])
                monitor = message['channel'][len(DETECTION_CHANNEL_PREFIX):] # monitor follows prefix
                self._frame_metadata_by_monitor[monitor].append((recv_time, frame_metadata))
        except Exception as exc:
            logger.error(f'Error parsing message: {message}')
            logger.exception(exc)
            return

        crossings = {}
        for monitor, frame_metadata in self._frame_metadata_by_monitor.items():
            # send only wires for this monitor
            monitor_wires = {wire_id: wire for wire_id, wire in tripwires.items() 
                             if wire['monitor_id'] == monitor}

            # calculate the window length to calculate crossings over;
            # need to overlap some number, and expect filter_len samples left over from last call
            window_len = len(frame_metadata) - self._filter_len + self._overlap_len

            # for very first call or long filter length (=negative window size), send all samples
            if window_len < 2:
                window_len = len(frame_metadata)

            logger.info(f'Calculating crossings for {len(frame_metadata)} frames in monitor {monitor} from t={frame_metadata[0][0]} to t={frame_metadata[-1][0]}; window_len = {window_len}')

            monitor_crossings = self.calculate_crossings(frame_metadata, self._filter_len, window_len, monitor_wires)
            crossings.update(monitor_crossings) # add to overall record           

            frames_to_drop = len(frame_metadata) - self._filter_len
            for _ in range(frames_to_drop):
                frame_metadata.popleft()

        last_ts = ts_frames[-1][0] # TODO: last timestamp per monitor, pass dict to make_alerts
        alerts = self.make_alerts(triggers, tripwires, crossings, last_ts)

        logger.info(f'Filtered {len(ts_frames)} Detection messages through {len(triggers)} trigger(s), generating {len(alerts)} alert(s)')
        return alerts

    def get_trigger_param(trigger, name):
        for param in trigger['params']:
            if param['name'] == name:
                return param['value']
            
        raise ValueError(f'{name} not in params for trigger {trigger}')