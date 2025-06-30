# Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear
from app import region_analytics_server as ras
import json
import pytest

# RAS defaults, reset for each test
DEFAULT_TIME_DRIFT_LIMIT = 3.0

@pytest.fixture(scope='function', autouse=True)
def set_pas_context_to_default():
    ras.TIME_DRIFT_LIMIT_SECS = DEFAULT_TIME_DRIFT_LIMIT
    ras.seconds_offsets_by_channel = {}
    ras.region_histories = {}
    ras.last_trigger_occupancy = {}



@pytest.fixture
async def regions():
    class FakeRedis:
        async def hgetall(self, *args):
            return {
                # From default loaded in the mariadb on the demo platform
                "THE_REGION": "{\"monitor_id\":\"0\",\"region_id\":\"THE_REGION\",\"region_name\":\"Triangle covering bottom-right diagonal half of monitor\",\"coordinates\":\"[{\\\"x\\\": 1,\\\"y\\\": 1},{\\\"x\\\": 1,\\\"y\\\": 0},{\\\"x\\\": 0,\\\"y\\\": 1}]\"}"
            }
    return await ras.get_regions(FakeRedis())

async def test_get_regions_returns_fake_trigger(regions):
    assert len(regions) == 1, "Expect one hard-coded region with faked Redis"
    r = regions['THE_REGION']
    assert r['monitor_id'] == '0'
    assert r['region_name'] == 'Triangle covering bottom-right diagonal half of monitor'
    contour = r['coordinates']
    assert len(contour) == 3, 'Expected 3 points for triangle'
    assert contour[0]['x'] == 1 and contour[0]['y'] == 1, 'First point should be bottom-right'
    assert contour[1]['x'] == 1 and contour[1]['y'] == 0, 'Second point should be top-right'
    assert contour[2]['x'] == 0 and contour[2]['y'] == 1, 'Last point should be bottom-left'


@pytest.fixture
async def triggers_occ_changed():
    class FakeRedis:
        async def hgetall(self, *args):
            return {
                # From default loaded in the mariadb on the demo platform
                # Unsure why this is called "NO_ALERTS"
                "NO_ALERTS": "{\"region_id\":\"THE_REGION\",\"trigger_id\":\"NO_ALERTS\",\"trigger_name\":\"This is just an example\",\"trigger_condition\":\"occupancy_changed\"}"
            }
    return await ras.get_triggers(FakeRedis())

async def test_get_triggers_returns_fake_trigger(triggers_occ_changed):
    assert len(triggers_occ_changed) == 1, "Expect one hard-coded trigger with faked Redis"
    t = triggers_occ_changed[0]
    assert t['trigger_id'] == 'NO_ALERTS'
    assert t['region_id'] == 'THE_REGION'
    assert t['trigger_name'] == 'This is just an example'
    assert t['trigger_condition'] == 'occupancy_changed'


