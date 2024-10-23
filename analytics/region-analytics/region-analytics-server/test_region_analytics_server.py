# Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear
import region_analytics_server as ras
import json
import pytest
import time
import logging

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
async def triggers():
    class FakeRedis:
        async def hgetall(self, *args):
            return {
                # From default loaded in the mariadb on the demo platform
                # Unsure why this is called "NO_ALERTS"
                "NO_ALERTS": "{\"region_id\":\"THE_REGION\",\"trigger_id\":\"NO_ALERTS\",\"trigger_name\":\"This is just an example\",\"trigger_condition\":\"occupancy_changed\"}"
            }
    return await ras.get_triggers(FakeRedis())

async def test_get_triggers_returns_fake_trigger(triggers):
    assert len(triggers) == 1, "Expect one hard-coded trigger with faked Redis"
    t = triggers[0]
    assert t['trigger_id'] == 'NO_ALERTS'
    assert t['region_id'] == 'THE_REGION'
    assert t['trigger_name'] == 'This is just an example'
    assert t['trigger_condition'] == 'occupancy_changed'


def make_message(type : str):
    redis_message_from_aug_19_build = "{\"ObjectDetection\":[{\"id\":2,\"label\":\"person_2\",\"confidence\":87.233505249023438,\"color\":4278190335,\"rectangle\":[0.26250000000000001,0.29999999999999999,0.09947916666666666,0.53240740740740744],\"landmarks\":{\"feet-middle-point\":{\"x\":0.41361256544502617,\"y\":0.84869565217391307}},\"ObjectDetection\":[{\"label\":\"safety vest\",\"confidence\":93.361564636230469,\"color\":16711935,\"rectangle\":[0.29375000000000001,0.37314814814814817,0.061979166666666669,0.16111111111111112]},{\"label\":\"helmet\",\"confidence\":93.097320556640625,\"color\":16711935,\"rectangle\":[0.31510416666666669,0.29999999999999999,0.04010416666666667,0.075925925925925924]}]},{\"label\":\"feet\",\"confidence\":100.0,\"color\":4278190335,\"rectangle\":[0.30156250000000001,0.74907407407407411,0.0041666666666666666,0.0055555555555555558]},{\"id\":3,\"label\":\"person_3\",\"confidence\":80.210472106933594,\"color\":4278190335,\"rectangle\":[0.34999999999999998,0.29999999999999999,0.086979166666666663,0.53240740740740744],\"landmarks\":{\"feet-middle-point\":{\"x\":0.32934131736526945,\"y\":0.84173913043478266}},\"ObjectDetection\":[{\"label\":\"safety vest\",\"confidence\":94.599288940429688,\"color\":16711935,\"rectangle\":[0.35052083333333334,0.38703703703703701,0.071354166666666663,0.16296296296296298]},{\"label\":\"helmet\",\"confidence\":91.813453674316406,\"color\":16711935,\"rectangle\":[0.36822916666666666,0.31296296296296294,0.03229166666666667,0.075925925925925924]}]},{\"label\":\"feet\",\"confidence\":100.0,\"color\":4278190335,\"rectangle\":[0.36197916666666669,0.73796296296296293,0.0041666666666666666,0.0055555555555555558]},{\"id\":4,\"label\":\"person_4\",\"confidence\":90.560211181640625,\"color\":16711935,\"rectangle\":[0.42499999999999999,0.36666666666666664,0.086979166666666663,0.44351851851851853],\"landmarks\":{\"feet-middle-point\":{\"x\":0.42514970059880242,\"y\":0.85803757828810023}},\"ObjectDetection\":[{\"label\":\"safety vest\",\"confidence\":94.571319580078125,\"color\":16711935,\"rectangle\":[0.43906250000000002,0.43333333333333335,0.066145833333333334,0.1648148148148148]}]},{\"label\":\"feet\",\"confidence\":100.0,\"color\":16711935,\"rectangle\":[0.45989583333333334,0.75277777777777777,0.0041666666666666666,0.0055555555555555558]},{\"id\":1,\"label\":\"person_1\",\"confidence\":93.14764404296875,\"color\":16711935,\"rectangle\":[0.50624999999999998,0.25555555555555554,0.09947916666666666,0.59907407407407409],\"landmarks\":{\"feet-middle-point\":{\"x\":0.43979057591623039,\"y\":0.87326120556414222}},\"ObjectDetection\":[{\"label\":\"safety vest\",\"confidence\":93.683395385742188,\"color\":16711935,\"rectangle\":[0.53281250000000002,0.37592592592592594,0.063541666666666663,0.16851851851851851]}]},{\"label\":\"feet\",\"confidence\":100.0,\"color\":16711935,\"rectangle\":[0.55989583333333337,0.77870370370370368,0.0041666666666666666,0.0055555555555555558]}],\"Parameters\":{\"timestamp\":\"4270933333\"}}"
    frame_message = json.loads(redis_message_from_aug_19_build)

    if type == 'default':
        return frame_message
    
    if type == 'person_2_leaves':
        # same string but person 2 is in the top-left corner {0.01, 0.01}
        return json.loads("{\"ObjectDetection\":[{\"id\":2,\"label\":\"person_2\",\"confidence\":87.233505249023438,\"color\":4278190335,\"rectangle\":[0.26250000000000001,0.29999999999999999,0.09947916666666666,0.53240740740740744],\"landmarks\":{\"feet-middle-point\":{\"x\":0.01,\"y\":0.01}},\"ObjectDetection\":[{\"label\":\"safety vest\",\"confidence\":93.361564636230469,\"color\":16711935,\"rectangle\":[0.29375000000000001,0.37314814814814817,0.061979166666666669,0.16111111111111112]},{\"label\":\"helmet\",\"confidence\":93.097320556640625,\"color\":16711935,\"rectangle\":[0.31510416666666669,0.29999999999999999,0.04010416666666667,0.075925925925925924]}]},{\"label\":\"feet\",\"confidence\":100.0,\"color\":4278190335,\"rectangle\":[0.30156250000000001,0.74907407407407411,0.0041666666666666666,0.0055555555555555558]},{\"id\":3,\"label\":\"person_3\",\"confidence\":80.210472106933594,\"color\":4278190335,\"rectangle\":[0.34999999999999998,0.29999999999999999,0.086979166666666663,0.53240740740740744],\"landmarks\":{\"feet-middle-point\":{\"x\":0.32934131736526945,\"y\":0.84173913043478266}},\"ObjectDetection\":[{\"label\":\"safety vest\",\"confidence\":94.599288940429688,\"color\":16711935,\"rectangle\":[0.35052083333333334,0.38703703703703701,0.071354166666666663,0.16296296296296298]},{\"label\":\"helmet\",\"confidence\":91.813453674316406,\"color\":16711935,\"rectangle\":[0.36822916666666666,0.31296296296296294,0.03229166666666667,0.075925925925925924]}]},{\"label\":\"feet\",\"confidence\":100.0,\"color\":4278190335,\"rectangle\":[0.36197916666666669,0.73796296296296293,0.0041666666666666666,0.0055555555555555558]},{\"id\":4,\"label\":\"person_4\",\"confidence\":90.560211181640625,\"color\":16711935,\"rectangle\":[0.42499999999999999,0.36666666666666664,0.086979166666666663,0.44351851851851853],\"landmarks\":{\"feet-middle-point\":{\"x\":0.42514970059880242,\"y\":0.85803757828810023}},\"ObjectDetection\":[{\"label\":\"safety vest\",\"confidence\":94.571319580078125,\"color\":16711935,\"rectangle\":[0.43906250000000002,0.43333333333333335,0.066145833333333334,0.1648148148148148]}]},{\"label\":\"feet\",\"confidence\":100.0,\"color\":16711935,\"rectangle\":[0.45989583333333334,0.75277777777777777,0.0041666666666666666,0.0055555555555555558]},{\"id\":1,\"label\":\"person_1\",\"confidence\":93.14764404296875,\"color\":16711935,\"rectangle\":[0.50624999999999998,0.25555555555555554,0.09947916666666666,0.59907407407407409],\"landmarks\":{\"feet-middle-point\":{\"x\":0.43979057591623039,\"y\":0.87326120556414222}},\"ObjectDetection\":[{\"label\":\"safety vest\",\"confidence\":93.683395385742188,\"color\":16711935,\"rectangle\":[0.53281250000000002,0.37592592592592594,0.063541666666666663,0.16851851851851851]}]},{\"label\":\"feet\",\"confidence\":100.0,\"color\":16711935,\"rectangle\":[0.55989583333333337,0.77870370370370368,0.0041666666666666666,0.0055555555555555558]}],\"Parameters\":{\"timestamp\":\"4270933333\"}}")
    
    if type == 'person_no_feet':
        del frame_message['ObjectDetection'][0]['landmarks'] # person 0 has no feet
        return frame_message

    '''
    if type == 'one_person_no_accessories':
        pass # all set up above

    elif type == 'nobody_and_nothing':
        frame_message['ObjectDetection'] = []

    elif type == 'only_vest_in_room':
        frame_message['ObjectDetection'][0]['label'] = 'vest'

    elif type == 'person_with_vest_in_frame_but_not_worn':
        # create new top-level object which is top level vest only
        frame_message['ObjectDetection'].append(
            {
                'label': 'vest' # top-level vest, e.g. placed on a chair
            }
        )
    
    elif type == 'one_person_with_vest':
        frame_message['ObjectDetection'][0]['ObjectDetection'] = [{'label': 'vest'}]
    '''
        
    return frame_message

