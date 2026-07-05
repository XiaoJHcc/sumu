# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
from __future__ import annotations

"""Zero-copy wrap of a raw CUDA device pointer (as returned by native `Player.
get_cuda_nv12_by_frame()`, see native/src/player.cpp) into a `torch.Tensor`, via a
hand-built DLPack capsule constructed in pure Python with `ctypes` -- no native extension
code, no `__cuda_array_interface__` (torch does not natively consume that protocol; only
cupy/numba do).

Why this exists / why not simpler options:
- The native bridge (`get_cuda_nv12_by_frame`) deliberately returns a raw `dev_ptr` (a plain
  uint64 `CUdeviceptr`) rather than a pybind11-wrapped tensor object or a hand-rolled DLPack
  capsule built in C++ -- keeping the native layer's surface small/boring for validated code
  (spikes/spike3_nv12_interop's own recommendation: avoid `__cuda_array_interface__`/DLPack
  plumbing in the native layer entirely, see docs/spike3_nv12_interop.md's API
  recommendation #1). Something on the Python side has to turn that raw pointer into a
  torch.Tensor without copying it -- that's what this module does.
- This exact `ctypes`-based `PyCapsule_New` + `DLManagedTensor` construction was empirically
  validated (on this project's target machine/torch build) before being written here: a
  throwaway probe script confirmed `tensor.data_ptr() == original dev_ptr` (true zero-copy,
  not a hidden copy), that mutating the underlying CUDA memory through one view is visible
  through the other, and that the `deleter` callback fires when the wrapper is released.
"""

import ctypes

import torch

# ---- DLPack ABI structs (matching dlpack.h's DLManagedTensor layout; the legacy,
# non-versioned form, which is what torch.from_dlpack / the __dlpack__ protocol on this torch
# version consumes via a "dltensor"-named capsule). ------------------------------------------

_DLPACK_CUDA_DEVICE_TYPE = 2   # kDLCUDA
_DLPACK_UINT_CODE = 1          # kDLUInt


class _DLDevice(ctypes.Structure):
    _fields_ = [
        ("device_type", ctypes.c_int32),
        ("device_id", ctypes.c_int32),
    ]


class _DLDataType(ctypes.Structure):
    _fields_ = [
        ("code", ctypes.c_uint8),
        ("bits", ctypes.c_uint8),
        ("lanes", ctypes.c_uint16),
    ]


class _DLTensor(ctypes.Structure):
    _fields_ = [
        ("data", ctypes.c_void_p),
        ("device", _DLDevice),
        ("ndim", ctypes.c_int32),
        ("dtype", _DLDataType),
        ("shape", ctypes.POINTER(ctypes.c_int64)),
        ("strides", ctypes.POINTER(ctypes.c_int64)),
        ("byte_offset", ctypes.c_uint64),
    ]


_DLManagedTensorDeleter = ctypes.CFUNCTYPE(None, ctypes.c_void_p)


class _DLManagedTensor(ctypes.Structure):
    _fields_ = [
        ("dl_tensor", _DLTensor),
        ("manager_ctx", ctypes.c_void_p),
        ("deleter", _DLManagedTensorDeleter),
    ]


_PyCapsule_New = ctypes.pythonapi.PyCapsule_New
_PyCapsule_New.restype = ctypes.py_object
_PyCapsule_New.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p]

# Keyed by id(holder) -- keeps the holder (and therefore its ctypes buffers + CFUNCTYPE
# deleter closure) alive for exactly as long as some capsule/tensor might still call back into
# it. Popped by the deleter itself when DLPack's consumer (torch) is done with the tensor.
_ALIVE_HOLDERS: dict[int, "_CudaBufferDlpackHolder"] = {}


