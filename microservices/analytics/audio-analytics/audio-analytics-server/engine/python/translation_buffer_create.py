# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

import sys
import struct
from argparse import ArgumentParser
import sys
import math
if sys.version_info.major == 3:
    from functools import reduce

align_num = 256
def align_n(x, n=8):
    assert type(x) == int, '  input not of type int in align_n'
    assert type(n) == int, '  align not of type int in align_n'
    if n == 0:
        y = x
    else:
        y = int(math.ceil(math.ceil(float(x) / float(n)) * float(n)))
    return y

def build_structure_opus(opus_encoder_model_org_size,
            opus_encoder_model_pad_size, opus_decoder_model_org_size,
            opus_decoder_model_pad_size, tokenizer_autogen_org_size,
            tokenizer_autogen_w_pad_size, tokenizer_autogen_lookups_org_size,
            tokenizer_autogen_lookups_w_pad_size, decode_lookup_str_w_pad_size, scratch_mem_size,
            qnn_version_major, qnn_version_minor, qnn_version_patch,
            model_version_major, model_version_minor,
            model_lang, enc_model_max_seq_len, dec_model_max_seq_len,
            rep_penalty,
            align_num=align_num,):

    body = bytes()

    opus_max_translation_char_size = 1024
    opus_max_out_tokens = 65536
    opus_tokenizer_struct_size = 248
    opus_tokenstream_struct_size = 2129924
    split_sentence_struct_size = 10244
    opus_max_sentence_len = 256
    opus_desired_sentence_len = 128
    opus_max_num_suppress_logits = 6
    header_size = 16
    repeat_count_threshold = 10
    dsp_kpps = 100000
    so_filename_size = 20
    #Index of "<pad>" token in vocab.json: e.g., "<pad>": 65000. Can be found in vocab.json of the corresponding model repo
    opus_start_of_sequence = 65000
    #Index of "</s>" token in vocab.json: e.g., "</s>": 0
    opus_end_of_sequence = 0

    # Define backend SO filenames
    backend_file_name = "libQnnHtpV73.so"
    system_file_name = "libQnnSystem.so"

    # Make sure the strings are within the defined size
    backend_file_name = backend_file_name[:so_filename_size].ljust(so_filename_size, '\0')
    system_file_name = system_file_name[:so_filename_size].ljust(so_filename_size, '\0')

    #========================================
    # uint32_t opus_encoder_model_org_size;
    # uint32_t opus_encoder_model_pad_size;
    body += struct.pack('II', opus_encoder_model_org_size, opus_encoder_model_pad_size)

    # uint32_t opus_decoder_model_org_size;
    # uint32_t opus_decoder_model_pad_size;
    body += struct.pack('II', opus_decoder_model_org_size, opus_decoder_model_pad_size)

    # uint32_t tokenizer_autogen_org_size;
    # uint32_t tokenizer_autogen_w_pad_size;
    body += struct.pack('II', tokenizer_autogen_org_size, tokenizer_autogen_w_pad_size)

    # uint32_t tokenizer_autogen_lookups_org_size;
    # uint32_t tokenizer_autogen_lookups_w_pad_size;
    body += struct.pack('II', tokenizer_autogen_lookups_org_size, tokenizer_autogen_lookups_w_pad_size)

    # uint32_t decode_decode_lookup_str_w_pad_size
    body += struct.pack('I', decode_lookup_str_w_pad_size)

    #========================================
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

    # uint32_t backend_type;
    body += struct.pack('I', 1)

    # uint32_t opus_KPPS;
    body += struct.pack('I', dsp_kpps)

    # uint32_t htp_power_cfg_profilemode;
    body += struct.pack('I', 10)

    # int8_t reserved_0[4];
    body += struct.pack('b' * 4, *[0] * 4)

    #========================================
    # char backend_file_name[so_filename_size];
    body += struct.pack(f'{so_filename_size}s', backend_file_name.encode('utf-8'))

    # char system_file_name[so_filename_size];
    body += struct.pack(f'{so_filename_size}s', system_file_name.encode('utf-8'))

    #========================================
    # uint32_t input_language_code;
    if "en" == model_lang:
        body += struct.pack('I', 0)
    elif "zh" == model_lang:
        body += struct.pack('I', 1)
    elif "es" == model_lang:
        body += struct.pack('I', 3)

    # uint32_t opus_num_valid_out_tokens;
    body += struct.pack('I', 0)

    # uint32_t opus_num_layers;
    # uint32_t opus_num_heads;
    body += struct.pack('II', 6, 8)

    # uint32_t opus_max_seq_len_enc;
    # uint32_t opus_max_seq_len_dec;
    body += struct.pack('II', enc_model_max_seq_len, dec_model_max_seq_len)

    # uint32_t opus_dec_tensor_col_size;
    body += struct.pack('I', 64)

    # int32_t opus_start_of_sequence;
    # int32_t opus_end_of_sequence;
    body += struct.pack('ii', opus_start_of_sequence, opus_end_of_sequence)

    # uint32_t opus_is_final;
    body += struct.pack('I', 0)

    # Suppress the token ID '<pad>' (typically the last token in vocab.json, also representing start of sequence. e.g., "<pad>": 65000 in en<->es/zh model).
    # A value of -1 means "do not suppress" for that position.
    # This setup allows future extension: replace -1 with specific token IDs to suppress additional logits.

    # int32_t suppress_logit_id[OPUS_MAX_NUM_SUPPRESS_LOGITS];
    body += struct.pack('i' * opus_max_num_suppress_logits, opus_start_of_sequence, -1, -1, -1, -1, -1)

    # uint32_t repeat_count_threshold;        # To reduce hallucination, discard output tokens if repeat count is higher than repeat_count_threshold
    body += struct.pack('I', repeat_count_threshold)

    # int8_t reserved_1[4];
    body += struct.pack('b' * 4, *[0] * 4)

    #========================================
    # uint32_t input_text_size;
    body += struct.pack('I', 0)

    # uint32_t output_text_size;
    body += struct.pack('I', 0)

    # uint8_t input_text[HELSINKI_MAX_TRANSLATION_CHAR_SIZE];
    body += struct.pack('b' * opus_max_translation_char_size, *[0] * opus_max_translation_char_size)

    # uint8_t output_text[HELSINKI_MAX_TRANSLATION_CHAR_SIZE];
    body += struct.pack('b' * opus_max_translation_char_size, *[0] * opus_max_translation_char_size)

    # int32_t opus_out_tokens[OPUS_MAX_OUT_TOKENS];
    body += struct.pack('i' * opus_max_out_tokens, *[0] * opus_max_out_tokens)

    # float repetition_penalty;
    body += struct.pack('f', rep_penalty)

    # float repetition_penalty_decay;
    body += struct.pack('f', 0.8)

    #========================================
    # uint8_t *encodeTable_ptr;
    body += struct.pack('b' * 8, *[0] * 8)
    # uint8_t *decodeTable_ptr;
    body += struct.pack('b' * 8, *[0] * 8)
    # uint8_t *decodeTokenStr_ptr;
    body += struct.pack('b' * 8, *[0] * 8)

    #========================================
    # struct Tokenizer opusTokenizer;
    body += struct.pack('b' * opus_tokenizer_struct_size, *[0] * opus_tokenizer_struct_size)

    # struct TokenStream preTokens;
    body += struct.pack('b' * opus_tokenstream_struct_size, *[0] * opus_tokenstream_struct_size)

    # struct TokenStream outTokens;
    body += struct.pack('b' * opus_tokenstream_struct_size, *[0] * opus_tokenstream_struct_size)

    #========================================
    # split_sentence split_sentence;
    body += struct.pack('b' * split_sentence_struct_size, *[0] * split_sentence_struct_size)

    # uint32_t current_inference_sentence;
    body += struct.pack('I', 0)

    # uint32_t opus_max_sentence_len;
    # uint32_t opus_desired_sentence_len;
    body += struct.pack('II', opus_max_sentence_len, opus_desired_sentence_len)

    #========================================
    # opus_config_t  opus_config;
    body += struct.pack('i' * 4, *[0] * 4)

    #========================================
    # uint64_t scratch_mem_size;
    body += struct.pack('Q', scratch_mem_size)

    # uint64_t scratch_mem_offset;
    body += struct.pack('Q', 0)

    # uint8_t* scratch_mem_ptr;
    body += struct.pack('b' * 8, *[0] * 8)

    #========================================
    # uint32_t struct_size;         - size of opus_struct
    # uint32_t struct_w_pad_size;   - size of opus_struct + padding

    struct_size = len(body) + 8     # extra 8 bytes accounting for struct_size & struct_w_pad_size
    struct_w_pad_size = align_n(struct_size + header_size, align_num) - header_size # (tts structure + padding + header) needs to be 256 byte aligned
    pad_size = struct_w_pad_size - struct_size

    # padding size after struct for alignment
    body += struct.pack('b' * pad_size, *[0] * pad_size)

    # Add struct_size and struct_w_pad_size before body
    body = struct.pack('II', struct_size, struct_w_pad_size) + body

    print('STRUCTURE ORG SIZE = ', struct_size)
    print('STRUCTURE PAD SIZE = ', pad_size)
    print('STRUCTURE WITH PAD SIZE = ', struct_w_pad_size)

    return body

