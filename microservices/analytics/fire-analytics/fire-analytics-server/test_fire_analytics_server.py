# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import app.fire_analytics_server as fas
import json
import pytest

# PAS defaults, reset for each test
DEFAULT_TIME_DRIFT_LIMIT = 3.0

@pytest.fixture(scope='function', autouse=True)
def set_fas_context_to_default():
    fas.TIME_DRIFT_LIMIT_SECS = DEFAULT_TIME_DRIFT_LIMIT
    fas.seconds_offsets_by_channel = {}
    fas.LOOKBACK_FRAMES=1
    fas.frame_history_by_channel = {}



@pytest.fixture
async def triggers():
    class FakeRedis:
        async def hgetall(self, *args):
            return {}
    return await fas.get_triggers(FakeRedis())

async def test_get_triggers_returns_fake_trigger(triggers):
    assert len(triggers) == 1, "Expect one hard-coded trigger with faked Redis"

def make_message(type : str):
    MESSAGE_TIMESTAMP = "1234567890" # in nanoseconds; 1.234567890 in seconds
    frame_message = {
                'object_detection': [
                    {
                        'label': 'fire',
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

    if type == 'nothing':
        del(frame_message['object_detection']) # no object_detection object in case of no fire and smoke

    elif type == 'smoke':
        frame_message['object_detection'][0]['label'] = 'smoke'

    elif type == 'fire_with_extra_whitespace':
        frame_message['object_detection'][0]['object_detection'] = [{'label': '  fire  '}]

    elif type == 'fire':
        pass

    return frame_message


def make_message_list(messages, channel='0'):
    message_list = [
        # messages are tuples of (loop_timestamp, message)
        (0, # loop timestamp
            {
                'channel': 'detection.fire:' + channel,
                'data': json.dumps(message)
            }
        )
        for message in messages
    ]
    return message_list

async def test_one_fire_generates_one_alert(triggers):
    message_list = make_message_list(
        # Note: see "make_message" for the defaults which are checked below
        [make_message('fire')]
    )
    alerts =  fas.apply_triggers(triggers, message_list)
    assert len(alerts) == 1
    alert = alerts[0]
    assert alert['source_trigger'] == triggers[0], 'Expected the test trigger to be the source_trigger'
    # TODO: uncomment this if not compensating for system time
    # assert alert['time'] == 1.234567890, 'Expected the message timestamp converted from ns to sec'
    causes = alert['causes']
    assert len(causes) == 1, 'Should have one fire causing trigger'
    cause = causes[0]
    assert cause['event'] == 'fire'
    assert cause['violator']['top_left']['x'] == 0.1
    assert cause['violator']['top_left']['y'] == 0.2
    assert cause['violator']['bottom_right']['x'] == 0.3
    assert cause['violator']['bottom_right']['y'] == 0.4


async def test_one_message_all_events_present_generates_alerts(triggers):
    message_list = make_message_list(
        [make_message('fire')]
    )
    alerts =  fas.apply_triggers(triggers, message_list)
    assert len(alerts) != 0, f'Expected alerts because event present; alert {alerts[0]}'

async def test_missing_accessory_on_other_channel_generates_no_alerts(triggers):
    message_list = make_message_list(
        [make_message('fire')], channel='other'
    )
    alerts =  fas.apply_triggers(triggers, message_list)
    assert len(alerts) == 0, f'Expected no alerts because violation was on other channel; alert {alerts[0]}'


async def test_wrong_event_causes_alert(triggers):
    msg = make_message('smoke')
    msg['object_detection'][0]['label'] = 'fire'
    message_list = make_message_list([msg])
    alerts =  fas.apply_triggers(triggers, message_list)
    assert len(alerts) == 1, f'Expected alert because no smoke, only fire'


async def test_two_messages_only_looks_at_second_for_alert(triggers):
    message_list = make_message_list([
        make_message('no_fire'),
        make_message('smoke')
    ])
    alerts =  fas.apply_triggers(triggers, message_list)
    assert len(alerts) == 0, f'Expected only last message to be viewed and no alert, alert: {alerts[0]}'

async def test_can_register_events_from_redis(caplog):
    class FakeRedis:
        async def hgetall(self, *args):
            return {
                '12345': json.dumps(
                    {
                        "monitor_id": "0",
                        "trigger_id": "0xDEADBEEF",
                        "trigger_name": "fire",
                        "params": '[{"name":"accessories","value":"fire"}]'
                    }
                )
            }
    triggers = await fas.get_triggers(FakeRedis())
    assert len(caplog.records) == 0, 'Expect no errors'
    assert len(triggers) == 1


async def test_trigger_event_whitespace_is_stripped():
    class FakeRedis:
        async def hgetall(self, *args):
            return {
                '12345': json.dumps(
                    {
                        "monitor_id": "0",
                        "trigger_id": "0xDEADBEEF",
                        "trigger_name": "fire",
                        # "accessories" value has extra spaces the alert should strip
                        "params": '[{"name":"event","value":"    fire"}]'
                    }
                )
            }
    triggers = await fas.get_triggers(FakeRedis())
    message_list = make_message_list([
        make_message('fire'),
    ])
    alerts =  fas.apply_triggers(triggers, message_list)
    causes = alerts[0]['causes']
    assert causes[0]['event'] == 'fire', 'Expected spaces to be removed from event'


async def test_redis_message_accessory_whitespace_is_stripped():
    message_list = make_message_list([
        make_message('fire_with_extra_whitespace')
    ])
    alerts =  fas.apply_triggers(triggers, message_list)
    assert len(alerts) == 0, f'Expected only last message to be viewed and no alert, alert: {alerts[0]}'


async def test_malformed_message_logs_error_and_returns_no_alerts(triggers, caplog):
    message_list = make_message_list([
        { 'mlmeta': [ { 'name': 'this is not the schema we agreed to!!' } ] }
    ])
    try:
        alerts =  fas.apply_triggers(triggers, message_list)
    except:
        assert False, f'Exception not caught in apply_triggers; was a "raise(exc)" left in?'

    assert len(caplog.records) == 2, 'Expected one error log followed by an exception log'
    errlog = caplog.records[0]
    assert errlog.levelname == 'ERROR'
    assert 'not the schema we agreed to' in errlog.getMessage()

    assert len(alerts) == 0, f'Expected no alerts with a parsing error, alert: {alerts[0]}'


def test_no_alerts_if_lookback_buffer_if_room_in_lookback_buffer(triggers):
    fas.LOOKBACK_FRAMES=5
    message_list = make_message_list([make_message('fire')] * 4)
    alerts =  fas.apply_triggers(triggers, message_list)
    assert len(alerts) == 0, f'Expected no alerts if only 4 frames of violations: {alerts[0]}'


def test_multiple_frames_needed_for_trigger(triggers):
    fas.LOOKBACK_FRAMES=5
    message_list = make_message_list([make_message('fire')] * 5)
    alerts =  fas.apply_triggers(triggers, message_list)
    assert len(alerts) == 1, f'Should have one alert with 5 violation frames'

def test_message_on_different_channel_ignored(triggers, caplog):
    message_list = make_message_list([make_message('fire')], channel="not 0")
    alerts =  fas.apply_triggers(triggers, message_list)
    assert len(alerts) == 0, f'Should be no alerts because message was for different channel'
    assert len(caplog.records) == 0, 'Should be no logged errors'


# Time conversion tests

# TODO: when real time comes from camera, can remove the sys time override
def test_msg_and_system_time_same():
    msg_time = 111.123456789
    msg_ts = str(msg_time * 1e9) # timestamp a string of nanoseconds since 0-time
    sys_time = 987654321

    assert sys_time == fas.convert_msg_timestamp_to_epoch_time(
        msg_ts=msg_ts, sys_time=sys_time), 'Function just takes system time and ignores msg_ts'

# TODO: channels clean up stats if no messages received for some amount of time?