def make_message(type : str):
    redis_message_from_aug_19_build = '''
        {\"object_detection\":[
            {\"tracking_id\":2,\"label\":\"person_2\",\"confidence\":87.233505249023438,\"color\":4278190335,
                \"rectangle\":[0.26250000000000001,0.29999999999999999,0.09947916666666666,0.53240740740740744],
                \"landmarks\":{
                    \"feet-middle-point\":{\"x\":0.41361256544502617,\"y\":0.84869565217391307}},
                    \"object_detection\":[{\"label\":\"safety vest\",\"confidence\":93.361564636230469,\"color\":16711935,\"rectangle\":[0.29375000000000001,0.37314814814814817,0.061979166666666669,0.16111111111111112]},{\"label\":\"helmet\",\"confidence\":93.097320556640625,\"color\":16711935,\"rectangle\":[0.31510416666666669,0.29999999999999999,0.04010416666666667,0.075925925925925924]}]},{\"label\":\"feet\",\"confidence\":100.0,\"color\":4278190335,\"rectangle\":[0.30156250000000001,0.74907407407407411,0.0041666666666666666,0.0055555555555555558]},{\"tracking_id\":3,\"label\":\"person_3\",\"confidence\":80.210472106933594,\"color\":4278190335,\"rectangle\":[0.34999999999999998,0.29999999999999999,0.086979166666666663,0.53240740740740744],\"landmarks\":{\"feet-middle-point\":{\"x\":0.32934131736526945,\"y\":0.84173913043478266}},\"object_detection\":[{\"label\":\"safety vest\",\"confidence\":94.599288940429688,\"color\":16711935,\"rectangle\":[0.35052083333333334,0.38703703703703701,0.071354166666666663,0.16296296296296298]},{\"label\":\"helmet\",\"confidence\":91.813453674316406,\"color\":16711935,\"rectangle\":[0.36822916666666666,0.31296296296296294,0.03229166666666667,0.075925925925925924]}]},{\"label\":\"feet\",\"confidence\":100.0,\"color\":4278190335,\"rectangle\":[0.36197916666666669,0.73796296296296293,0.0041666666666666666,0.0055555555555555558]},{\"tracking_id\":4,\"label\":\"person_4\",\"confidence\":90.560211181640625,\"color\":16711935,\"rectangle\":[0.42499999999999999,0.36666666666666664,0.086979166666666663,0.44351851851851853],\"landmarks\":{\"feet-middle-point\":{\"x\":0.42514970059880242,\"y\":0.85803757828810023}},\"object_detection\":[{\"label\":\"safety vest\",\"confidence\":94.571319580078125,\"color\":16711935,\"rectangle\":[0.43906250000000002,0.43333333333333335,0.066145833333333334,0.1648148148148148]}]},{\"label\":\"feet\",\"confidence\":100.0,\"color\":16711935,\"rectangle\":[0.45989583333333334,0.75277777777777777,0.0041666666666666666,0.0055555555555555558]},{\"tracking_id\":1,\"label\":\"person_1\",\"confidence\":93.14764404296875,\"color\":16711935,\"rectangle\":[0.50624999999999998,0.25555555555555554,0.09947916666666666,0.59907407407407409],\"landmarks\":{\"feet-middle-point\":{\"x\":0.43979057591623039,\"y\":0.87326120556414222}},\"object_detection\":[{\"label\":\"safety vest\",\"confidence\":93.683395385742188,\"color\":16711935,\"rectangle\":[0.53281250000000002,0.37592592592592594,0.063541666666666663,0.16851851851851851]}]},{\"label\":\"feet\",\"confidence\":100.0,\"color\":16711935,\"rectangle\":[0.55989583333333337,0.77870370370370368,0.0041666666666666666,0.0055555555555555558]}],\"Parameters\":{\"timestamp\":\"4270933333\"}}
        '''
    # NOTE: changed tracking_id on first person to '2' to match above, and duplicated
    # both people since tests expect 4 people.
    redis_message_from_sep_30_build = '''
        {
        \"object_detection\":[
            {
                \"tracking_id\":2,
                \"label\":\"person\",
                \"confidence\":93.14764404296875,
                \"color\":4294902015,
                \"rectangle\":{\"x\":0.41354166666666664,\"y\":0.30092592592592593,\"width\":0.10885416666666667,\"height\":0.59629629629629632},
                \"landmarks\":
                {
                    \"nose\":{\"x\":0.46302083333333333,\"y\":0.36851851851851852},
                    \"left_ankle\":{\"x\":0.47968749999999999,\"y\":0.83888888888888891},
                    \"right_ankle\":{\"x\":0.44635416666666666,\"y\":0.81111111111111112}
                }
        },
        {
            \"tracking_id\":13,
            \"label\":\"person\",
            \"confidence\":94.256546020507812,
            \"color\":4294902015,
            \"rectangle\":{\"x\":0.31197916666666664,\"y\":0.23333333333333334,\"width\":0.11197916666666667,\"height\":0.6425925925925926},
            \"landmarks\":
            {
                \"nose\":{\"x\":0.37656250000000002,\"y\":0.29444444444444445},
                \"left_ankle\":{\"x\":0.36302083333333335,\"y\":0.82222222222222219},
                \"right_ankle\":{\"x\":0.34895833333333331,\"y\":0.7944444444444444}
            }
        },
        {
            \"tracking_id\":14,
            \"label\":\"person\",
            \"confidence\":94.256546020507812,
            \"color\":4294902015,
            \"rectangle\":{\"x\":0.31197916666666664,\"y\":0.23333333333333334,\"width\":0.11197916666666667,\"height\":0.6425925925925926},
            \"landmarks\":
            {
                \"nose\":{\"x\":0.37656250000000002,\"y\":0.29444444444444445},
                \"left_ankle\":{\"x\":0.36302083333333335,\"y\":0.82222222222222219},
                \"right_ankle\":{\"x\":0.34895833333333331,\"y\":0.7944444444444444}
            }
        },
        {
            \"tracking_id\":15,
            \"label\":\"person\",
            \"confidence\":94.256546020507812,
            \"color\":4294902015,
            \"rectangle\":{\"x\":0.31197916666666664,\"y\":0.23333333333333334,\"width\":0.11197916666666667,\"height\":0.6425925925925926},
            \"landmarks\":
            {
                \"nose\":{\"x\":0.37656250000000002,\"y\":0.29444444444444445},
                \"left_ankle\":{\"x\":0.36302083333333335,\"y\":0.82222222222222219},
                \"right_ankle\":{\"x\":0.34895833333333331,\"y\":0.7944444444444444}
            }
        }
        ],
        \"parameters\":{\"timestamp\":\"8375033333\"}
        }
        '''
    # frame_message = json.loads(redis_message_from_aug_19_build)
    frame_message = json.loads(redis_message_from_sep_30_build)

    if type == 'default':
        return frame_message
    
    if type == 'empty':
        return json.loads("{\"parameters\":{\"timestamp\":\"433766666\"}}")
    
    if type == 'person_2_outside':
        # same string but person 2 is in the top-left corner {0.01, 0.01}
        redis_message_from_sep_30_build_with_person_2_moved = '''
            {
            \"object_detection\":[
                {
                    \"tracking_id\":2,
                    \"label\":\"person\",
                    \"confidence\":93.14764404296875,
                    \"color\":4294902015,
                    \"rectangle\":{\"x\":0.41354166666666664,\"y\":0.30092592592592593,\"width\":0.10885416666666667,\"height\":0.59629629629629632},
                    \"landmarks\":
                    {
                        \"nose\":{\"x\":0.46302083333333333,\"y\":0.36851851851851852},
                        \"left_ankle\":{\"x\":0.01,\"y\":0.01},
                        \"right_ankle\":{\"x\":0.01001,\"y\":0.01}
                    }
            },
            {
                \"tracking_id\":13,
                \"label\":\"person\",
                \"confidence\":94.256546020507812,
                \"color\":4294902015,
                \"rectangle\":{\"x\":0.31197916666666664,\"y\":0.23333333333333334,\"width\":0.11197916666666667,\"height\":0.6425925925925926},
                \"landmarks\":
                {
                    \"nose\":{\"x\":0.37656250000000002,\"y\":0.29444444444444445},
                    \"left_ankle\":{\"x\":0.36302083333333335,\"y\":0.82222222222222219},
                    \"right_ankle\":{\"x\":0.34895833333333331,\"y\":0.7944444444444444}
                }
            },
            {
                \"tracking_id\":14,
                \"label\":\"person\",
                \"confidence\":94.256546020507812,
                \"color\":4294902015,
                \"rectangle\":{\"x\":0.31197916666666664,\"y\":0.23333333333333334,\"width\":0.11197916666666667,\"height\":0.6425925925925926},
                \"landmarks\":
                {
                    \"nose\":{\"x\":0.37656250000000002,\"y\":0.29444444444444445},
                    \"left_ankle\":{\"x\":0.36302083333333335,\"y\":0.82222222222222219},
                    \"right_ankle\":{\"x\":0.34895833333333331,\"y\":0.7944444444444444}
                }
            },
            {
                \"tracking_id\":15,
                \"label\":\"person\",
                \"confidence\":94.256546020507812,
                \"color\":4294902015,
                \"rectangle\":{\"x\":0.31197916666666664,\"y\":0.23333333333333334,\"width\":0.11197916666666667,\"height\":0.6425925925925926},
                \"landmarks\":
                {
                    \"nose\":{\"x\":0.37656250000000002,\"y\":0.29444444444444445},
                    \"left_ankle\":{\"x\":0.36302083333333335,\"y\":0.82222222222222219},
                    \"right_ankle\":{\"x\":0.34895833333333331,\"y\":0.7944444444444444}
                }
            }
            ],
            \"parameters\":{\"timestamp\":\"8375033333\"}
            }
            '''
            
        return json.loads(redis_message_from_sep_30_build_with_person_2_moved)
    
    if type == 'person_no_feet':
        del frame_message['object_detection'][0]['landmarks'] # person 0 has no feet
        return frame_message

    raise RuntimeError('Should not have gotten here')

