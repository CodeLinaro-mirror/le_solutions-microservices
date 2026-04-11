# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

import time
import sys
import struct
import gc
import math
import array
from argparse import ArgumentParser
from concurrent.futures import ThreadPoolExecutor

if sys.version_info.major == 3:
    from functools import reduce

ALIGN_NUM = 256

'''
align memory addresses to a desired boundary
'''
def align_n(x, n=8):
    assert type(x) == int, '  input not of type int in align_n'
    assert type(n) == int, '  align not of type int in align_n'
    if n == 0:
        y = x
    else:
        y = int(math.ceil(math.ceil(float(x) / float(n)) * float(n)))
    return y

def build_structure_tts(
    dict_size, 
    bert_model_org_size, 
    bert_model_pad_size, 
    melo_encoder_model_org_size,
    melo_encoder_model_pad_size, 
    melo_flow_model_org_size, 
    melo_flow_model_pad_size, 
    melo_decoder_model_org_size,
    melo_decoder_model_pad_size, 
    g2p_encoder_model_org_size, 
    g2p_encoder_model_pad_size, 
    g2p_decoder_model_org_size,
    g2p_decoder_model_pad_size, 
    bert_tokenizer_org_size, 
    bert_tokenizer_pad_size, 
    bert_normalizer_org_size,
    bert_normalizer_pad_size, 
    scratch_mem_size,
    # Additional parameters
    qnn_version_major,
    qnn_version_minor,
    qnn_version_patch,
    model_version_major,
    model_version_minor,
    model_lang,
    word_offset_list,
    word_list,
    phones_list,
    phones_offset_list,
    g2p_token_list,
    g2p_phones_list,
    g2p_vowel_list,
    phones_per_word_list,
    persistent_data_size,
    is_model_quantized=0,
    melo_sdp_model_org_size=None,
    melo_sdp_model_pad_size=None
):

    body = bytes()

    repeat_count_threshold = 10
    melo_max_tts_char_size = 1024
    melo_max_seq_len = 512
    melo_max_pcm_size =  40960      # (40 x 512 x 2 for 0.46s)
    bert_tokenizer_struct_size = 304
    bert_max_tokens = 400
    melo_max_token_len = 128
    bert_tokenstream_struct_size = 557060
    split_sentence_struct_size = 10224
    melo_speech_format_struct_size = 40
    header_size = 16
    dsp_kpps = 100000
    so_filename_size = 20
    melo_out_g_max_size = 256
    B = 1
    T_Y = 3 * melo_max_seq_len
    melo_out_z_max_size = 192 * T_Y
    melo_y_mask_len = B * T_Y
    melo_attn_squeezed_len = T_Y * melo_max_seq_len

    # Define backend SO filenames
    backend_file_name = "libQnnHtp.so"
    system_file_name = "libQnnSystem.so"

    if "en" == model_lang:
        g2p_lang_prefix = "<eng-us>: "
    elif "es" == model_lang:
        g2p_lang_prefix = "<spa>: "
    else:
        g2p_lang_prefix = ""

    # Make sure the strings are within the defined size
    backend_file_name = backend_file_name[:so_filename_size].ljust(so_filename_size, '\0')
    system_file_name = system_file_name[:so_filename_size].ljust(so_filename_size, '\0')

    g2p_lang_prefix = g2p_lang_prefix[:so_filename_size].ljust(so_filename_size, '\0')

    # uint32_t struct_size;         - size of melo_struct. to be added at the end of this function
    # uint32_t struct_w_pad_size;   - size of melo_struct + padding. to be added at the end of this function

    # uint32_t dict_size;
    body += struct.pack('I', dict_size)

    # uint32_t bert_model_org_size;
    # uint32_t bert_model_pad_size;
    body += struct.pack('II', bert_model_org_size, bert_model_pad_size)

    # uint32_t melo_encoder_model_org_size;
    # uint32_t melo_encoder_model_pad_size;
    body += struct.pack('II', melo_encoder_model_org_size, melo_encoder_model_pad_size)

    # uint32_t melo_flow_model_org_size;
    # uint32_t melo_flow_model_pad_size;
    body += struct.pack('II', melo_flow_model_org_size, melo_flow_model_pad_size)

    # uint32_t melo_decoder_model_org_size;
    # uint32_t melo_decoder_model_pad_size;
    body += struct.pack('II', melo_decoder_model_org_size, melo_decoder_model_pad_size)

    # uint32_t g2p_encoder_model_org_size;
    # uint32_t g2p_encoder_model_pad_size;
    # uint32_t g2p_decoder_model_org_size;
    # uint32_t g2p_decoder_model_pad_size;
    body += struct.pack('IIII', g2p_encoder_model_org_size, g2p_encoder_model_pad_size,
                        g2p_decoder_model_org_size, g2p_decoder_model_pad_size)

    # uint32_t bert_tokenizer_org_size;
    # uint32_t bert_tokenizer_pad_size;
    body += struct.pack('II', bert_tokenizer_org_size, bert_tokenizer_pad_size)

    # uint32_t bert_normalizer_org_size;
    # uint32_t bert_normalizer_pad_size;
    body += struct.pack('II', bert_normalizer_org_size, bert_normalizer_pad_size)

    # uint32_t qnn_version_major;
    body += struct.pack('i', qnn_version_major)

    # uint32_t qnn_version_minor;
    body += struct.pack('i', qnn_version_minor)

    # uint32_t qnn_version_patch;
    body += struct.pack('i', qnn_version_patch)

    # uint32_t  model_version_major;
    body += struct.pack('i', model_version_major)

    # uint32_t  model_version_minor;
    body += struct.pack('i', model_version_minor)

    # uint32_t bert_backend_type;
    body += struct.pack('I', 1)

    # uint32_t melo_encoder_backend_type;
    body += struct.pack('I', 1)

    # uint32_t melo_flow_backend_type;
    body += struct.pack('I', 1)

    # uint32_t melo_decoder_backend_type;
    body += struct.pack('I', 1)

    # uint32_t g2p_encoder_backend_type;
    # uint32_t g2p_decoder_backend_type;
    body += struct.pack('II', 1, 1)

    # uint32_t KPPS;
    body += struct.pack('i', dsp_kpps)

    # uint32_t htp_power_cfg_profilemode;
    body += struct.pack('I', 10)         # CUSTOM_PERF_PROFILE_MODE

    #========================================
    # char backend_file_name[so_filename_size];
    body += struct.pack(f'{so_filename_size}s', backend_file_name.encode('utf-8'))

    # char system_file_name[so_filename_size];
    body += struct.pack(f'{so_filename_size}s', system_file_name.encode('utf-8'))

    #========================================
    # uint32_t melo_current_phones_len;
    body += struct.pack('I', 0)

    # uint32_t melo_current_tones_len;
    body += struct.pack('I', 0)

    # uint32_t input_language_code;
    if "en" == model_lang:
        body += struct.pack('I', 0)
    elif "zh" == model_lang:
        body += struct.pack('I', 1)
    elif "es" == model_lang:
        body += struct.pack('I', 3)

    # uint32_t input_text_size;
    body += struct.pack('I', 0)

    # uint8_t input_text[MELO_MAX_TTS_CHAR_SIZE];
    body += _zero_bytes(melo_max_tts_char_size)

    # uint32_t melo_input_phones_array[MELO_MAX_SEQ_LEN];
    body += _zero_bytes(melo_max_seq_len * 4)

    # uint32_t melo_input_tones_array[MELO_MAX_SEQ_LEN]
    body += _zero_bytes(melo_max_seq_len * 4)

    # uint8_t output_pcm[MELO_MAX_PCM_SIZE];
    body += _zero_bytes(melo_max_pcm_size)

    # uint32_t output_pcm_size;
    body += struct.pack('I', 0)

    # uint32_t current_inference_sentence;
    body += struct.pack('I', 0)

    # bool final_flag;
    body += struct.pack('b', 0)

    # bool melo_decoder_final_flag;
    body += struct.pack('b', 1)

    # bool is_bert_enabled;
    if (bert_model_org_size > 0):
        body += struct.pack('b', 1)
    else:
        body += struct.pack('b', 0)

    # bool is_g2p_enabled;
    if ((g2p_encoder_model_org_size > 0) and (g2p_decoder_model_org_size > 0)):
        body += struct.pack('b', 1)
    else:
        body += struct.pack('b', 0)

    if is_model_quantized == 1:
        if melo_sdp_model_org_size == None:
            print('ERROR: melo_sdp_model_org_size is None. Pass valid sdp model size. Exiting...')
            exit(1)
        # uint32_t melo_sdp_model_org_size;
        body += struct.pack('I', melo_sdp_model_org_size)
    else:
        # int8_t reserved_0[4];
        body += _zero_bytes(4)

    #========================================
    # uint32_t sample_rate;
    body += struct.pack('I', 44100)

    # uint32_t bits_per_sample;
    body += struct.pack('I', 16)

    # uint32_t num_channels;
    body += struct.pack('I', 1)

    # uint32_t num_formats;
    body += struct.pack('I', 1)

    # uint32_t speech_format;
    body += struct.pack('I', 0)       # enum = LINEAR16

    if is_model_quantized == 1:
        if melo_sdp_model_pad_size == None:
            print('ERROR: melo_sdp_model_pad_size is None. Pass valid sdp model pad size. Exiting...')
            exit(1)
        # uint32_t melo_sdp_model_pad_size;
        body += struct.pack('I', melo_sdp_model_pad_size)
    else:
        # int8_t reserved_1[4];
        body += _zero_bytes(4)

    # melo_speech_format_pcm_t melo_speech_format_pcm;
    body += _zero_bytes(melo_speech_format_struct_size)

    #========================================
    # int64_t x_length;
    body += struct.pack('q', 0)

    # int64_t sid;
    if "zh" == model_lang:
        body += struct.pack('q', 1)
    else:
        body += struct.pack('q', 0)

    # float sdp_ratio;
    body += struct.pack('f', 0.2)

    # float noise_scale;
    body += struct.pack('f', 0.667)

    # float noise_scale_w;
    body += struct.pack('f', 0.8)

    # float length_scale;
    body += struct.pack('f', 1.0)

    # float melo_out_g[MELO_OUT_G_MAX_SIZE];
    body += _zero_bytes(melo_out_g_max_size * 4)

    # float melo_out_z[MELO_OUT_Z_MAX_SIZE * T_Y];
    body += _zero_bytes(melo_out_z_max_size * 4)

    # float x_mask_f[MELO_MAX_SEQ_LEN];
    body += _zero_bytes(melo_max_seq_len * 4)

    # float w_ceil_f[MELO_MAX_SEQ_LEN];
    body += _zero_bytes(melo_max_seq_len * 4)

    # float y_mask_f[B * T_Y];
    body += _zero_bytes(melo_y_mask_len * 4)

    # float attn_squeezed_f[T_Y * MELO_MAX_SEQ_LEN];
    body += _zero_bytes(melo_attn_squeezed_len * 4)

    # uint32_t y_lengths;
    # uint32_t total_dec_seq_len;
    body += struct.pack('II', 0, 0)

    #========================================
    # char* dict_all_words;  uint32_t* dict_word_offset;  uint32_t* dict_phones_offset;
    # uint8_t* phones_list_array;  uint8_t* tones_list_array;  uint8_t* phones_per_word_list_array;
    # int32_t* g2p_token_list_array;  int32_t* g2p_phones_list_array;  int32_t* g2p_vowel_list_array;
    body += _zero_bytes(9 * 8)  # 9 null pointers × 8 bytes each

    #========================================
    # uint32_t dict_num_words;
    body += struct.pack('I', len(word_offset_list))

    # uint32_t word_list_size;
    body += struct.pack('I', len(word_list))

    # uint32_t dict_num_phones;
    body += struct.pack('I', len(phones_list))

    # uint32_t dict_num_phones_offset;
    body += struct.pack('I', len(phones_offset_list))

    # uint32_t g2p_tokens;
    body += struct.pack('I', len(g2p_token_list))

    # uint32_t g2p_phones;
    body += struct.pack('I', len(g2p_phones_list))

    # uint32_t g2p_vowel;
    body += struct.pack('I', len(g2p_vowel_list))

    # uint32_t g2p_dict_offset;
    body += struct.pack('I', len(phones_per_word_list))

    # char g2p_lang_prefix[so_filename_size];
    body += struct.pack(f'{so_filename_size}s', g2p_lang_prefix.encode('utf-8'))

    # uint32_t repeat_count_threshold;
    body += struct.pack('I', repeat_count_threshold)   # To reduce hallucination

    #========================================
    # struct Tokenizer bert_tokenizer;
    body += _zero_bytes(bert_tokenizer_struct_size)

    # struct TokenStream preTokens;
    body += _zero_bytes(bert_tokenstream_struct_size)

    # struct TokenStream outTokens;
    body += _zero_bytes(bert_tokenstream_struct_size)

    # uint8_t word_count[MELO_BERT_MAX_TOKENS];
    body += _zero_bytes(bert_max_tokens)

    # uint8_t phones_per_word[MELO_BERT_MAX_TOKENS];
    body += _zero_bytes(bert_max_tokens)

    # uint32_t word2ph[MELO_BERT_MAX_TOKENS];
    body += _zero_bytes(bert_max_tokens * 4)

    # uint32_t bert_distribute_idx;
    body += struct.pack('I', 0)

    # char bert_word_groups[MELO_BERT_MAX_TOKENS * MAX_TOKEN_LEN];
    body += _zero_bytes(bert_max_tokens * melo_max_token_len)

    # uint32_t bert_offset_list[MELO_BERT_MAX_TOKENS];
    body += _zero_bytes(bert_max_tokens * 4)

    if is_model_quantized == 1:
        # uint32_t melo_sdp_backend_type;
        body += struct.pack('I', 1)
    else:
        # int8_t reserved_2[4];
        body += _zero_bytes(4)

    # uint8_t* bert_tokenizer_ptr;
    body += _zero_bytes(8)

    # uint8_t* bert_normalizer_ptr;
    body += _zero_bytes(8)

    #========================================
    # split_sentence split_sentence;
    body += _zero_bytes(split_sentence_struct_size)

    # uint64_t persistent_data_size;
    body += struct.pack('Q', persistent_data_size)

    # uint8_t* persistent_data;  (pointer placeholder, initialized at runtime)
    body += struct.pack('b' * 8, *[0] * 8)

    # int8_t reserved_3[8];
    body += struct.pack('b' * 8, *[0] * 8)

    # int8_t reserved_3[4]/reserved_1[4];
    # body += _zero_bytes(4)

    #========================================
    # melo_config_t  melo_config;
    body += _zero_bytes(8 * 4)

    #========================================
    # uint64_t scratch_mem_size;
    body += struct.pack('Q', scratch_mem_size)

    # uint64_t scratch_mem_offset;
    body += struct.pack('Q', 0)

    # uint8_t* scratch_mem_ptr;
    body += _zero_bytes(8)

    #========================================
    # uint32_t struct_size;         - size of melo_struct
    # uint32_t struct_w_pad_size;   - size of melo_struct + padding

    struct_size = len(body) + 8     # extra 8 bytes accounting for struct_size & struct_w_pad_size
    struct_w_pad_size = align_n(struct_size + header_size, ALIGN_NUM) - header_size # (tts structure + padding + header) needs to be 256 byte aligned
    pad_size = struct_w_pad_size - struct_size

    # padding size after struct for alignment
    body += _zero_bytes(pad_size)

    # Add struct_size and struct_w_pad_size before body
    body = struct.pack('II', struct_size, struct_w_pad_size) + body

    print('STRUCTURE ORG SIZE = ', struct_size)
    print('STRUCTURE PAD SIZE = ', pad_size)
    print('STRUCTURE WITH PAD SIZE = ', struct_w_pad_size)

    return body


