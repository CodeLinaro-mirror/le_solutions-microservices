# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear


from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

import time
import sys
import struct
from argparse import ArgumentParser
import sys
import math
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
    piper_encoder_model_org_size,
    piper_encoder_model_pad_size,  
    piper_sdp_model_org_size,
    piper_sdp_model_pad_size,
    piper_flow_model_org_size,
    piper_flow_model_pad_size,
    piper_decoder_model_org_size,
    piper_decoder_model_pad_size,
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
    phones_per_word_list,
    persistent_data_size
):

    body = bytes()

    repeat_count_threshold = 10
    piper_max_tts_char_size = 1024
    piper_max_seq_len = 512
    piper_max_pcm_size =  20480      # (40 x 256 x 2 for 0.46s) //todo: test with 20480
    bert_tokenizer_struct_size = 304
    bert_max_tokens = 400
    piper_max_token_len = 128
    bert_tokenstream_struct_size = 557060
    split_sentence_struct_size = 10224
    piper_speech_format_struct_size = 40
    header_size = 16
    dsp_kpps = 100000
    so_filename_size = 32
    B = 1
    T_Y = 3 * piper_max_seq_len
    piper_out_z_max_size = 192 * T_Y
    piper_y_mask_len = B * T_Y
    piper_attn_squeezed_len = T_Y * piper_max_seq_len

    # Define backend SO filenames
    # Using just the filename (no path) allows the system to search in LD_LIBRARY_PATH
    # You can set the library path at runtime using:
    # export LD_LIBRARY_PATH=/path/to/libs:$LD_LIBRARY_PATH
    backend_file_name = "libQnnHtp.so"
    system_file_name = "libQnnSystem.so"

    if "en" == model_lang:
        g2p_lang_prefix = "<eng-us>: "
    elif "es" == model_lang:
        g2p_lang_prefix = "<spa>: "
    elif "de" == model_lang:
        g2p_lang_prefix = "<deu>: "
    elif "it" == model_lang:
        g2p_lang_prefix = "<ita>: "
    else:
        g2p_lang_prefix = ""

    # Make sure the strings are within the defined size
    backend_file_name = backend_file_name[:so_filename_size].ljust(so_filename_size, '\0')
    system_file_name = system_file_name[:so_filename_size].ljust(so_filename_size, '\0')

    g2p_lang_prefix = g2p_lang_prefix[:so_filename_size].ljust(so_filename_size, '\0')

    # uint32_t struct_size;         - size of piper_struct. to be added at the end of this function
    # uint32_t struct_w_pad_size;   - size of piper_struct + padding. to be added at the end of this function

    # uint32_t dict_size;
    body += struct.pack('I', dict_size)

    # uint32_t piper_encoder_model_org_size;
    # uint32_t piper_encoder_model_pad_size;
    body += struct.pack('II', piper_encoder_model_org_size, piper_encoder_model_pad_size)

    # uint32_t piper_sdp_model_org_size;
    # uint32_t piper_sdp_model_pad_size;
    body += struct.pack('II', piper_sdp_model_org_size, piper_sdp_model_pad_size)


    # uint32_t piper_flow_model_org_size;
    # uint32_t piper_flow_model_pad_size;
    body += struct.pack('II', piper_flow_model_org_size, piper_flow_model_pad_size)

    # uint32_t piper_decoder_model_org_size;
    # uint32_t piper_decoder_model_pad_size;
    body += struct.pack('II', piper_decoder_model_org_size, piper_decoder_model_pad_size)

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

    # uint32_t piper_encoder_backend_type;
    body += struct.pack('I', 1)

    # uint32_t piper_sdp_backend_type:
    body += struct.pack('I', 1)

    # uint32_t piper_flow_backend_type;
    body += struct.pack('I', 1)

    # uint32_t piper_decoder_backend_type;
    body += struct.pack('I', 1)

    # uint32_t g2p_encoder_backend_type;
    # uint32_t g2p_decoder_backend_type;
    body += struct.pack('II', 1, 1)

    # uint32_t KPPS;
    body += struct.pack('i', dsp_kpps)

    # uint32_t htp_power_cfg_profilemode;
    body += struct.pack('I', 10)         # CUSTOM_PERF_PROFILE_MODE

    #uint32_t htp_heap_grow_enable;
    body += struct.pack('I', 0)

    #uint32_t htp_heap_grow_size_in_mb;
    body += struct.pack('I', 32)

    # char backend_file_name[so_filename_size];
    body += struct.pack(f'{so_filename_size}s', backend_file_name.encode('utf-8'))

    # char system_file_name[so_filename_size];
    body += struct.pack(f'{so_filename_size}s', system_file_name.encode('utf-8'))

    #========================================
    # uint32_t piper_current_phones_len;
    body += struct.pack('I', 0)

    # uint32_t input_language_code;
    if "en" == model_lang:
        body += struct.pack('I', 0)
    elif "zh" == model_lang:
        body += struct.pack('I', 1)
    elif "de" == model_lang:
        body += struct.pack('I', 2)
    elif "es" == model_lang:
        body += struct.pack('I', 3)
    elif "it" == model_lang:
        body += struct.pack('I', 15)   # piper_it = 15 in PIPER_LANGUAGE_CODE enum

    # uint32_t input_text_size;
    body += struct.pack('I', 0)

    # uint8_t input_text[PIPER_MAX_TTS_CHAR_SIZE];
    body += struct.pack('b' * piper_max_tts_char_size, *[0] * piper_max_tts_char_size)

    # uint32_t piper_input_phones_array[PIPER_MAX_SEQ_LEN];
    body += struct.pack('I' * piper_max_seq_len, *[0] * piper_max_seq_len)

    # uint8_t output_pcm[PIPER_MAX_PCM_SIZE];
    body += struct.pack('b' * piper_max_pcm_size, *[0] * piper_max_pcm_size)

    # uint32_t output_pcm_size;
    body += struct.pack('I', 0)

    # uint32_t current_inference_sentence;
    body += struct.pack('I', 0)

    # bool final_flag;
    body += struct.pack('b', 0)

    # bool piper_decoder_final_flag;
    body += struct.pack('b', 1)

    # bool is_g2p_enabled;
    if ("en" == model_lang) or ("de" == model_lang) or ("it" == model_lang):
        body += struct.pack('b', 1)
    elif ("zh" == model_lang) or ("es" == model_lang):
        body += struct.pack('b', 0)

    # bool use_bert_tokenizer;
    # true  (1) only for Chinese (zh): use full BERT tokenization path
    # false (0) for all others: use PreTokenize path directly
    if "zh" == model_lang:
        body += struct.pack('b', 1)
    else:
        body += struct.pack('b', 0)

    #========================================
    # uint32_t sample_rate;
    body += struct.pack('I', 22050)

    # uint32_t bits_per_sample;
    body += struct.pack('I', 16)

    # uint32_t num_channels;
    body += struct.pack('I', 1)

    # uint32_t num_formats;
    body += struct.pack('I', 1)

    # uint32_t speech_format;
    body += struct.pack('I', 0)       # enum = LINEAR16

    # int8_t reserved_1[4];
    body += struct.pack('b' * 4, *[0] * 4)

    # piper_speech_format_pcm_t piper_speech_format_pcm;
    body += struct.pack('b' * piper_speech_format_struct_size, *[0] * piper_speech_format_struct_size)

    #========================================
    # int64_t x_lengths;
    body += struct.pack('q', 0)

    # float noise_scale;
    body += struct.pack('f', 0.667)

    # float noise_scale_w;
    body += struct.pack('f', 0.8)

    # float length_scale;
    body += struct.pack('f', 1.0)

    # float piper_out_z[PIPER_OUT_Z_MAX_SIZE * T_Y];
    body += struct.pack('f' * piper_out_z_max_size, *[0] * piper_out_z_max_size)

    # float x_mask_f[PIPER_MAX_SEQ_LEN];
    body += struct.pack('f' * piper_max_seq_len, *[0] * piper_max_seq_len)

    # float w_ceil_f[PIPER_MAX_SEQ_LEN];
    body += struct.pack('f' * piper_max_seq_len, *[0] * piper_max_seq_len)

    # float y_mask_f[B * T_Y];
    body += struct.pack('f' * piper_y_mask_len, *[0] * piper_y_mask_len)

    # float attn_squeezed_f[T_Y * PIPER_MAX_SEQ_LEN];
    body += struct.pack('f' * piper_attn_squeezed_len, *[0] * piper_attn_squeezed_len)

    # uint32_t y_lengths;
    # uint32_t total_dec_seq_len;
    body += struct.pack('II', 0, 0)

    # int8_t reserved_2[4];
    body += struct.pack('b' * 4, *[0] * 4)

    #========================================
    # char* dict_all_words;
    body += struct.pack('b' * 8, *[0] * 8)

    # uint32_t* dict_word_offset;
    body += struct.pack('b' * 8, *[0] * 8)

    # uint32_t* dict_phones_offset;
    body += struct.pack('b' * 8, *[0] * 8)

    # uint8_t* phones_list_array;
    body += struct.pack('b' * 8, *[0] * 8)

    # uint8_t* phones_per_word_list_array;
    body += struct.pack('b' * 8, *[0] * 8)

    # int32_t* g2p_token_list_array;
    body += struct.pack('b' * 8, *[0] * 8)

    # int32_t* g2p_phones_list_array;
    body += struct.pack('b' * 8, *[0] * 8)

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

    # uint32_t repeat_count_threshold;
    body += struct.pack('I', repeat_count_threshold)   # To reduce hallucination

    # int8_t reserved_3[4];
    body += struct.pack('b' * 4, *[0] * 4)

    # char g2p_lang_prefix[so_filename_size];
    body += struct.pack(f'{so_filename_size}s', g2p_lang_prefix.encode('utf-8'))

    #========================================
    # struct Tokenizer bert_tokenizer;
    body += struct.pack('b' * bert_tokenizer_struct_size, *[0] * bert_tokenizer_struct_size)

    # struct TokenStream preTokens;
    body += struct.pack('b' * bert_tokenstream_struct_size, *[0] * bert_tokenstream_struct_size)

    # struct TokenStream outTokens;
    body += struct.pack('b' * bert_tokenstream_struct_size, *[0] * bert_tokenstream_struct_size)

    # uint8_t word_count[PIPER_BERT_MAX_TOKENS];
    body += struct.pack('b' * bert_max_tokens, *[0] * bert_max_tokens)

    # uint8_t phones_per_word[PIPER_BERT_MAX_TOKENS];
    body += struct.pack('b' * bert_max_tokens, *[0] * bert_max_tokens)

    # char bert_word_groups[PIPER_BERT_MAX_TOKENS * MAX_TOKEN_LEN];
    body += struct.pack('b' * bert_max_tokens * piper_max_token_len, *[0] * bert_max_tokens * piper_max_token_len)

    # uint32_t bert_offset_list[PIPER_BERT_MAX_TOKENS];
    body += struct.pack('I' * bert_max_tokens, *[0] * bert_max_tokens)

    #========================================
    # uint8_t* bert_tokenizer_ptr;
    body += struct.pack('b' * 8, *[0] * 8)

    # uint8_t* bert_normalizer_ptr;
    body += struct.pack('b' * 8, *[0] * 8)

    #========================================
    # split_sentence split_sentence;
    body += struct.pack('b' * split_sentence_struct_size, *[0] * split_sentence_struct_size)

    # uint64_t persistent_data_size;
    body += struct.pack('Q', persistent_data_size)

    # uint8_t* persistent_data_ptr;  (pointer placeholder, initialized at runtime)
    body += struct.pack('b' * 8, *[0] * 8)

    # int8_t reserved_4[8];
    body += struct.pack('b' * 8, *[0] * 8)

    #========================================
    # piper_config_t  piper_config;
    body += struct.pack('i' * 8, *[0] * 8)

    #========================================
    # uint64_t scratch_mem_size;
    body += struct.pack('Q', scratch_mem_size)

    # uint64_t scratch_mem_offset;
    body += struct.pack('Q', 0)

    # uint8_t* scratch_mem_ptr;
    body += struct.pack('b' * 8, *[0] * 8)

    #========================================
    # uint32_t struct_size;         - size of piper_struct
    # uint32_t struct_w_pad_size;   - size of piper_struct + padding

    struct_size = len(body) + 8     # extra 8 bytes accounting for struct_size & struct_w_pad_size
    struct_w_pad_size = align_n(struct_size + header_size, ALIGN_NUM) - header_size # (tts structure + padding + header) needs to be 256 byte aligned
    pad_size = struct_w_pad_size - struct_size

    # padding size after struct for alignment
    body += struct.pack('b' * pad_size, *[0] * pad_size)

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