def write_model(output_file, start_magic, end_magic, version_major, version_minor, model_body):
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

    with open(output_file, 'wb') as f:
        f.write(model_header)
        print('HEADER_SIZE = ', f.tell())
        f.write(model_body)
        f.write(model_footer)

# Allocate persistent or scratch mem for qnn models
def allocate_mem(mem_size_req):
    # Allocate 10% extra memory for persistent/scratch as memory requirements can change with EAI version change
    extra_mem_size_factor = 0.1
    mem_size = align_n(round(mem_size_req*(1 + extra_mem_size_factor)), align_num)
    mem_byte = bytes()
    mem_byte += struct.pack('b' * mem_size, *[0] * mem_size)
    return(mem_byte, mem_size)

def pack_model_to_buffer(start_magic, end_magic, version_major, version_minor, model_body: bytes) -> bytes:
    assert len(start_magic) == 4 and len(end_magic) == 8
    type_char = 'c' if sys.version_info.major == 2 else 'b'

    start_magic_b = start_magic.encode('ascii')
    end_magic_b   = end_magic.encode('ascii')

    header = b""
    header += struct.pack(type_char * 4, *start_magic_b)
    header += struct.pack('hh', version_major, version_minor)

    footer = struct.pack(type_char * 8, *end_magic_b)

    size = len(header) + 8 + len(model_body) + len(footer)
    header += struct.pack('II', size, 0)  # keep 8-byte align

    return header + model_body + footer