def pack_model_body(start_magic, end_magic, version_major, version_minor, model_body):
    assert(len(start_magic)==4 and len(end_magic)==8)

    start_magic = bytes(start_magic.encode('ascii'))
    end_magic = bytes(end_magic.encode('ascii'))

    model_header = bytes()
    model_footer = bytes()

    # footer
    type_char = 'c' if sys.version_info.major == 2 else 'b'

    model_footer += struct.pack(type_char * 8, *end_magic)

    # header
    model_header += struct.pack(type_char * 4, *start_magic)
    model_header += struct.pack('hh', version_major, version_minor)
    size = len(model_header) + 8 + len(model_body) + len(model_footer)
    # To keep 8byte align, added 4byte zero.
    model_header += struct.pack('II', size, 0)

    return model_header+model_body+model_footer


def write_model(output_file, packed_model_body):
    with open(output_file, 'wb') as f:
        f.write(packed_model_body)

def _zero_bytes(n):
    """Return n zero bytes (single C memset, no Python loop)."""
    return bytes(n)

def _read_file(path):
    """Read a binary file and return its contents, or b'' if path is None."""
    if path is None:
        return b''
    with open(path, 'rb') as f:
        return f.read()


def _pad_to_align(data, align=ALIGN_NUM):
    """Return (padded_data, org_size, pad_size) aligned to boundary."""
    org_size = len(data)
    pad_size = align_n(org_size, align) - org_size
    return (data + _zero_bytes(pad_size) if pad_size else data, org_size, pad_size)


