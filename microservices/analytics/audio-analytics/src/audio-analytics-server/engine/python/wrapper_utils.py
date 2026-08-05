# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import os
import json
import errno


def evict_lru_cache(cache_dir: str, required_bytes: int, exclude_path: str = "") -> bool:
    """Delete the least-recently-used .qnn file in cache_dir to free space.

    Called when a cache write fails with ENOSPC.  Removes the single oldest
    .qnn file (by mtime) that is not the file currently being written, then
    returns True so the caller can retry the write.  Returns False if no
    evictable file was found.

    Args:
        cache_dir:     Directory to scan for .qnn files.
        required_bytes: How many bytes we need to free (logged only).
        exclude_path:  Absolute path of the file currently being written
                       (never evicted).

    Returns:
        True if a file was evicted, False otherwise.
    """
    try:
        candidates = []
        for fname in os.listdir(cache_dir):
            if not fname.endswith('.qnn'):
                continue
            fpath = os.path.join(cache_dir, fname)
            if os.path.abspath(fpath) == os.path.abspath(exclude_path):
                continue
            try:
                candidates.append((os.path.getmtime(fpath), fpath))
            except OSError:
                pass

        if not candidates:
            print(f"[cache_evict] No evictable .qnn files in {cache_dir} "
                  f"(need {required_bytes // 1024 // 1024} MB)")
            return False

        candidates.sort()  # oldest mtime first
        oldest_path = candidates[0][1]
        os.remove(oldest_path)
        print(f"[cache_evict] Evicted LRU cache file: {oldest_path} "
              f"(need {required_bytes // 1024 // 1024} MB)")
        return True

    except Exception as e:
        print(f"[cache_evict] Error during eviction: {e}")
        return False

def search_file(target_file, search_path):
    if not os.path.isdir(search_path):
        print(f"Invalid directory: {search_path}")

    for entry in os.listdir(search_path):
        if entry == target_file:
            full_path = os.path.join(search_path, target_file)
            if os.path.isfile(full_path):
                return full_path
    return None

def open_file(file_path):
    if not os.path.isfile(file_path):
        print("Invalid file path:", file_path)
        return

    with open(file_path, "r") as file:
        return json.loads(file.read())

def match_files_to_assets(config_json, model_dir_path):
    model_dict = dict()
    for file, file_name in config_json["assets"].items():
        # Skip None values (e.g., g2p_encoder: null for Chinese models)
        if file_name is None:
            model_dict[file] = None
            continue
        full_path = os.path.join(model_dir_path, file_name)
        model_dict[file] = full_path if os.path.isfile(full_path) else None
    return model_dict

def remove_file_from_folder(file_path):
    try:
        if not os.path.isfile(file_path):
            print(f"Error: File {file_path} not found")
            return

        os.remove(file_path)
        print(f"File '{file_path}' has been removed successfully.")
    except PermissionError:
        print(f"Error: Permission denied while deleting '{file}'.")
    except OSError as e:
        print(f"Error: Could not delete file. {e}")

def check_config(config_file,dir):
    config_path = os.path.join(dir,config_file)
    if not os.path.isfile(config_path):
        print("config file not exist")
        return False
    return True

def get_config(config_file,dir):
    config_path = os.path.join(dir,config_file)
    with open(config_path, "r") as file:
        return json.loads(file.read())
    

def check_assests(config_json,expect_model_files,model_dir):
    model_dict = {}
    for file,file_name in config_json["assets"].items():
        if file not in expect_model_files or file_name != expect_model_files[file]:
            return {}
        #get file path
        file_path = os.path.join(model_dir,file_name)
        if not os.path.isfile(file_path):
            print(f"{file_name} not exist.")
        model_dict[file] = file_path
    return model_dict

def check_t2t_runtime(config_json,input_lang_code):
    if "runtime" not in config_json:
        print("Config file does not contain runtime")
        return False
    runtime = config_json["runtime"]
    # # Check QNN version
    # if runtime["qnn_version"].get("major") != 2 or runtime["qnn_version"].get("minor") != 33:
    #     print(f"Unsupported QNN version: {runtime["qnn_version"].get('qnn_version_major', 'N/A')}.{runtime["qnn_version"].get('qnn_version_minor', 'N/A')}")
    #     print("Required QNN version: 2.33")
    #     return False
    
    # # Check QNN patch version
    # if runtime.get("patch") != 0:
    #     print(f"Warning: QNN patch version is {runtime.get('qnn_version_patch')}, expected 0")
    
    # Check architecture
    if runtime.get("arch") != 64:
        print(f"Unsupported architecture: {runtime.get('arch')}-bit")
        print("Required architecture: 64-bit")
        return False
    
    # Check encoder model max sequence length
    if runtime.get("enc_model_max_seq_len") != 256:
        print(f"Invalid encoder max sequence length: {runtime.get('enc_model_max_seq_len')}")
        print("Required encoder max sequence length: 256")
        return False
    
    # Check decoder model max sequence length
    if runtime.get("dec_model_max_seq_len") != 256:
        print(f"Invalid decoder max sequence length: {runtime.get('dec_model_max_seq_len')}")
        print("Required decoder max sequence length: 256")
        return False
    
    # Check repetition penalty
    if runtime.get("rep_penalty") != 1.2:
        print(f"Invalid repetition penalty: {runtime.get('rep_penalty')}")
        print("Required repetition penalty: 1.2")
        return False
    
    # Check scratch memory size requirement
    if runtime.get("scratch_mem_size_req") != 3200000:
        print(f"Invalid scratch memory size: {runtime.get('scratch_mem_size_req')}")
        print("Required scratch memory size: 3200000 bytes")
        return False
    
    #check lang code 
    model_lang = runtime.get("model_lang")
    if model_lang!= input_lang_code:
        print(f"Language mismatch: input language '{input_lang_code}' does not match model language '{model_lang}'")
        return False
    return True