def make_message_list(messages, channel='0'):
    message_list = [
        # messages are tuples of (system time, message)
        (0, # system time
            {
                'channel': 'detection.rz:' + channel,
                'data': json.dumps(message)
            }
        )
        for message in messages
    ]
    return message_list

async def test_region_histories_populated(regions, triggers_occ_changed):
    messages = make_message_list([make_message('default') ] * 3)
    alerts = ras.apply_triggers(triggers_occ_changed, regions, messages)
    assert len(alerts) == 0, 'Require at least 4 frames for a person to become an occupant'
    assert len(ras.region_histories.items()) == 1, 'Expected one history created for this region'

async def test_number_of_people_in_region_for_default(regions, triggers_occ_changed):
    MONITOR_ID = '0' # must match value embedded in region config above
    messages = make_message_list([make_message('default') ] * 4, channel=MONITOR_ID)
    alerts = ras.apply_triggers(triggers_occ_changed, regions, messages)
    assert len(alerts) == 1, 'Expected occupancy change alert'
    alert = alerts[0]
    assert len(alert['occupants']) == 4
    assert alert['monitor_id'] == MONITOR_ID
    assert 'time' in alert
    trigger = alert['source_trigger']
    region_id = list(regions.values())[0]['region_id']
    assert trigger['trigger_name'] == 'This is just an example'
    assert trigger['region_id'] == region_id, 'Expected trigger and region to be linked by id'