def make_message_list(messages, channel='0'):
    message_list = [
        # messages are tuples of (system time, message)
        (0, # system time
            {
                # NOTE: aas of 2024-08-19, restricted zone is also going to PPE channel
                'channel': 'Detection::YoloV8::RZ::' + channel,
                'data': json.dumps(message)
            }
        )
        for message in messages
    ]
    return message_list

async def test_region_histories_populated(regions, triggers):
    messages = make_message_list([make_message('default') ] * 3)
    alerts = ras.apply_triggers(triggers, regions, messages)
    assert len(alerts) == 0, 'Require at least 4 frames for a person to become an occupant'
    assert len(ras.region_histories.items()) == 1, 'Expected one history created for this region'

async def test_number_of_people_in_region_for_default(regions, triggers):
    MONITOR_ID = '0' # must match value embedded in region config above
    messages = make_message_list([make_message('default') ] * 4, channel=MONITOR_ID)
    alerts = ras.apply_triggers(triggers, regions, messages)
    assert len(alerts) == 1, 'Expected occupancy change alert'
    alert = alerts[0]
    assert len(alert['occupants']) == 4
    assert alert['monitor_id'] == MONITOR_ID
    assert 'time' in alert
    trigger = alert['source_trigger']
    region_id = list(regions.values())[0]['region_id']
    assert trigger['trigger_name'] == 'This is just an example'
    assert trigger['region_id'] == region_id, 'Expected trigger and region to be linked by id'

