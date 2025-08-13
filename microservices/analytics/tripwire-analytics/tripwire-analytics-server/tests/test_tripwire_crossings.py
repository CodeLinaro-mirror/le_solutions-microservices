# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear
import pytest
import json
from app.tripwire_crossings import Tripwire, calculate_tripwire_crossings
from pathlib import Path

@pytest.fixture
def tripwires_data_corridor():
    tripwires_content_fixed = '''
    [
        {
            "direction": [[0.5, 0.7], [0.5, 0.9]],
            "wire": [[0.1, 0.6], [0.4, 0.7], [0.7, 0.65], [0.9, 0.8]],
            "id": 1,
            "name": "Polygonal Wire 1"
        },
        {
            "direction": [[0.4, 0.8], [0.4, 0.6]],
            "wire": [[0.2, 0.8], [0.3, 0.7], [0.6, 0.75], [0.8, 0.65]],
            "id": 2,
            "name": "Polygonal Wire 2"
        }
    ]
    '''
    return json.loads(tripwires_content_fixed)

@pytest.fixture
def frame_metadata_corridor():
    with Path('data/simulated_inputs.json').open() as infile:
        return json.load(infile)


def run_tripwire_test(window_size, filter_size, raw_data, expected_results_file, tripwires_data):
    # Create Tripwire objects
    tripwires = [
        Tripwire(
            t.get("direction", []),
            t.get("wire", []),
            t.get("id", -1),
            t.get("name", "")
        )
        for t in tripwires_data
    ]

    step = window_size - 1 if window_size > 1 else 1
    results_overall = []

    # Slide over raw_data in windows
    # e.g., from i=0, i+=step while i+window_size <= len(raw_data)
    for i in range(0, len(raw_data) - window_size + 1, step):
        start_i = max(0, i - filter_size + 1)
        rd_window = raw_data[start_i : i + window_size]
        # Compute crossing for this window
        res = calculate_tripwire_crossings(
            rd_window,
            filter_size,
            window_size,
            tripwires
        )
        results_overall.append(res)

    ref_results = json.loads(Path(expected_results_file).read_text())
    jsonified_results = json.loads(json.dumps(results_overall))
    assert ref_results == jsonified_results


def test_reference_tripwire_from_algo_delivery(frame_metadata_corridor, tripwires_data_corridor):
    window_size=48
    filter_size=24
    raw_data=frame_metadata_corridor
    expected_results_file='data/output_filter24.json'
    
    run_tripwire_test(window_size, filter_size, raw_data, expected_results_file, tripwires_data_corridor)


def test_reference_tripwire_from_algo_delivery_filter10(frame_metadata_corridor, tripwires_data_corridor):
    window_size=48
    filter_size=10
    raw_data=frame_metadata_corridor
    expected_results_file='data/output_filter10.json'
    
    run_tripwire_test(window_size, filter_size, raw_data, expected_results_file, tripwires_data_corridor)


def test_reference_tripwire_from_algo_delivery_filter1(frame_metadata_corridor, tripwires_data_corridor):
    window_size=48
    filter_size=1
    raw_data=frame_metadata_corridor
    expected_results_file='data/output_filter1.json'
    
    run_tripwire_test(window_size, filter_size, raw_data, expected_results_file, tripwires_data_corridor)