async def test_occupants_schema(regions, triggers_occ_changed):
    '''
    From API spec for Region Analytics:
    "occupants": [
      {
        "top_left": {
          "x": 0.2,
          "y": 0.3
        },
        "bottom_right": {
          "x": 0.4,
          "y": 0.5
        }
      }
    ]
    '''
    messages = make_message_list([make_message('default') ] * 4)
    alerts = ras.apply_triggers(triggers_occ_changed, regions, messages)
    assert len(alerts) == 1, 'Expected one occupancy change alert'
    occupants = alerts[0]['occupants']
    assert len(occupants) == 4, 'Expected four occupants'
    for occupant in occupants:
        for corner in ['top_left', 'bottom_right']:
            for point in ['x', 'y']:
                val = occupant[corner][point]
                assert type(val) == float and val >= 0.0 and val <= 1.0, (
                    f'Problem validating {corner}[\'{point}\'] in occupant {occupant}')


async def test_only_3_people_in_region_after_person_2_outside(regions, triggers_occ_changed):
    messages = make_message_list([make_message('default') ] * 4)
    alerts = ras.apply_triggers(triggers_occ_changed, regions, messages)
    assert len(alerts) == 1, 'Expected occupancy change'
    assert len(alerts[0]['occupants']) == 4, 'Expected four occupants after 4 frames'

    messages = make_message_list([make_message('person_2_outside') ] * 4)
    alerts = ras.apply_triggers(triggers_occ_changed, regions, messages)
    assert len(alerts) == 0, 'Should not get an alert until 5 frames out of region'

    messages = make_message_list([make_message('person_2_outside') ] * 1)
    alerts = ras.apply_triggers(triggers_occ_changed, regions, messages)
    assert len(alerts) == 1, 'Expected occupancy change alert on 5th frame out-of-region'
    alert = alerts[0]
    assert len(alert['occupants']) == 3, 'Person 2 should have left, only 3 people should be in region'