async def test_only_3_people_in_region(regions, triggers):
    messages = make_message_list([make_message('person_2_leaves') ] * 4)
    alerts = ras.apply_triggers(triggers, regions, messages)
    assert len(alerts) == 1, 'Expected occupancy change alert'
    alert = alerts[0]
    assert len(alert['occupants']) == 3, 'Person 2 should have left, only 3 should be in region'

async def test_person_2_leaving_after_a_bit(regions, triggers):
    messages = make_message_list([make_message('default') ] * 4) # 4 frames of everybody
    alerts = ras.apply_triggers(triggers, regions, messages)
    assert len(alerts[0]['occupants']) == 4, 'All 4 people should be there'

    messages = make_message_list([make_message('person_2_leaves') ] * 4) # 4 frames of only 3
    alerts = ras.apply_triggers(triggers, regions, messages)
    assert len(alerts) == 0, 'Should still be all 4 people; takes 5 with a person missing for them to drop'

    messages = make_message_list([make_message('person_2_leaves')])
    alerts = ras.apply_triggers(triggers, regions, messages)
    assert len(alerts) == 1, 'Should get alert for person leaving'
    assert len(alerts[0]['occupants']) == 3, 'Should only have 3 people occupying region'

async def test_footless_person(regions, triggers):
    messages = make_message_list([make_message('person_no_feet')]) # If there's a problem it will crash immediately
    alerts = ras.apply_triggers(triggers, regions, messages)
    assert len(alerts) == 0 # shouldn't be an alert





# TODO: delete stale regions?
