# SPDX-FileCopyrightText: Lada Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Ported from jasna (github.com/Kruk2/jasna), a lada fork that accelerates
# BasicVSR++ with TensorRT. Only the BasicVSR++-relevant helpers are kept;
# the ONNX/YOLO/unet paths from the source are intentionally omitted.
from __future__ import annotations

import logging
import sys

import tensorrt as trt
import torch

_TRT_LOGGER = trt.Logger(trt.Logger.ERROR)


def get_trt_logger() -> trt.ILogger:
    torchtrt = sys.modules.get("torch_tensorrt")
    if torchtrt is not None:
        return torchtrt.logging.TRT_LOGGER
    return _TRT_LOGGER


def _engine_io_names(engine: trt.ICudaEngine) -> tuple[list[str], list[str]]:
    input_names: list[str] = []
    output_names: list[str] = []

    if hasattr(engine, "num_io_tensors"):
        for i in range(engine.num_io_tensors):
            name = engine.get_tensor_name(i)
            if engine.get_tensor_mode(name) == trt.TensorIOMode.INPUT:
                input_names.append(name)
            else:
                output_names.append(name)
        return input_names, output_names

    for i in range(engine.num_bindings):
        name = engine.get_binding_name(i)
        if engine.binding_is_input(i):
            input_names.append(name)
        else:
            output_names.append(name)
    return input_names, output_names


def _trt_dtype_to_torch(trt_dtype: trt.DataType) -> torch.dtype:
    if trt_dtype == trt.DataType.FLOAT:
        return torch.float32
    if trt_dtype == trt.DataType.HALF:
        return torch.float16
    if trt_dtype == trt.DataType.INT8:
        return torch.int8
    if trt_dtype == trt.DataType.INT32:
        return torch.int32
    if trt_dtype == trt.DataType.BOOL:
        return torch.bool
    raise ValueError(f"Unsupported TensorRT dtype: {trt_dtype}")