async def test_occupants_schema(regions, triggers_occ_changed):
    '''
    From API spec for Region Analytics:
    "occupants": [
      {
        "top_left": {
          "x": 0.2,
          "y": 0.3
        },
        "bottom_right": {
          "x": 0.4,
          "y": 0.5
        }
      }
    ]
    '''
    messages = make_message_list([make_message('default') ] * 4)
    alerts = ras.apply_triggers(triggers_occ_changed, regions, messages)
    assert len(alerts) == 1, 'Expected one occupancy change alert'
    occupants = alerts[0]['occupants']
    assert len(occupants) == 4, 'Expected four occupants'
    for occupant in occupants:
        for corner in ['top_left', 'bottom_right']:
            for point in ['x', 'y']:
                # TODO: bounding_box should be removed, not part of API
                val = occupant['bounding_box'][corner][point]
                assert type(val) == float and val >= 0.0 and val <= 1.0, (
                    f'Problem validating {corner}[\'{point}\'] in occupant {occupant}')


async def test_only_3_people_in_region_after_person_2_outside(regions, triggers_occ_changed):
    messages = make_message_list([make_message('default') ] * 4)
    alerts = ras.apply_triggers(triggers_occ_changed, regions, messages)
    assert len(alerts) == 1, 'Expected occupancy change'
    assert len(alerts[0]['occupants']) == 4, 'Expected four occupants after 4 frames'

    messages = make_message_list([make_message('person_2_outside') ] * 4)
    alerts = ras.apply_triggers(triggers_occ_changed, regions, messages)
    assert len(alerts) == 0, 'Should not get an alert until 5 frames out of region'

    messages = make_message_list([make_message('person_2_outside') ] * 1)
    alerts = ras.apply_triggers(triggers_occ_changed, regions, messages)
    assert len(alerts) == 1, 'Expected occupancy change alert on 5th frame out-of-region'
    alert = alerts[0]
    assert len(alert['occupants']) == 3, 'Person 2 should have left, only 3 people should be in region'


async def test_person_2_leaving_after_a_bit(regions, triggers_occ_changed):
    messages = make_message_list([make_message('default') ] * 4) # 4 frames of everybody
    alerts = ras.apply_triggers(triggers_occ_changed, regions, messages)
    assert len(alerts[0]['occupants']) == 4, 'All 4 people should be there'

    messages = make_message_list([make_message('person_2_outside') ] * 4) # 4 frames of only 3
    alerts = ras.apply_triggers(triggers_occ_changed, regions, messages)
    assert len(alerts) == 0, 'Should still be all 4 people; takes 5 with a person missing for them to drop'

    messages = make_message_list([make_message('person_2_outside')])
    alerts = ras.apply_triggers(triggers_occ_changed, regions, messages)
    assert len(alerts) == 1, 'Should get alert for person leaving'
    assert len(alerts[0]['occupants']) == 3, 'Should only have 3 people occupying region'

async def test_footless_person(regions, triggers_occ_changed):
    messages = make_message_list([make_message('person_no_feet')]) # If there's a problem it will crash immediately
    alerts = ras.apply_triggers(triggers_occ_changed, regions, messages)
    assert len(alerts) == 0 # shouldn't be an alert


@pytest.fixture
async def triggers_occ_over():
    class FakeRedis:
        async def hgetall(self, *args):
            return {
                'over_3': json.dumps(
                    {
                        'region_id': 'THE_REGION',
                        'trigger_id': 'over_3',
                        'trigger_name': 'More than 3 people occupy THE_REGION',
                        'trigger_condition': 'occupancy_over',
                        'params': [
                            {
                                'name': 'threshold',
                                'value': 3
                            }
                        ]
                    }
                )
            }
    return await ras.get_triggers(FakeRedis())


