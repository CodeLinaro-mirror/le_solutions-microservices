# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import builtins
import ctypes
from typing import Optional

try:
    from .qnn_types import Qnn_Tensor_t  # adjust import if needed
    from .io_tensor import tv_view
except Exception:
    from openapi_server.interface.qnn_runtime.qnn_types import Qnn_Tensor_t  # type: ignore
    from openapi_server.interface.qnn_runtime.io_tensor import tv_view  # type: ignore


def print_to_log(fn):
    def wrapper(*args, **kwargs):
        # print only to file
        with open("/tmp/log_qnn-cli-python.txt", "a", buffering=1) as f:
            fn(*args, **kwargs, file=f)

    return wrapper


@print_to_log
def print(*args, **kwargs):
    builtins.print(*args, **kwargs)


def _c_void_p_value(ptr) -> int:
    """Return integer address from a c_void_p-compatible field (or 0)."""
    try:
        return int(ctypes.cast(ptr, ctypes.c_void_p).value or 0)
    except Exception:
        return 0


def _safe_name(c_char_p) -> str:
    try:
        return c_char_p.decode() if c_char_p else ""
    except Exception:
        return ""


def dump_tensor(t: Optional[Qnn_Tensor_t], idx: int, kind: str = "INFO",
                logger: Optional[object] = None) -> None:
    """
    Python port of dumpTensor(const Qnn_Tensor_t* t, ...).

    Parameters
    ----------
    t : Qnn_Tensor_t or None
        The tensor ctypes struct instance.
    idx : int
        Index to print (matches the C++ signature).
    kind : str
        A free-form tag (e.g., "INPUT", "OUTPUT").
    logger : logging.Logger, optional
        If provided, use logger.error(...) for the line; otherwise print(...).
    """
    log_err = (logger.error if logger else print)
    log_dbg = (logger.debug if logger else print)

    if t is None:
        log_dbg(f"[{kind}] tensor[{idx}] CORRUPTED (nullptr)")
        return

    try:
        # Pick a safe view into the union fields
        _ver, tv = tv_view(t)

        # name
        name = _safe_name(tv.name)

        # fields we want to display
        dtype = int(getattr(tv, "dataType", 0))
        rank = int(getattr(tv, "rank", 0))
        mem_type = int(getattr(tv, "memType", 0))

        # client buffer (pointer + size)
        buf_ptr = _c_void_p_value(tv.clientBuf.data)
        data_size = int(getattr(tv.clientBuf, "dataSize", 0))

        # Mirror the C++ formatting: dtype as 0x%04x, rank, memType, buf %p, size %u
        # Note: Python lacks %p; we'll print hex address with 0x prefix.
        msg = (f"[{kind}] tensor[{idx}] name='{name}' "
               f"dtype=0x{dtype:04x} rank={rank} memType={mem_type} "
               f"buf=0x{buf_ptr:016x} size={data_size}")
        # The C++ uses QNN_ERROR here; keep as error-level for visibility
        log_err(msg)

    except Exception as e:
        log_err(f"[{kind}] tensor[{idx}] <exception while dumping>: {e}")


def dump_tensors(tensors: Optional[ctypes.Array], count: int, kind: str = "INFO",
                 logger: Optional[object] = None) -> None:
    """
    Python port of dumpTensors(const Qnn_Tensor_t* tensors, ...).

    Parameters
    ----------
    tensors : ctypes array of Qnn_Tensor_t or None
        Typically (Qnn_Tensor_t * N)() or a list you constructed.
    count : int
        Number of entries to dump.
    kind : str
        Tag for the line prefix.
    logger : logging.Logger, optional
        If provided, use it; otherwise print.
    """
    log_dbg = (logger.debug if logger else print)

    if tensors is None:
        log_dbg(f"[{kind}] tensors pointer is nullptr")
        return

    # tensors can be a ctypes array, Python list, or other sequence
    # We’ll index up to 'count' while guarding length
    try:
        length = getattr(tensors, "_length_", None)
        if length is None:
            # Attempt len() for lists/tuples or ctypes arrays lacking _length_
            length = len(tensors)
    except Exception:
        length = count  # best effort

    n = min(int(count), int(length) if length is not None else int(count))

    for i in range(max(0, n)):
        try:
            dump_tensor(tensors[i], i, kind, logger=logger)
        except Exception as e:
            # Keep going even if one entry fails
            err = (logger.error if logger else print)
            err(f"[{kind}] tensors[{i}] <exception while dumping>: {e}")
