# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

import sys
import struct
import math

align_num = 256

def align_n(x, n=8):
    assert type(x) == int, '  input not of type int in align_n'
    assert type(n) == int, '  align not of type int in align_n'
    if n == 0:
        return x
    return int(math.ceil(math.ceil(float(x) / float(n)) * float(n)))

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

    parts = []

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
    opus_start_of_sequence = 65000
    opus_end_of_sequence = 0

    backend_file_name = "libQnnHtpV73.so"
    system_file_name = "libQnnSystem.so"

    backend_file_name = backend_file_name[:so_filename_size].ljust(so_filename_size, '\0')
    system_file_name = system_file_name[:so_filename_size].ljust(so_filename_size, '\0')

    parts.append(struct.pack('II', opus_encoder_model_org_size, opus_encoder_model_pad_size))
    parts.append(struct.pack('II', opus_decoder_model_org_size, opus_decoder_model_pad_size))
    parts.append(struct.pack('II', tokenizer_autogen_org_size, tokenizer_autogen_w_pad_size))
    parts.append(struct.pack('II', tokenizer_autogen_lookups_org_size, tokenizer_autogen_lookups_w_pad_size))
    parts.append(struct.pack('I', decode_lookup_str_w_pad_size))

    parts.append(struct.pack('i', qnn_version_major))
    parts.append(struct.pack('i', qnn_version_minor))
    parts.append(struct.pack('i', qnn_version_patch))
    parts.append(struct.pack('i', model_version_major))
    parts.append(struct.pack('i', model_version_minor))
    parts.append(struct.pack('I', 1))        # backend_type
    parts.append(struct.pack('I', dsp_kpps)) # opus_KPPS
    parts.append(struct.pack('I', 10))       # htp_power_cfg_profilemode
    parts.append(bytes(4))                   # reserved_0[4]

    parts.append(struct.pack(f'{so_filename_size}s', backend_file_name.encode('utf-8')))
    parts.append(struct.pack(f'{so_filename_size}s', system_file_name.encode('utf-8')))

    if "en" == model_lang:
        parts.append(struct.pack('I', 0))
    elif "zh" == model_lang:
        parts.append(struct.pack('I', 1))
    elif "es" == model_lang:
        parts.append(struct.pack('I', 3))

    parts.append(struct.pack('I', 0))                              # opus_num_valid_out_tokens
    parts.append(struct.pack('II', 6, 8))                          # opus_num_layers, opus_num_heads
    parts.append(struct.pack('II', enc_model_max_seq_len, dec_model_max_seq_len))
    parts.append(struct.pack('I', 64))                             # opus_dec_tensor_col_size
    parts.append(struct.pack('ii', opus_start_of_sequence, opus_end_of_sequence))
    parts.append(struct.pack('I', 0))                              # opus_is_final
    parts.append(struct.pack('i' * opus_max_num_suppress_logits,
                             opus_start_of_sequence, -1, -1, -1, -1, -1))
    parts.append(struct.pack('I', repeat_count_threshold))
    parts.append(bytes(4))                                         # reserved_1[4]

    parts.append(struct.pack('I', 0))                              # input_text_size
    parts.append(struct.pack('I', 0))                              # output_text_size
    parts.append(bytes(opus_max_translation_char_size))            # input_text
    parts.append(bytes(opus_max_translation_char_size))            # output_text
    parts.append(bytes(opus_max_out_tokens * 4))                   # opus_out_tokens (int32 array)
    parts.append(struct.pack('f', rep_penalty))                    # repetition_penalty
    parts.append(struct.pack('f', 0.8))                            # repetition_penalty_decay

    parts.append(bytes(8))                                         # encodeTable_ptr
    parts.append(bytes(8))                                         # decodeTable_ptr
    parts.append(bytes(8))                                         # decodeTokenStr_ptr

    parts.append(bytes(opus_tokenizer_struct_size))                # opusTokenizer
    parts.append(bytes(opus_tokenstream_struct_size))              # preTokens
    parts.append(bytes(opus_tokenstream_struct_size))              # outTokens

    parts.append(bytes(split_sentence_struct_size))                # split_sentence
    parts.append(struct.pack('I', 0))                              # current_inference_sentence
    parts.append(struct.pack('II', opus_max_sentence_len, opus_desired_sentence_len))

    parts.append(bytes(16))                                        # opus_config_t (4 x int32)

    parts.append(struct.pack('Q', scratch_mem_size))               # scratch_mem_size
    parts.append(struct.pack('Q', 0))                              # scratch_mem_offset
    parts.append(bytes(8))                                         # scratch_mem_ptr

    body = b"".join(parts)

    struct_size = len(body) + 8
    struct_w_pad_size = align_n(struct_size + header_size, align_num) - header_size
    pad_size = struct_w_pad_size - struct_size

    body = struct.pack('II', struct_size, struct_w_pad_size) + body + bytes(pad_size)

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

    type_char = 'c' if sys.version_info.major == 2 else 'b'

    model_footer += struct.pack(type_char * 8, *end_magic)

    model_header += struct.pack(type_char * 4, *start_magic)
    model_header += struct.pack('hh', version_major, version_minor)
    size = len(model_header) + 8 + len(model_body) + len(model_footer)
    model_header += struct.pack('II', size, 0)

    with open(output_file, 'wb') as f:
        f.write(model_header)
        print('HEADER_SIZE = ', f.tell())
        f.write(model_body)
        f.write(model_footer)