def allocate_mem(mem_size_req):
    extra_mem_size_factor = 0.1
    mem_size = align_n(round(mem_size_req * (1 + extra_mem_size_factor)), ALIGN_NUM)
    return (_zero_bytes(mem_size), mem_size)

def generate_model(bert_model,
                   bert_tokenizer,
                   bert_normalizer,
                   melo_encoder_model,
                   melo_sdp_model,
                   melo_flow_model,
                   melo_decoder_model,
                   g2p_enc_model,
                   g2p_dec_model,
                   model_version_major,
                   model_version_minor,
                   qnn_version_major,
                   qnn_version_minor,
                   qnn_version_patch,
                   arch_bit,
                   is_model_quantized,
                   model_lang,
                   persistant_data_size=2048000,
                   scratch_mem_size_req=3200000):

    start = time.time()

    print('The eAI model is {} bytes aligned'.format(ALIGN_NUM))

    if arch_bit == 32:
        ptr_byte_size = 4
    elif arch_bit == 64:
        ptr_byte_size = 8

    # ── Load language dictionaries ───────────────────────────────────────────────
    t_dict = time.time()
    if model_lang == "en":
        print("EN MODEL DICTIONARY LOADED ...")
        from melo.en_dict_generation import word_dict, phones_dict, tones_dict, word_offset, phones_offset
        from melo.en_dict_generation import phones_per_word_dict as phones_per_word
        from melo.en_g2p_dict_generation import g2p_token_dict, g2p_phones_dict, g2p_vowel_dict
    elif model_lang == "es":
        print("ES MODEL DICTIONARY LOADED ...")
        from melo.es_dict_generation import word_dict, phones_dict, tones_dict, word_offset, phones_offset
        from melo.es_dict_generation import phones_per_word_dict as phones_per_word
        from melo.es_g2p_dict_generation import g2p_token_dict, g2p_phones_dict
    elif model_lang == "zh":
        print("ZH MODEL DICTIONARY LOADED ...")
        from melo.zh_dict_generation import word_dict, phones_dict, tones_dict, word_offset, phones_offset
        from melo.zh_dict_generation import phones_per_word_dict as phones_per_word
    else:
        print("Error: Model type does not exist!")
    print(f"  dict import took {time.time() - t_dict:.2f}s")

    word_list = word_dict.dict_word_byte
    phones_list = phones_dict.dict_phones
    tones_list = tones_dict.dict_tones
    word_offset_list = word_offset.dict_word_offset
    phones_offset_list = phones_offset.dict_phones_offset
    phones_per_word_list = phones_per_word.dict_phones_per_word

    try:
        g2p_vowel_list = g2p_vowel_dict.dict_vowel
    except NameError:
        g2p_vowel_list = []
    try:
        g2p_token_list = g2p_token_dict.dict_token
    except NameError:
        g2p_token_list = []
    try:
        g2p_phones_list = g2p_phones_dict.dict_phones
    except NameError:
        g2p_phones_list = []

    # ── Read all model files concurrently ─────────────────────────────────────
    t_io = time.time()
    file_paths = [
        bert_model,
        melo_encoder_model,
        melo_flow_model,
        melo_decoder_model,
        melo_sdp_model if is_model_quantized == 1 else None,
        g2p_enc_model,
        g2p_dec_model,
        bert_tokenizer,
        bert_normalizer,
    ]
    with ThreadPoolExecutor(max_workers=len(file_paths)) as pool:
        file_contents = list(pool.map(_read_file, file_paths))
    print(f"Concurrent file reads took {time.time() - t_io:.2f}s")

    (
        bert_model_raw, melo_encoder_model_raw, melo_flow_model_raw,
        melo_decoder_model_raw, melo_sdp_model_raw, g2p_enc_model_raw,
        g2p_dec_model_raw, bert_tokenizer_raw, bert_normalizer_raw,
    ) = file_contents
    del file_contents
    gc.collect()

    # ── Pad each model to 256-byte alignment ──────────────────────────────────
    if bert_model is not None:
        print("BERT MODEL IS ENABLED ...")
        bert_model, bert_model_org_size, bert_model_pad_size = _pad_to_align(bert_model_raw)
    else:
        print("BERT MODEL IS DISABLED ...")
        bert_model, bert_model_org_size, bert_model_pad_size = b'', 0, 0
    del bert_model_raw
    bert_model_size = bert_model_org_size + bert_model_pad_size
    print("BERT MODEL SIZE = ", bert_model_size)

    melo_encoder_model, melo_encoder_model_org_size, melo_encoder_model_pad_size = _pad_to_align(melo_encoder_model_raw)
    del melo_encoder_model_raw
    melo_encoder_model_size = melo_encoder_model_org_size + melo_encoder_model_pad_size
    print("MELO ENCODER MODEL SIZE = ", melo_encoder_model_size)

    melo_flow_model, melo_flow_model_org_size, melo_flow_model_pad_size = _pad_to_align(melo_flow_model_raw)
    del melo_flow_model_raw
    melo_flow_model_size = melo_flow_model_org_size + melo_flow_model_pad_size
    print("MELO FLOW MODEL SIZE = ", melo_flow_model_size)

    melo_decoder_model, melo_decoder_model_org_size, melo_decoder_model_pad_size = _pad_to_align(melo_decoder_model_raw)
    del melo_decoder_model_raw
    melo_decoder_model_size = melo_decoder_model_org_size + melo_decoder_model_pad_size
    print("MELO DECODER MODEL SIZE = ", melo_decoder_model_size)

    melo_sdp_model_size = None
    if is_model_quantized == 1:
        if melo_sdp_model is None:
            print('ERROR: melo_sdp_model is None for quantized model. Exiting...')
            exit(1)
        melo_sdp_model, melo_sdp_model_org_size, melo_sdp_model_pad_size = _pad_to_align(melo_sdp_model_raw)
        melo_sdp_model_size = melo_sdp_model_org_size + melo_sdp_model_pad_size
        print("MELO SDP MODEL SIZE = ", melo_sdp_model_size)
    else:
        melo_sdp_model, melo_sdp_model_org_size, melo_sdp_model_pad_size = b'', None, None
    del melo_sdp_model_raw

    # g2p Encoder Model network
    if g2p_enc_model is not None:
        g2p_enc_model, g2p_encoder_model_org_size, g2p_encoder_model_pad_size = _pad_to_align(g2p_enc_model_raw)
    else:
        g2p_enc_model, g2p_encoder_model_org_size, g2p_encoder_model_pad_size = b'', 0, 0
    del g2p_enc_model_raw
    g2p_encoder_model_size = g2p_encoder_model_org_size + g2p_encoder_model_pad_size
    print("G2P ENCODER MODEL SIZE = ", g2p_encoder_model_size)

    if g2p_dec_model is not None:
        g2p_dec_model, g2p_decoder_model_org_size, g2p_decoder_model_pad_size = _pad_to_align(g2p_dec_model_raw)
    else:
        g2p_dec_model, g2p_decoder_model_org_size, g2p_decoder_model_pad_size = b'', 0, 0
    del g2p_dec_model_raw
    g2p_decoder_model_size = g2p_decoder_model_org_size + g2p_decoder_model_pad_size
    print("G2P DECODER MODEL SIZE = ", g2p_decoder_model_size)

    if bert_tokenizer is not None:
        bert_tokenizer, bert_tokenizer_org_size, bert_tokenizer_pad_size = _pad_to_align(bert_tokenizer_raw)
    else:
        bert_tokenizer, bert_tokenizer_org_size, bert_tokenizer_pad_size = b'', 0, 0
    del bert_tokenizer_raw
    bert_tokenizer_size = bert_tokenizer_org_size + bert_tokenizer_pad_size
    print("BERT TOKENIZER SIZE = ", bert_tokenizer_size)

    if bert_normalizer is not None:
        bert_normalizer, bert_normalizer_org_size, bert_normalizer_pad_size = _pad_to_align(bert_normalizer_raw)
    else:
        bert_normalizer, bert_normalizer_org_size, bert_normalizer_pad_size = b'', 0, 0
    del bert_normalizer_raw
    bert_normalizer_size = bert_normalizer_org_size + bert_normalizer_pad_size
    print("BERT NORMALIZER SIZE = ", bert_normalizer_size)

    # ── Dictionary packing ─────────────────────────────────────────────────────
    t_pack = time.time()
    def _pack_dict_section(data):
        pad = align_n(len(data), 8) - len(data)
        return data + _zero_bytes(pad) if pad else data

    def _pack_int_list(fmt_char, values):
        """Pack a list of integers into bytes using array — no Python-level unpacking."""
        if not values:
            return b''
        # Map struct format chars to array typecodes
        _typecode = {'B': 'B', 'b': 'b', 'I': 'I', 'i': 'i', 'H': 'H', 'h': 'h'}
        typecode = _typecode.get(fmt_char)
        if typecode:
            return array.array(typecode, values).tobytes()
        # Fallback for any other format
        item_size = struct.calcsize(fmt_char)
        buf = bytearray(len(values) * item_size)
        struct.pack_into(f'{len(values)}{fmt_char}', buf, 0, *values)
        return bytes(buf)

    model_byte_dict = b''
    model_byte_dict += _pack_dict_section(_pack_int_list('I', word_offset_list))
    model_byte_dict += _pack_dict_section(struct.pack(f'{len(word_list)}s', word_list))
    model_byte_dict += _pack_dict_section(_pack_int_list('I', phones_offset_list))
    model_byte_dict += _pack_dict_section(_pack_int_list('B', phones_list))
    model_byte_dict += _pack_dict_section(_pack_int_list('B', tones_list))
    model_byte_dict += _pack_dict_section(_pack_int_list('i', g2p_token_list))
    model_byte_dict += _pack_dict_section(_pack_int_list('i', g2p_phones_list))
    model_byte_dict += _pack_dict_section(_pack_int_list('i', g2p_vowel_list))
    model_byte_dict += _pack_dict_section(_pack_int_list('B', phones_per_word_list))

    print(f"  dict packing took {time.time() - t_pack:.2f}s")
    dict_org_size = len(model_byte_dict)
    dict_pad_size = align_n(dict_org_size, ALIGN_NUM) - dict_org_size
    dict_size = dict_org_size + dict_pad_size
    if dict_pad_size:
        model_byte_dict += _zero_bytes(dict_pad_size)
    print("DICTIONARY SIZE = ", dict_size)

    # ── Scratch memory ────────────────────────────────────────────────────────
    scratch_mem_size_req = int(scratch_mem_size_req)
    scratch_memory, scratch_mem_size = allocate_mem(scratch_mem_size_req)
    print("SCRATCH MEM SIZE = ", scratch_mem_size)

    # Replacement dictionary packing (EN and ES only; ZH has no abbreviation dict)
    model_byte_replacement_dict = bytes()
    if model_lang in ("en", "es"):
        if model_lang == "en":
            import melo.en_replacement_dict_generation.original_word_list as orig_wl
            import melo.en_replacement_dict_generation.original_word_offset as orig_wo
            import melo.en_replacement_dict_generation.replacement_word_list as repl_wl
            import melo.en_replacement_dict_generation.replacement_word_offset as repl_wo
        else:
            import melo.es_replacement_dict_generation.original_word_list as orig_wl
            import melo.es_replacement_dict_generation.original_word_offset as orig_wo
            import melo.es_replacement_dict_generation.replacement_word_list as repl_wl
            import melo.es_replacement_dict_generation.replacement_word_offset as repl_wo

        orig_word_list_bytes = orig_wl.dict_original_word_byte
        orig_word_offsets    = orig_wo.dict_original_word_offset
        repl_word_list_bytes = repl_wl.dict_replacement_word_byte
        repl_word_offsets    = repl_wo.dict_replacement_word_offset
        num_entries          = len(orig_word_offsets)

        # replacement_dict fixed header:
        #   uint32_t num_entries
        #   uint32_t original_word_list_size
        #   uint32_t replacement_word_list_size
        #   uint32_t reserved
        model_byte_replacement_dict += struct.pack('IIII',
            num_entries,
            len(orig_word_list_bytes),
            len(repl_word_list_bytes),
            0)

        # Pointer placeholders (4 pointers x 8 bytes each, initialized at runtime)
        model_byte_replacement_dict += struct.pack('b' * 8, *[0] * 8)  # original_word_offset ptr
        model_byte_replacement_dict += struct.pack('b' * 8, *[0] * 8)  # replacement_word_offset ptr
        model_byte_replacement_dict += struct.pack('b' * 8, *[0] * 8)  # original_word_list ptr
        model_byte_replacement_dict += struct.pack('b' * 8, *[0] * 8)  # replacement_word_list ptr

        # original_word_offset array (8-byte aligned)
        temp_org_size = len(model_byte_replacement_dict)
        temp_pad_size = align_n(temp_org_size, 8) - temp_org_size
        if temp_pad_size > 0:
            model_byte_replacement_dict += struct.pack('b' * temp_pad_size, *[0] * temp_pad_size)
        model_byte_replacement_dict += struct.pack('I' * num_entries, *orig_word_offsets)

        # replacement_word_offset array (8-byte aligned)
        temp_org_size = len(model_byte_replacement_dict)
        temp_pad_size = align_n(temp_org_size, 8) - temp_org_size
        if temp_pad_size > 0:
            model_byte_replacement_dict += struct.pack('b' * temp_pad_size, *[0] * temp_pad_size)
        model_byte_replacement_dict += struct.pack('I' * num_entries, *repl_word_offsets)

        # original_word_list blob (8-byte aligned)
        temp_org_size = len(model_byte_replacement_dict)
        temp_pad_size = align_n(temp_org_size, 8) - temp_org_size
        if temp_pad_size > 0:
            model_byte_replacement_dict += struct.pack('b' * temp_pad_size, *[0] * temp_pad_size)
        model_byte_replacement_dict += struct.pack(f'{len(orig_word_list_bytes)}s', orig_word_list_bytes)

        # replacement_word_list blob (8-byte aligned)
        temp_org_size = len(model_byte_replacement_dict)
        temp_pad_size = align_n(temp_org_size, 8) - temp_org_size
        if temp_pad_size > 0:
            model_byte_replacement_dict += struct.pack('b' * temp_pad_size, *[0] * temp_pad_size)
        model_byte_replacement_dict += struct.pack(f'{len(repl_word_list_bytes)}s', repl_word_list_bytes)

    replacement_dict_packed_size = len(model_byte_replacement_dict)
    # 256-byte align the replacement dict
    replacement_dict_pad_size = align_n(replacement_dict_packed_size, ALIGN_NUM) - replacement_dict_packed_size
    replacement_dict_size = replacement_dict_packed_size + replacement_dict_pad_size
    if replacement_dict_pad_size > 0:
        model_byte_replacement_dict += struct.pack('b' * replacement_dict_pad_size, *[0] * replacement_dict_pad_size)
    print("REPLACEMENT DICT SIZE = ", replacement_dict_size)

    # Build persistent data section: replacement dict at the start, zero-padded to persistent_data_size
    persistent_data_size_req = int(persistant_data_size)
    persistent_data_size = align_n(persistent_data_size_req, ALIGN_NUM)
    if replacement_dict_size > persistent_data_size:
        print('ERROR: replacement dict size ({}) exceeds persistent_data_size ({}). Exiting...'.format(
            replacement_dict_size, persistent_data_size))
        exit(1)
    persistent_data_pad_size = persistent_data_size - replacement_dict_size
    model_byte_persistent_data = model_byte_replacement_dict + struct.pack('b' * persistent_data_pad_size, *[0] * persistent_data_pad_size)
    print(f"PERSISTENT DATA PAD SIZE = {persistent_data_pad_size}, PERSISTENT DATA TOTAL SIZE {persistent_data_size}")

    # ── Build structure ─────────────────────────────────────────────────────────
    t_struct = time.time()
    structure_tts = build_structure_tts(
        dict_size,
        bert_model_org_size, bert_model_size,
        melo_encoder_model_org_size, melo_encoder_model_size,
        melo_flow_model_org_size, melo_flow_model_size,
        melo_decoder_model_org_size, melo_decoder_model_size,
        g2p_encoder_model_org_size, g2p_encoder_model_size,
        g2p_decoder_model_org_size, g2p_decoder_model_size,
        bert_tokenizer_org_size, bert_tokenizer_size,
        bert_normalizer_org_size, bert_normalizer_size,
        scratch_mem_size,
        qnn_version_major, qnn_version_minor, qnn_version_patch,
        model_version_major, model_version_minor,
        model_lang,
        word_offset_list, word_list, phones_list, phones_offset_list,
        g2p_token_list, g2p_phones_list, g2p_vowel_list, phones_per_word_list,
        persistent_data_size,
        is_model_quantized,
        melo_sdp_model_org_size,
        melo_sdp_model_size
    )
    print(f"  build_structure_tts took {time.time() - t_struct:.2f}s")

    model_buffer_gen_time = time.time()
    print(f"Model Buffer Generation Time: {model_buffer_gen_time - start:.2f} seconds")
    
    if is_model_quantized == 1:
        model_body = (structure_tts + model_byte_dict + bert_model +
                      melo_encoder_model + melo_sdp_model + melo_flow_model +
                      melo_decoder_model + g2p_enc_model + g2p_dec_model +
                      bert_tokenizer + bert_normalizer + scratch_memory +
                      model_byte_persistent_data)
    else:
        model_body = (structure_tts + model_byte_dict + bert_model +
                      melo_encoder_model + melo_flow_model + melo_decoder_model +
                      g2p_enc_model + g2p_dec_model +
                      bert_tokenizer + bert_normalizer + scratch_memory +
                      model_byte_persistent_data)

    full_model_data = pack_model_body('0LEM', 'MEL0MODE', model_version_major, model_version_minor, model_body)
    print(f"Full Model Buffer (with model body and footer) Time: {time.time() - model_buffer_gen_time:.2f} seconds")

    return full_model_data

