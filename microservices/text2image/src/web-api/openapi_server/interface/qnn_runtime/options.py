# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from dataclasses import dataclass
from typing import List, Optional

@dataclass
class AppOptions:
    model_path:             Optional[str] = None       # path to model .so
    backend_path:           Optional[str] = None       # path to libQnnHtp.so
    input_list_paths:       Optional[str] = None       # path to input_list.txt
    output_dir:             str = "./output"
    op_packages:            Optional[str] = None
    debug:                  bool = False
    output_data_type:       str = "float_only"         # "float_only"|"native_only"|"float_and_native"
    input_data_type:        str = "float"              # "float"|"native"
    profiling_level:        str = "off"                # "off"|"basic"|"detailed"
    retrieve_contexts:      Optional[List[str]] = None # list of explicit model .bin file paths (set by caller)
    retrieve_context:       Optional[str] = None       # path to a single .bin (set internally per-manager by start())
    save_context:           Optional[str] = None       # filename stem to write context (.bin)
    log_level:              int = 2                    # 0..4
    system_library:         Optional[str] = None       # path to libQnnSystem.so (needed for context)
    num_inferences:         int = 1
    serialize_profile_logs: bool = False