def allocate_mem(mem_size_req):
    extra_mem_size_factor = 0.1
    mem_size = align_n(round(mem_size_req*(1 + extra_mem_size_factor)), align_num)
    return bytes(mem_size), mem_size

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
    header += struct.pack('II', size, 0)

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
    def _pad(data: bytes) -> tuple[bytes, int, int]:
        org_size = len(data)
        pad_size = align_n(org_size, align_num) - org_size
        return (data + bytes(pad_size) if pad_size else data), org_size, pad_size

    opus_encoder_model, enc_org, enc_pad   = _pad(opus_encoder_model)
    opus_decoder_model, dec_org, dec_pad   = _pad(opus_decoder_model)
    tokenizer_autogen,  tok_org, tok_pad   = _pad(tokenizer_autogen)
    tokenizer_autogen_lookups, lkp_org, lkp_pad = _pad(tokenizer_autogen_lookups)

    print("OPUS ENCODER MODEL SIZE = ", enc_org + enc_pad)
    print("OPUS DECODER MODEL SIZE = ", dec_org + dec_pad)
    print("TOKENIZER AUTOGEN SIZE = ", tok_org + tok_pad)

    lookups_size = struct.unpack('II', tokenizer_autogen_lookups[:8])[1]
    print("Size of Lookup in TOKENIZER AUTOGEN LOOKUPS = ", lookups_size)
    print("TOKENIZER AUTOGEN LOOKUPS SIZE = ", lkp_org + lkp_pad)

    lookups_size_w_pad_size = align_n(lookups_size, align_num)
    decode_token_str_mem = bytes(lookups_size_w_pad_size)

    scratch_mem_size_req = int(scratch_mem_size_req)
    scratch_memory, scratch_mem_size = allocate_mem(scratch_mem_size_req)
    print("SCRATCH MEM SIZE = ", scratch_mem_size)

    structure_t2t = build_structure_opus(enc_org,
                    enc_org + enc_pad, dec_org,
                    dec_org + dec_pad, tok_org,
                    tok_org + tok_pad, lkp_org,
                    lkp_org + lkp_pad, lookups_size_w_pad_size, scratch_mem_size,
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

    full_body = b"".join([
        structure_t2t,
        opus_encoder_model,
        opus_decoder_model,
        tokenizer_autogen,
        tokenizer_autogen_lookups,
        decode_token_str_mem,
        scratch_memory,
    ])

    return pack_model_to_buffer('SUPO', 'OPUSMODE', model_version_major, model_version_minor, full_body)
