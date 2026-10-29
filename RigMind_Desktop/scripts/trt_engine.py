"""FP16 TensorRT engines, built on first use and reused after that.

Engines are never shipped: an engine only loads on the GPU model and TensorRT version it was built on.
The installer ships the .onnx file; this module builds the engine on the target machine.
Cache key: GPU name + TensorRT version + SHA-256 of the ONNX file, so a model or driver change rebuilds it.

Usage: trt_engine.py <model.onnx> <cache_dir>   -> prints the engine path
"""

import hashlib
import os
import re
import subprocess
import sys

import tensorrt as trt


def gpu_name():
    try:
        out = subprocess.run(["nvidia-smi", "--query-gpu=name", "--format=csv,noheader"],
                             capture_output=True, text=True, timeout=10, check=True).stdout
        return re.sub(r"[^A-Za-z0-9]+", "_", out.strip().splitlines()[0]).strip("_") or "unknown_gpu"
    except (OSError, subprocess.SubprocessError, IndexError):
        return "unknown_gpu"


def engine_path(onnx_path, cache_dir):
    with open(onnx_path, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()[:12]
    name = os.path.splitext(os.path.basename(onnx_path))[0]
    key = f"{gpu_name()}-trt{trt.__version__}-{digest}"
    return os.path.join(cache_dir, f"{name}.{key}.fp16.engine")


def build(onnx_path, out_path):
    logger = trt.Logger(trt.Logger.WARNING)
    builder = trt.Builder(logger)
    network = builder.create_network(0)  # explicit batch is the only mode in TensorRT 10+
    parser = trt.OnnxParser(network, logger)
    with open(onnx_path, "rb") as f:
        if not parser.parse(f.read()):
            errors = "; ".join(str(parser.get_error(i)) for i in range(parser.num_errors))
            raise RuntimeError(f"ONNX parse failed: {errors}")
    config = builder.create_builder_config()
    config.set_flag(trt.BuilderFlag.FP16)  # FP16 only, as the target machines require
    config.set_memory_pool_limit(trt.MemoryPoolType.WORKSPACE, 1 << 30)
    plan = builder.build_serialized_network(network, config)
    if plan is None:
        raise RuntimeError(f"engine build failed for {onnx_path}")
    tmp = out_path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(plan)
    os.replace(tmp, out_path)  # never leaves a half-written engine in the cache


def ensure_engine(onnx_path, cache_dir):
    """Returns the cached engine path, building it first if this machine has none."""
    os.makedirs(cache_dir, exist_ok=True)
    path = engine_path(onnx_path, cache_dir)
    if not os.path.isfile(path):
        build(onnx_path, path)
    return path


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("usage: trt_engine.py <model.onnx> <cache_dir>", file=sys.stderr)
        sys.exit(2)
    print(ensure_engine(sys.argv[1], sys.argv[2]))
