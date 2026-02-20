# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import app.vehicle_analytics_server as vas
import json
import pytest

# VAS defaults, reset for each test
DEFAULT_TIME_DRIFT_LIMIT = 3.0

@pytest.fixture(scope='function', autouse=True)
def set_pas_context_to_default():
    vas.TIME_DRIFT_LIMIT_SECS = DEFAULT_TIME_DRIFT_LIMIT
    vas.seconds_offsets_by_channel = {}
    vas.LOOKBACK_FRAMES=1
    vas.frame_history_by_channel = {}



@pytest.fixture
async def triggers():
    class FakeRedis:
        async def hgetall(self, *args):
            return {}
    return await pas.get_triggers(FakeRedis())

async def test_get_triggers_returns_fake_trigger(triggers):
    assert len(triggers) == 1, "Expect one hard-coded trigger with faked Redis"

def make_message(type : str):
    MESSAGE_TIMESTAMP = "1234567890" # in nanoseconds; 1.234567890 in seconds
    frame_message = {
                'object_detection': [
                    {
                        'label': 'car',
                        'rectangle': {
                            # Make x0, y0, x1, y1 = 0.1, 0.2, 0.3, 0.4
                            'x': 0.1,
                            'y': 0.2,
                            'width': (0.3 - 0.1),
                            'height': (0.4 - 0.2)
                        }

                    }
                ],
                'parameters': {
                    'timestamp': MESSAGE_TIMESTAMP # source timestamp from monitor in nsec
                }
            }

    return frame_message


def make_message_list(messages, channel='0'):
    message_list = [
        # messages are tuples of (loop_timestamp, message)
        (0, # loop timestamp
            {
                'channel': 'detection.vehicle:' + channel,
                'data': json.dumps(message)
            }
        )
        for message in messages
    ]
    return message_list

# TODO: Add Tests!


# Time conversion tests

# TODO: when real time comes from camera, can remove the sys time override
def test_msg_and_system_time_same():
    msg_time = 111.123456789
    msg_ts = str(msg_time * 1e9) # timestamp a string of nanoseconds since 0-time
    sys_time = 987654321

    assert sys_time == vas.convert_msg_timestamp_to_epoch_time(
        msg_ts=msg_ts, sys_time=sys_time), 'Function just takes system time and ignores msg_ts'

# TODO: channels clean up stats if no messages received for some amount of time?