def generate_packed_model_file(file_name, model_data):
    if file_name:
        start = time.time()
        write_model(file_name, model_data)
        print(f"Packed Model File Generation Time: {time.time() - start:.2f} seconds")

def main():
    parser = ArgumentParser(description='TTS runtime data builder')
    parser.add_argument('--bert_model', default=None, help='bert model bin file path')
    parser.add_argument('--bert_tokenizer', default=None, help='bert tokenizer bin file path')
    parser.add_argument('--bert_normalizer', default=None, help='bert normalizer bin file path')
    parser.add_argument('--melo_encoder_model', default=None, help='melo encoder model qnn file path')
    parser.add_argument('--melo_sdp_model', default=None, help='melo sdp model qnn file path')
    parser.add_argument('--melo_flow_model', default=None, help='melo flow model qnn file path')
    parser.add_argument('--melo_decoder_model', default=None, help='melo decoder model qnn file path')
    parser.add_argument('--g2p_enc_model', default=None, help='g2p encoder model qnn file path')
    parser.add_argument('--g2p_dec_model', default=None, help='g2p decoder model qnn file path')
    parser.add_argument('--model_version_major', default=None, help='model version major (e.g., KP=1.0, Hawi=2.0)')
    parser.add_argument('--model_version_minor', default=None, help='model version minor (e.g., KP=1.0, Hawi=2.0)')
    parser.add_argument('--qnn_version_major', default=None, help='qnn version major')
    parser.add_argument('--qnn_version_minor', default=None, help='qnn version minor')
    parser.add_argument('--qnn_version_patch', default=None, help='qnn version patch')
    parser.add_argument('--arch', default=None, help='32-bit/64-bit architecture')
    parser.add_argument('--is_model_quantized', default=None, help='are enc/dec models quantized or not')
    parser.add_argument('--model_lang', default=None, help='English/Multi-lingual model')
    parser.add_argument('--scratch_mem_size_req', help='General scratch memory size')
    parser.add_argument('--persistent_data_size', default=2048000, help='Persistent data section size in bytes (default: 2MB)')
    parser.add_argument('--path_out_model', help='Output file name for tts model')
    args = parser.parse_args()
    print(args)

    # Get global variables from command line
    qnn_version_major = int(args.qnn_version_major)
    qnn_version_minor = int(args.qnn_version_minor)
    qnn_version_patch = int(args.qnn_version_patch)
    arch_bit = int(args.arch)
    model_lang = args.model_lang
    model_version_major = int(args.model_version_major)
    model_version_minor = int(args.model_version_minor)
    is_model_quantized = int(args.is_model_quantized) if args.is_model_quantized else 0

    # Generate model using the generate_model function
    model_data = generate_model(
        bert_model=args.bert_model,
        bert_tokenizer=args.bert_tokenizer,
        bert_normalizer=args.bert_normalizer,
        melo_encoder_model=args.melo_encoder_model,
        melo_flow_model=args.melo_flow_model,
        melo_decoder_model=args.melo_decoder_model,
        g2p_enc_model=args.g2p_enc_model,
        g2p_dec_model=args.g2p_dec_model,
        qnn_version_major=qnn_version_major,
        qnn_version_minor=qnn_version_minor,
        qnn_version_patch=qnn_version_patch,
        arch_bit=arch_bit,
        model_lang=model_lang,
        scratch_mem_size_req=int(args.scratch_mem_size_req),
        is_model_quantized=is_model_quantized,
        persistant_data_size=int(args.persistent_data_size),
        melo_sdp_model=args.melo_sdp_model
    )

    # Write the model to file
    generate_packed_model_file(args.path_out_model, model_data)


if __name__ == '__main__':
    main()