# Allocate persistent or scratch mem for qnn models
def allocate_mem(mem_size_req):
    # Allocate 10% extra memory for persistent/scratch as memory requirements can change with EAI version change
    extra_mem_size_factor = 0.1
    mem_size = align_n(round(mem_size_req*(1 + extra_mem_size_factor)), ALIGN_NUM)
    mem_byte = bytes()
    mem_byte += struct.pack('b' * mem_size, *[0] * mem_size)
    return(mem_byte, mem_size)

def generate_model(
    bert_model,
    bert_tokenizer,
    bert_normalizer,
    piper_encoder_model,
    piper_sdp_model,
    piper_flow_model,
    piper_decoder_model,
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
    persistent_data_size=2048000,
    scratch_mem_size_req=3200000
):
    """
    Generate a packaged Piper TTS model buffer/file.
    
    This function is a wrapper that creates a single model buffer 
    and/or file containing all model components and metadata.
    
    Args:
        bert_model: Path to BERT model (not used in Piper, kept for compatibility)
        bert_tokenizer: Path to BERT tokenizer binary
        bert_normalizer: Path to Unicode normalizer binary
        piper_encoder_model: Path to Piper encoder QNN model
        piper_sdp_model: Path to Piper SDP QNN model
        piper_flow_model: Path to Piper flow QNN model
        piper_decoder_model: Path to Piper decoder QNN model
        g2p_enc_model: Path to G2P encoder model
        g2p_dec_model: Path to G2P decoder model
        model_version_major: Model major version
        model_version_minor: Model minor version
        qnn_version_major: QNN SDK major version
        qnn_version_minor: QNN SDK minor version
        qnn_version_patch: QNN SDK patch version
        arch_bit: Architecture bit width (32 or 64)
        is_model_quantized: Whether models are quantized (not used in Piper, kept for compatibility)
        model_lang: Language code (en, es, zh)
        scratch_mem_size_req: Scratch memory size in bytes (default 3.2MB)
    """

    print('The eAI model is {} bytes aligned'.format(ALIGN_NUM))
    start = time.time()

    if arch_bit == 32:
        ptr_byte_size = 4
    elif arch_bit == 64:
        ptr_byte_size = 8

    if model_lang == "en":
        print("EN MODEL DICTIONARY LOADED ...")
        import piper.en_dict_generation.word_dict as word_dict
        import piper.en_dict_generation.phones_dict as phones_dict
        import piper.en_dict_generation.word_offset as word_offset
        import piper.en_dict_generation.phones_offset as phones_offset
        import piper.en_dict_generation.phones_per_word_dict as phones_per_word

        import piper.en_g2p_dict_generation.g2p_token_dict as g2p_token_dict
        import piper.en_g2p_dict_generation.g2p_phones_dict as g2p_phones_dict
    elif model_lang == "es":
        print("ES MODEL DICTIONARY LOADED ...")
        import piper.es_dict_generation.word_dict as word_dict
        import piper.es_dict_generation.phones_dict as phones_dict
        import piper.es_dict_generation.word_offset as word_offset
        import piper.es_dict_generation.phones_offset as phones_offset
        import piper.es_dict_generation.phones_per_word_dict as phones_per_word

        # import es_g2p_dict_generation.g2p_token_dict as g2p_token_dict
        # import es_g2p_dict_generation.g2p_phones_dict as g2p_phones_dict
    elif model_lang == "zh":
        print("ZH MODEL DICTIONARY LOADED ...")
        import piper.zh_dict_generation.word_dict as word_dict
        import piper.zh_dict_generation.phones_dict as phones_dict
        import piper.zh_dict_generation.word_offset as word_offset
        import piper.zh_dict_generation.phones_offset as phones_offset
        import piper.zh_dict_generation.phones_per_word_dict as phones_per_word
    elif model_lang == "de":
        print("DE MODEL DICTIONARY LOADED ...")
        import piper.de_dict_generation.word_dict as word_dict
        import piper.de_dict_generation.phones_dict as phones_dict
        import piper.de_dict_generation.word_offset as word_offset
        import piper.de_dict_generation.phones_offset as phones_offset
        import piper.de_dict_generation.phones_per_word_dict as phones_per_word

        import piper.de_g2p_dict_generation.g2p_token_dict as g2p_token_dict
        import piper.de_g2p_dict_generation.g2p_phones_dict as g2p_phones_dict
    elif model_lang == "it":
        print("IT MODEL DICTIONARY LOADED ...")
        import piper.it_dict_generation.word_dict as word_dict
        import piper.it_dict_generation.phones_dict as phones_dict
        import piper.it_dict_generation.word_offset as word_offset
        import piper.it_dict_generation.phones_offset as phones_offset
        import piper.it_dict_generation.phones_per_word_dict as phones_per_word

        import piper.it_g2p_dict_generation.g2p_token_dict as g2p_token_dict
        import piper.it_g2p_dict_generation.g2p_phones_dict as g2p_phones_dict
    else:
        print("Error: Model type does not exist!")

    word_list = word_dict.dict_word_byte
    phones_list = phones_dict.dict_phones
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

    # piper Encoder Model network
    f = open(piper_encoder_model, 'rb')
    piper_encoder_model = f.read()
    f.close()

    piper_encoder_model_org_size = len(piper_encoder_model)
    piper_encoder_model_pad_size = align_n(piper_encoder_model_org_size, ALIGN_NUM) - piper_encoder_model_org_size
    piper_encoder_model_size = piper_encoder_model_org_size + piper_encoder_model_pad_size

    # piper Encoder Model 256 byte aligned (so that if there is any subsequent qnn model is 256 byte aligned)
    if (piper_encoder_model_org_size != piper_encoder_model_size):
        piper_encoder_model += struct.pack('b' * piper_encoder_model_pad_size, *[0] * piper_encoder_model_pad_size)
    print("PIPER ENCODER MODEL SIZE = ", piper_encoder_model_size)

    # piper SDP Model Network
    f = open(piper_sdp_model, 'rb')
    piper_sdp_model = f.read()
    f.close()

    piper_sdp_model_org_size = len(piper_sdp_model)
    piper_sdp_model_pad_size = align_n(piper_sdp_model_org_size, ALIGN_NUM) - piper_sdp_model_org_size
    piper_sdp_model_size = piper_sdp_model_org_size + piper_sdp_model_pad_size

    # piper SDP Model 256 byte aligned (so that if there is any subsequent qnn model is 256 byte aligned)
    if (piper_sdp_model_org_size != piper_sdp_model_size):
        piper_sdp_model += struct.pack('b' * piper_sdp_model_pad_size, *[0] * piper_sdp_model_pad_size)
    print("piper SDP MODEL SIZE = ", piper_sdp_model_size)

    # piper Flow Model network
    f = open(piper_flow_model, 'rb')
    piper_flow_model = f.read()
    f.close()

    piper_flow_model_org_size = len(piper_flow_model)
    piper_flow_model_pad_size = align_n(piper_flow_model_org_size, ALIGN_NUM) - piper_flow_model_org_size
    piper_flow_model_size = piper_flow_model_org_size + piper_flow_model_pad_size

    # piper Flow Model 256 byte aligned (so that if there is any subsequent qnn model is 256 byte aligned)
    if (piper_flow_model_org_size != piper_flow_model_size):
        piper_flow_model += struct.pack('b' * piper_flow_model_pad_size, *[0] * piper_flow_model_pad_size)
    print("PIPER FLOW MODEL SIZE = ", piper_flow_model_size)

    # piper Decoder Model network
    f = open(piper_decoder_model, 'rb')
    piper_decoder_model = f.read()
    f.close()

    piper_decoder_model_org_size = len(piper_decoder_model)
    piper_decoder_model_pad_size = align_n(piper_decoder_model_org_size, ALIGN_NUM) - piper_decoder_model_org_size
    piper_decoder_model_size = piper_decoder_model_org_size + piper_decoder_model_pad_size

    # piper Decoder Model 256 byte aligned (so that if there is any subsequent qnn model is 256 byte aligned)
    if (piper_decoder_model_org_size != piper_decoder_model_size):
        piper_decoder_model += struct.pack('b' * piper_decoder_model_pad_size, *[0] * piper_decoder_model_pad_size)
    print("PIPER DECODER MODEL SIZE = ", piper_decoder_model_size)

    # g2p Encoder Model network
    if g2p_enc_model is not None:
        f = open(g2p_enc_model, 'rb')
        g2p_enc_model = f.read()
        f.close()

        g2p_encoder_model_org_size = len(g2p_enc_model)
    else:
        # Initialize empty g2p enc model in case g2p is disabled
        g2p_encoder_model_org_size = 0
        g2p_enc_model = b''

    g2p_encoder_model_pad_size = align_n(g2p_encoder_model_org_size, ALIGN_NUM) - g2p_encoder_model_org_size
    g2p_encoder_model_size = g2p_encoder_model_org_size + g2p_encoder_model_pad_size

    # piper Encoder Model 256 byte aligned (so that if there is any subsequent qnn model is 256 byte aligned)
    if (g2p_encoder_model_org_size != g2p_encoder_model_size):
        g2p_enc_model += struct.pack('b' * g2p_encoder_model_pad_size, *[0] * g2p_encoder_model_pad_size)
    print("G2P ENCODER MODEL SIZE = ", g2p_encoder_model_size)

    # g2p Decoder Model network
    if g2p_dec_model is not None:
        f = open(g2p_dec_model, 'rb')
        g2p_dec_model = f.read()
        f.close()

        g2p_decoder_model_org_size = len(g2p_dec_model)
    else:
        # Initialize empty g2p dec model in case g2p is disabled
        g2p_decoder_model_org_size = 0
        g2p_dec_model = b''

    g2p_decoder_model_pad_size = align_n(g2p_decoder_model_org_size, ALIGN_NUM) - g2p_decoder_model_org_size
    g2p_decoder_model_size = g2p_decoder_model_org_size + g2p_decoder_model_pad_size

    # piper Encoder Model 256 byte aligned (so that if there is any subsequent qnn model is 256 byte aligned)
    if (g2p_decoder_model_org_size != g2p_decoder_model_size):
        g2p_dec_model += struct.pack('b' * g2p_decoder_model_pad_size, *[0] * g2p_decoder_model_pad_size)
    print("G2P DECODER MODEL SIZE = ", g2p_decoder_model_size)

    # Dictionary packing
    model_byte_dict = bytes()
    # Offset for each word in dictionary.
    model_byte_dict += struct.pack('I' * len(word_offset_list), *word_offset_list)
    temp_org_size = len(model_byte_dict)
    temp_pad_size = align_n(temp_org_size, 8) - temp_org_size
    temp_align_size = temp_org_size + temp_pad_size
    if (temp_org_size != temp_align_size):
        model_byte_dict += struct.pack('b' * temp_pad_size, *[0] * temp_pad_size)

    # Pointer array for dict. Use 32/64 bit pointer for each word.
    # model_byte_dict += struct.pack('b' * ptr_byte_size * len(word_offset_list), *[0] * ptr_byte_size * len(word_offset_list))

    # Dictionary word sequence. Each word ends with \0
    model_byte_dict += struct.pack(f'{len(word_list)}s', word_list)
    temp_org_size = len(model_byte_dict)
    temp_pad_size = align_n(temp_org_size, 8) - temp_org_size
    temp_align_size = temp_org_size + temp_pad_size
    if (temp_org_size != temp_align_size):
        model_byte_dict += struct.pack('b' * temp_pad_size, *[0] * temp_pad_size)

    # Offset for each phone in dictionary.
    model_byte_dict += struct.pack('I' * len(phones_offset_list), *phones_offset_list)
    temp_org_size = len(model_byte_dict)
    temp_pad_size = align_n(temp_org_size, 8) - temp_org_size
    temp_align_size = temp_org_size + temp_pad_size
    if (temp_org_size != temp_align_size):
        model_byte_dict += struct.pack('b' * temp_pad_size, *[0] * temp_pad_size)

    # Phones for each word in dictionary.
    model_byte_dict += struct.pack('B' * len(phones_list), *phones_list)
    temp_org_size = len(model_byte_dict)
    temp_pad_size = align_n(temp_org_size, 8) - temp_org_size
    temp_align_size = temp_org_size + temp_pad_size
    if (temp_org_size != temp_align_size):
        model_byte_dict += struct.pack('b' * temp_pad_size, *[0] * temp_pad_size)

    model_byte_dict += struct.pack('i' * len(g2p_token_list), *g2p_token_list)
    temp_org_size = len(model_byte_dict)
    temp_pad_size = align_n(temp_org_size, 8) - temp_org_size
    temp_align_size = temp_org_size + temp_pad_size
    if (temp_org_size != temp_align_size):
        model_byte_dict += struct.pack('b' * temp_pad_size, *[0] * temp_pad_size)

    model_byte_dict += struct.pack('i' * len(g2p_phones_list), *g2p_phones_list)
    temp_org_size = len(model_byte_dict)
    temp_pad_size = align_n(temp_org_size, 8) - temp_org_size
    temp_align_size = temp_org_size + temp_pad_size
    if (temp_org_size != temp_align_size):
        model_byte_dict += struct.pack('b' * temp_pad_size, *[0] * temp_pad_size)

    model_byte_dict += struct.pack('i' * len(g2p_vowel_list), *g2p_vowel_list)
    temp_org_size = len(model_byte_dict)
    temp_pad_size = align_n(temp_org_size, 8) - temp_org_size
    temp_align_size = temp_org_size + temp_pad_size
    if (temp_org_size != temp_align_size):
        model_byte_dict += struct.pack('b' * temp_pad_size, *[0] * temp_pad_size)

    # Number of phones for each word in dictionary.
    model_byte_dict += struct.pack('B' * len(phones_per_word_list), *phones_per_word_list)
    temp_org_size = len(model_byte_dict)
    temp_pad_size = align_n(temp_org_size, 8) - temp_org_size
    temp_align_size = temp_org_size + temp_pad_size
    if (temp_org_size != temp_align_size):
        model_byte_dict += struct.pack('b' * temp_pad_size, *[0] * temp_pad_size)

    # Size of dictionary model in bytes
    dict_org_size = len(model_byte_dict)
    dict_pad_size = align_n(dict_org_size, ALIGN_NUM) - dict_org_size
    dict_size = dict_org_size + dict_pad_size

    # dict model needs to be 256 byte aligned (so that subsequent qnn model is 256 byte aligned)
    if (dict_org_size != dict_size):
        model_byte_dict += struct.pack('b' * dict_pad_size, *[0] * dict_pad_size)
    print("DICTIONARY SIZE = ", dict_size)

    # BERT Tokenizer binary — only needed for Chinese (zh)
    if model_lang == "zh" and bert_tokenizer is not None:
        f = open(bert_tokenizer, 'rb')
        bert_tokenizer = f.read()
        f.close()

        bert_tokenizer_org_size = len(bert_tokenizer)
    else:
        # use_bert_tokenizer is False for this language — skip tokenizer file
        bert_tokenizer_org_size = 0
        bert_tokenizer = b''

    bert_tokenizer_pad_size = align_n(bert_tokenizer_org_size, ALIGN_NUM) - bert_tokenizer_org_size
    bert_tokenizer_size = bert_tokenizer_org_size + bert_tokenizer_pad_size

    # Tokenizer Autogen 256 byte aligned (so that if there is any subsequent qnn model is 256 byte aligned)
    if (bert_tokenizer_org_size != bert_tokenizer_size):
        bert_tokenizer += struct.pack('b' * bert_tokenizer_pad_size, *[0] * bert_tokenizer_pad_size)
    print("BERT TOKENIZER SIZE = ", bert_tokenizer_size)

    # Read normalizer binary, pad to align (256), pack to struct
    # Unicode Normalizer binary
    if bert_normalizer is not None:
        f = open(bert_normalizer, 'rb')
        bert_normalizer = f.read()
        f.close()

        bert_normalizer_org_size = len(bert_normalizer)
    else:
        # Initialize empty BERT normalizer in case BERT is disabled
        bert_normalizer_org_size = 0
        bert_normalizer = b''

    bert_normalizer_pad_size = align_n(bert_normalizer_org_size, ALIGN_NUM) - bert_normalizer_org_size
    bert_normalizer_size = bert_normalizer_org_size + bert_normalizer_pad_size

    # Normalizer Autogen 256 byte aligned (so that if there is any subsequent qnn model is 256 byte aligned)
    if (bert_normalizer_org_size != bert_normalizer_size):
        bert_normalizer += struct.pack('b' * bert_normalizer_pad_size, *[0] * bert_normalizer_pad_size)
    print("BERT NORMALIZER SIZE = ", bert_normalizer_size)

    # Allocate scratch mem
    scratch_mem_size_req = int(scratch_mem_size_req)
    scratch_memory , scratch_mem_size = allocate_mem(scratch_mem_size_req)
    print("SCRATCH MEM SIZE = ", scratch_mem_size)

    # Replacement dictionary packing (EN and ES only; ZH has no abbreviation dict)
    model_byte_replacement_dict = bytes()
    if model_lang in ("en", "es"):
        if model_lang == "en":
            import piper.en_replacement_dict_generation.original_word_list as orig_wl
            import piper.en_replacement_dict_generation.original_word_offset as orig_wo
            import piper.en_replacement_dict_generation.replacement_word_list as repl_wl
            import piper.en_replacement_dict_generation.replacement_word_offset as repl_wo
        # else:
        #     import es_replacement_dict_generation.original_word_list as orig_wl
        #     import es_replacement_dict_generation.original_word_offset as orig_wo
        #     import es_replacement_dict_generation.replacement_word_list as repl_wl
        #     import es_replacement_dict_generation.replacement_word_offset as repl_wo

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
    persistent_data_size_req = int(persistent_data_size)
    persistent_data_size = align_n(persistent_data_size_req, ALIGN_NUM)
    if replacement_dict_size > persistent_data_size:
        print('ERROR: replacement dict size ({}) exceeds persistent_data_size ({}). Exiting...'.format(
            replacement_dict_size, persistent_data_size))
        exit(1)
    persistent_data_pad_size = persistent_data_size - replacement_dict_size
    model_byte_persistent_data = model_byte_replacement_dict + struct.pack('b' * persistent_data_pad_size, *[0] * persistent_data_pad_size)
    print(f"PERSISTENT DATA PAD SIZE = {persistent_data_pad_size}, PERSISTENT DATA TOTAL SIZE {persistent_data_size}")

    structure_tts = build_structure_tts(
        dict_size,
        piper_encoder_model_org_size,
        piper_encoder_model_size,
        piper_sdp_model_org_size,
        piper_sdp_model_size,
        piper_flow_model_org_size,
        piper_flow_model_size,
        piper_decoder_model_org_size,
        piper_decoder_model_size,
        g2p_encoder_model_org_size,
        g2p_encoder_model_size,
        g2p_decoder_model_org_size,
        g2p_decoder_model_size,
        bert_tokenizer_org_size,
        bert_tokenizer_size,
        bert_normalizer_org_size,
        bert_normalizer_size,
        scratch_mem_size,
        
        # Additional arguments
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
        phones_per_word_list,
        persistent_data_size
    )

    model_buffer_gen_time = time.time()
    print(f"Model Buffer Generation Time: {model_buffer_gen_time - start:.2f} seconds")
    
    model_body = structure_tts+model_byte_dict+piper_encoder_model+piper_sdp_model+piper_flow_model+piper_decoder_model+g2p_enc_model+g2p_dec_model+bert_tokenizer+bert_normalizer+scratch_memory+model_byte_persistent_data
    
    full_model_data = pack_model_body('0PIP', 'PIP0MODE', model_version_major, model_version_minor, model_body)
    print(f"Full Model Buffer (with model body and footer) Time: {time.time() - model_buffer_gen_time:.2f} seconds")

    return full_model_data