async def test_occupancy_over_triggers_after_above_threshold(regions, triggers_occ_over):
    messages = make_message_list([make_message('person_2_outside')] * 4)
    alerts = ras.apply_triggers(triggers_occ_over, regions, messages)
    assert len(alerts) == 0, 'Alert should not trigger; exactly 3 occupants not over 3.'

    # send 3 frames one at a time -- expect no trigger as it takes 4 to "occupy"
    for count in range(3):
        messages = make_message_list([make_message('default')])
        alerts = ras.apply_triggers(triggers_occ_over, regions, messages)
        assert len(alerts) == 0, f'Unexpected alert at {count} in loop'

    messages = make_message_list([make_message('default')])
    alerts = ras.apply_triggers(triggers_occ_over, regions, messages)
    assert len(alerts) == 1, 'Alert should trigger now; 4 occupants for 4 frames'

    messages = make_message_list([make_message('default')])
    alerts = ras.apply_triggers(triggers_occ_over, regions, messages)
    assert len(alerts) == 1, 'Alert should keep triggering as there are >3 occupants'

    # Person 2 leaves, should be 3 occupants now, but it takes 5 frames to register
    for count in range(4):
        messages = make_message_list([make_message('default')])
        alerts = ras.apply_triggers(triggers_occ_over, regions, messages)
        assert len(alerts) == 1, f'Unexpected trigger at {count} in loop'
    

async def test_occupancy_over_stops_triggering_after_below_threshold(regions, triggers_occ_over):
    messages = make_message_list([make_message('default')] * 3) # default = 4 people
    alerts = ras.apply_triggers(triggers_occ_over, regions, messages)
    assert len(alerts) == 0, 'Alerts should not trigger as it takes 4 frames for occupancy to increase'

    messages = make_message_list([make_message('default')])
    alerts = ras.apply_triggers(triggers_occ_over, regions, messages)
    assert len(alerts) == 1, 'Alert should trigger now; 4 occupants for 4 frames'

    # Let person 2 leave; at/under occupancy, but takes 5 frames to register leaving
    for count in range(4):
        messages = make_message_list([make_message('person_2_outside')])
        alerts = ras.apply_triggers(triggers_occ_over, regions, messages)
        assert len(alerts) == 1, f'Unexpected trigger at {count} in loop'

    messages = make_message_list([make_message('person_2_outside')])
    alerts = ras.apply_triggers(triggers_occ_over, regions, messages)
    assert len(alerts) == 0, 'Should stop receiving alerts now that occupancy <= 3'


@pytest.fixture
async def triggers_occ_under():
    class FakeRedis:
        async def hgetall(self, *args):
            return {
                'over_3': json.dumps(
                    {
                        'region_id': 'THE_REGION',
                        'trigger_id': 'under_4',
                        'trigger_name': 'Fewer than 4 people occupy THE_REGION',
                        'trigger_condition': 'occupancy_under',
                        'params': [
                            {
                                'name': 'threshold',
                                'value': 4
                            }
                        ]
                    }
                )
            }
    return await ras.get_triggers(FakeRedis())


async def test_occupancy_under_triggers_immediately(regions, triggers_occ_under):
    messages = make_message_list([make_message('default')])
    alerts = ras.apply_triggers(triggers_occ_under, regions, messages)
    assert len(alerts) == 1, f'Expected immediate trigger as occupancies start at 0'
    alert = alerts[0]
    assert alert['source_trigger']['trigger_condition'] == 'occupancy_under'


async def test_occupancy_under_stops_after_occupancy_fills(regions, triggers_occ_under):
    # It takes 4 frames for occupants to be counted in a region
    for count in range(3):
        messages = make_message_list([make_message('default')])
        alerts = ras.apply_triggers(triggers_occ_under, regions, messages)
        assert len(alerts) == 1, f'Unexpected no-alerts at count {count}'

    messages = make_message_list([make_message('default')])
    alerts = ras.apply_triggers(triggers_occ_under, regions, messages)
    assert len(alerts) == 0, f'4 occupants for 4 frames, alerts should stop.'

async def test_empty_message(regions, triggers_occ_changed):
    messages = make_message_list([make_message('empty')])
    alerts = ras.apply_triggers(triggers_occ_changed, regions, messages)
    assert len(alerts) == 0, f'Expect no alert if nobody in frame' 

# TODO: tests for no landmarks
# TODO: tests for missing left ankle

# TODO: delete stale regions?