class _CudaBufferDlpackHolder:
    """Owns the ctypes `DLManagedTensor` + backing shape array for one DLPack export of a
    native-owned CUDA buffer. Does NOT own the CUDA memory itself -- that belongs to the
    native `Player` (a single persistent buffer reused across calls, see
    `native/src/player.cpp`'s `get_cuda_nv12_by_frame`) -- so the deleter here has nothing to
    `cuMemFree`; its only job is to let this Python-side bookkeeping object (and the ctypes
    struct/callback it holds) be garbage collected once the consumer (torch) is done."""

    def __init__(self, dev_ptr: int, shape: tuple[int, ...], device_index: int = 0):
        self._shape_arr = (ctypes.c_int64 * len(shape))(*shape)
        self._tensor = _DLManagedTensor()
        dl_tensor = self._tensor.dl_tensor
        dl_tensor.data = ctypes.c_void_p(dev_ptr)
        dl_tensor.device = _DLDevice(_DLPACK_CUDA_DEVICE_TYPE, device_index)
        dl_tensor.ndim = len(shape)
        dl_tensor.dtype = _DLDataType(_DLPACK_UINT_CODE, 8, 1)  # uint8
        dl_tensor.shape = ctypes.cast(self._shape_arr, ctypes.POINTER(ctypes.c_int64))
        dl_tensor.strides = None  # NULL strides == C-contiguous; valid since the native
        # buffer is tightly packed (pitch_bytes == width, enforced by the caller below).
        dl_tensor.byte_offset = 0
        self._tensor.manager_ctx = None
        self._deleter_cb = _DLManagedTensorDeleter(self._on_delete)
        self._tensor.deleter = self._deleter_cb
        self._device_index = device_index

    def _on_delete(self, _managed_tensor_ptr):
        _ALIVE_HOLDERS.pop(id(self), None)

    def __dlpack__(self, stream=None):
        _ALIVE_HOLDERS[id(self)] = self  # keep self (and the ctypes buffers) alive
        return _PyCapsule_New(ctypes.byref(self._tensor), b"dltensor", None)

    def __dlpack_device__(self):
        return (_DLPACK_CUDA_DEVICE_TYPE, self._device_index)


def wrap_nv12_cuda_buffer_as_tensor(dev_ptr: int, width: int, height: int, pitch_bytes: int,
                                     device_index: int = 0) -> "torch.Tensor":
    """Zero-copy-wrap the raw CUDA buffer described by native `Player.get_cuda_nv12_by_frame()`
    (`dev_ptr`, `width`, `height`, `pitch_bytes`) into a `torch.Tensor` of shape
    `(height * 3 // 2, width)`, dtype `uint8`, CUDA-resident -- the exact stacked NV12 layout
    `_nv12_to_bgr_hwc_gpu` (python/sumu/ai/utils/video_utils.py) expects as input.

    Only tightly-packed buffers are supported (`pitch_bytes == width`) -- true for the native
    bridge's own persistent destination buffer (see `create_ai_input_bridge()` in
    native/src/player.cpp), which is deliberately allocated that way so this wrap can describe
    it with NULL DLPack strides (a genuinely contiguous tensor, no stride-hack needed).

    CAUTION (mirrors the native side's own documented caveat): the underlying CUDA memory is a
    SINGLE buffer owned by the native `Player`, reused on every `get_cuda_nv12_by_frame()`
    call -- this wrap does not copy it. The returned tensor is only valid until the next call
    to `get_cuda_nv12_by_frame()` on the same `Player`; consume it (e.g. via
    `_nv12_to_bgr_hwc_gpu`) or `.clone()` it before pulling the next frame."""
    if pitch_bytes != width:
        raise ValueError(
            f"wrap_nv12_cuda_buffer_as_tensor: only tightly-packed buffers are supported "
            f"(pitch_bytes={pitch_bytes} != width={width}); the native bridge is expected to "
            f"always hand back a tightly-packed buffer -- this indicates a native/Python "
            f"contract mismatch."
        )
    nv12_height = height * 3 // 2
    holder = _CudaBufferDlpackHolder(dev_ptr, shape=(nv12_height, width), device_index=device_index)
    return torch.from_dlpack(holder)