def generate_model_blob_from_bytes(
    opus_encoder_model: bytes,
    opus_decoder_model: bytes,
    tokenizer_autogen: bytes,
    tokenizer_autogen_lookups: bytes,
    *,
    qnn_version_major: int, qnn_version_minor: int, qnn_version_patch: int,
    arch: int,
    enc_model_max_seq_len: int, dec_model_max_seq_len: int,
    rep_penalty: float,
    model_lang: str,
    scratch_mem_size_req: int,
    model_version_major: int = 1,
    model_version_minor: int = 0,
)->bytes:
    opus_encoder_model_org_size = len(opus_encoder_model)
    opus_encoder_model_pad_size = align_n(opus_encoder_model_org_size, align_num) - opus_encoder_model_org_size
    opus_encoder_model_size = opus_encoder_model_org_size + opus_encoder_model_pad_size

    # OPUS Encoder Model 256 byte aligned (so that if there is any subsequent qnn model is 256 byte aligned)
    if (opus_encoder_model_org_size != opus_encoder_model_size):
        opus_encoder_model += struct.pack('b' * opus_encoder_model_pad_size, *[0] * opus_encoder_model_pad_size)
    print("OPUS ENCODER MODEL SIZE = ", opus_encoder_model_size)

    # OPUS Decoder Model network

    opus_decoder_model_org_size = len(opus_decoder_model)
    opus_decoder_model_pad_size = align_n(opus_decoder_model_org_size, align_num) - opus_decoder_model_org_size
    opus_decoder_model_size = opus_decoder_model_org_size + opus_decoder_model_pad_size

    # OPUS Decoder Model 256 byte aligned (so that if there is any subsequent qnn model is 256 byte aligned)
    if (opus_decoder_model_org_size != opus_decoder_model_size):
        opus_decoder_model += struct.pack('b' * opus_decoder_model_pad_size, *[0] * opus_decoder_model_pad_size)
    print("OPUS DECODER MODEL SIZE = ", opus_decoder_model_size)

    # Tokenizer Autogen


    tokenizer_autogen_org_size = len(tokenizer_autogen)
    tokenizer_autogen_pad_size = align_n(tokenizer_autogen_org_size, align_num) - tokenizer_autogen_org_size
    tokenizer_autogen_size = tokenizer_autogen_org_size + tokenizer_autogen_pad_size

    # Tokenizer Autogen 256 byte aligned (so that if there is any subsequent qnn model is 256 byte aligned)
    if (tokenizer_autogen_org_size != tokenizer_autogen_size):
        tokenizer_autogen += struct.pack('b' * tokenizer_autogen_pad_size, *[0] * tokenizer_autogen_pad_size)
    print("TOKENIZER AUTOGEN SIZE = ", tokenizer_autogen_size)

    # Tokenizer Autogen Lookups

    # read lookup size from tokenizer lookup binary
    lookups_size = struct.unpack('II', tokenizer_autogen_lookups[:8])[1]
    print("Size of Lookup in TOKENIZER AUTOGEN LOOKUPS = ", lookups_size)

    tokenizer_autogen_lookups_org_size = len(tokenizer_autogen_lookups)
    tokenizer_autogen_lookups_pad_size = align_n(tokenizer_autogen_lookups_org_size, align_num) - tokenizer_autogen_lookups_org_size
    tokenizer_autogen_lookups_size = tokenizer_autogen_lookups_org_size + tokenizer_autogen_lookups_pad_size

    # Tokenizer Autogen Lookups 256 byte aligned (so that if there is any subsequent qnn model is 256 byte aligned)
    if (tokenizer_autogen_lookups_org_size != tokenizer_autogen_lookups_size):
        tokenizer_autogen_lookups += struct.pack('b' * tokenizer_autogen_lookups_pad_size, *[0] * tokenizer_autogen_lookups_pad_size)
    print("TOKENIZER AUTOGEN LOOKUPS SIZE = ", tokenizer_autogen_lookups_size)

    # Allocate memory for decode token string (with padding)
    lookups_size_w_pad_size = align_n(lookups_size, align_num)
    decode_token_str_mem = struct.pack('b' * lookups_size_w_pad_size, *[0] * lookups_size_w_pad_size)

    # Allocate scratch mem
    scratch_mem_size_req = int(scratch_mem_size_req)
    scratch_memory , scratch_mem_size = allocate_mem(scratch_mem_size_req)
    print("SCRATCH MEM SIZE = ", scratch_mem_size)

    structure_t2t = build_structure_opus(opus_encoder_model_org_size,
                    opus_encoder_model_size, opus_decoder_model_org_size,
                    opus_decoder_model_size, tokenizer_autogen_org_size,
                    tokenizer_autogen_size, tokenizer_autogen_lookups_org_size,
                    tokenizer_autogen_lookups_size, lookups_size_w_pad_size, scratch_mem_size,
                    qnn_version_major=qnn_version_major,
                    qnn_version_minor=qnn_version_minor,
                    qnn_version_patch=qnn_version_patch,
                    model_version_major=model_version_major,
                    model_version_minor=model_version_minor,
                    model_lang=model_lang,
                    enc_model_max_seq_len=int(enc_model_max_seq_len),
                    dec_model_max_seq_len=int(dec_model_max_seq_len),
                    rep_penalty=float(rep_penalty),
                    align_num=align_num,)
    
    full_body = (
        structure_t2t
        + opus_encoder_model
        + opus_decoder_model
        + tokenizer_autogen
        + tokenizer_autogen_lookups
        + decode_token_str_mem
        + scratch_memory
    )

    # write_model("./opus_en_to_zh.64_bit.qnn_v2.33.0.qnn", 'SUPO', 'OPUSMODE', model_version_major, model_version_minor,
    #         structure_t2t+opus_encoder_model+opus_decoder_model+tokenizer_autogen+
    #         tokenizer_autogen_lookups+decode_token_str_mem+scratch_memory)

    return pack_model_to_buffer('SUPO', 'OPUSMODE', model_version_major, model_version_minor, full_body)