def generate_packed_model_file(file_name, model_data):
    if file_name:
        start = time.time()
        write_model(file_name, model_data)
        print(f"Packed Model File Generation Time: {time.time() - start:.2f} seconds")

def main():
    parser = ArgumentParser(description='TTS runtime data builder')
    parser.add_argument('--bert_tokenizer', default=None, help='bert tokenizer bin file path')
    parser.add_argument('--bert_normalizer', default=None, help='bert normalizer bin file path')
    parser.add_argument('--piper_encoder_model', default=None, help='piper encoder model qnn file path')
    parser.add_argument('--piper_sdp_model', default=None, help='piper sdp model qnn file path')
    parser.add_argument('--piper_flow_model', default=None, help='piper flow model qnn file path')
    parser.add_argument('--piper_decoder_model', default=None, help='piper decoder model qnn file path')
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
    model_version_major = int(args.model_version_major) if args.model_version_major else 1
    model_version_minor = int(args.model_version_minor) if args.model_version_minor else 0
    is_model_quantized = int(args.is_model_quantized) if args.is_model_quantized else 0

    # Generate model using the generate_model function
    model_data = generate_model(
        bert_model=None,  # Piper doesn't use BERT model
        bert_tokenizer=args.bert_tokenizer,
        bert_normalizer=args.bert_normalizer,
        piper_encoder_model=args.piper_encoder_model,
        piper_sdp_model=args.piper_sdp_model,
        piper_flow_model=args.piper_flow_model,
        piper_decoder_model=args.piper_decoder_model,
        g2p_enc_model=args.g2p_enc_model,
        g2p_dec_model=args.g2p_dec_model,
        model_version_major=model_version_major,
        model_version_minor=model_version_minor,
        qnn_version_major=qnn_version_major,
        qnn_version_minor=qnn_version_minor,
        qnn_version_patch=qnn_version_patch,
        arch_bit=arch_bit,
        is_model_quantized=is_model_quantized,
        model_lang=model_lang,
        persistent_data_size=int(args.persistent_data_size),
        scratch_mem_size_req=int(args.scratch_mem_size_req)
    )

    # Write the model to file
    generate_packed_model_file(args.path_out_model, model_data)


if __name__ == '__main__':
    main()
