# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
from __future__ import annotations

"""Pure helper functions ported (照搬) from lada-realtime's `lada/utils/video_utils.py`.

Scope note: only the standalone pure functions are ported here -- NOT the `VideoReader`
class. Per `docs/porting_manifest.md` (项 6) and DESIGN.md I3/I7, sumu's production decode
path goes through the native `Player` (native/src/player.cpp, D3D11 hw decode -> CUDA
interop), never through PyAV/pynvc. `VideoReader` (lada's PyAV/pynvc decode wrapper) has no
place in that path and is intentionally left unported.
"""

import json
import subprocess
import sys
from fractions import Fraction

import cv2

from sumu.ai.utils import VideoMetadata

# NOTE: `torch` is intentionally not imported at module top-level, matching the convention
# established in sumu/ai/utils/__init__.py -- `_nv12_to_bgr_hwc_gpu` imports it lazily inside
# the function body so merely importing this module does not force CUDA/cuDNN init.


def _nv12_to_bgr_hwc_gpu(nv12, h: int, w: int, bt709: bool, full_range: bool):
    """Convert a CUDA-resident nv12 frame (shape (h*3//2, w) uint8, stacked-plane layout:
    rows [0,h) = luma, rows [h, h*3//2) = interleaved chroma) to a GPU BGR HWC uint8 tensor,
    matching libswscale's bgr24 output (MAE ~1 on real content -> AI-transparent).

    Ported from lada-realtime's `lada.utils.video_utils._nv12_to_bgr_hwc_gpu`, credited in
    full below, with one deliberate change to the function's *contract* (not its math):

    - lada's original takes a CPU numpy nv12 array and does `torch.from_numpy(nv12).to(
      torch_device, non_blocking=True)` internally, because PyAV cannot hand out the NVDEC
      surface zero-copy and a host round trip is unavoidable there (see lada's own docstring,
      reproduced below).
    - sumu's production input is different: the native `Player.get_cuda_nv12_by_frame()`
      bridge (native/src/player.cpp, validated by spikes/spike3_nv12_interop) already
      delivers a CUDA-resident NV12 buffer -- the whole point of that bridge is I3 (all-GPU,
      never touch main memory). Forcing a CPU numpy array through this function here would
      silently reintroduce the exact host round trip spike 3 eliminated. So this port takes
      `nv12` as an already-CUDA-resident torch.Tensor (uint8, shape (h*3//2, w)) and drops the
      `torch_device`/`torch.from_numpy(...).to(device)` upload step entirely -- everything
      else (the fp16 nearest-chroma-upsample BT.601/BT.709 + limited/full-range math) is
      unchanged from lada's original.

    This is the same math spike 3's local `spike3_driver.py` adaptation used (see
    docs/spike3_nv12_interop.md), now promoted into the shared production module instead of
    living as a spike-local copy. Validated there: MAE vs. independent PyAV CPU bgr24
    reference = 0.6994 (1080p) / 0.6215 (4K), both within lada's own "~1, AI-transparent"
    expectation for this formula.

    Caller-side pitfall this spike found and fixed (not a bug in this function itself, but a
    correction the *caller* must apply when deriving `full_range` from FFmpeg's
    `AVColorRange`): `AVCOL_RANGE_MPEG == 1` (limited/"tv"), `AVCOL_RANGE_JPEG == 2`
    (full/"pc") -- i.e. `full_range = (color_range == 2)`. lada's own source comments have
    this backwards; getting it wrong doesn't crash, it silently produces a plausible-but-wrong
    image with elevated MAE (~6-8 instead of ~0.6-0.7). See docs/spike3_nv12_interop.md
    "Second bug found" for the full writeup.

    Original lada docstring (kept for context): "Convert a CPU nv12 frame (shape (h*3//2, w)
    uint8, as PyAV's to_ndarray('nv12') returns for an NVDEC-decoded frame) to a GPU BGR HWC
    uint8 tensor, matching libswscale's bgr24 output (MAE ~1 on this content ->
    AI-transparent; the models were trained on swscale frames). [...] Chroma is
    nearest-upsampled (repeat_interleave); BT.601 (the swscale default for unspecified
    colourspace, which is what these files declare) vs BT.709 and limited/full range are
    picked from the frame tags."
    """
    import torch
    t = nv12
    # fp16 throughout: values are 0..255 so fp16's ~3-digit precision is ample, and it halves
    # the convert's GPU cost. This convert runs on the CUDA cores the AI (YOLO + BasicVSR++)
    # also needs, so every ms saved here is returned to AI throughput. Chroma is
    # nearest-upsampled (repeat_interleave, no bilinear interpolate kernel): chroma is already
    # 2x2-subsampled low-frequency data, so nearest is visually indistinguishable and stays
    # AI-transparent (MAE ~1 vs swscale bgr24).
    y = t[:h, :].to(torch.float16)
    uv = t[h:, :].reshape(h // 2, w // 2, 2)
    uv = uv.repeat_interleave(2, dim=0).repeat_interleave(2, dim=1).to(torch.float16)
    u = uv[:, :, 0]; v = uv[:, :, 1]
    kr, kb = (0.2126, 0.0722) if bt709 else (0.299, 0.114)
    kg = 1.0 - kr - kb
    if full_range:
        yv = y; uu = u - 128.0; vv = v - 128.0
    else:
        yv = (y - 16.0) * (255.0 / 219.0)
        uu = (u - 128.0) * (255.0 / 224.0)
        vv = (v - 128.0) * (255.0 / 224.0)
    r = yv + (2.0 * (1.0 - kr)) * vv
    b = yv + (2.0 * (1.0 - kb)) * uu
    g = yv - (2.0 * kr * (1.0 - kr) / kg) * vv - (2.0 * kb * (1.0 - kb) / kg) * uu
    return torch.stack([b, g, r], dim=2).clamp_(0, 255).to(torch.uint8)


def _get_subprocess_startup_info():
    """Local inline equivalent of lada's `os_utils.get_subprocess_startup_info()` -- sumu has
    not ported a standalone `os_utils.py` module (nothing else in sumu needs it yet), so this
    is kept as a private helper here rather than introducing a new module just for one call
    site. Suppresses the ffprobe console window flash on Windows."""
    if sys.platform != "win32":
        return None
    startup_info = subprocess.STARTUPINFO()
    startup_info.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    return startup_info


def get_video_meta_data(path: str) -> VideoMetadata:
    """Ported verbatim (照搬) from lada's `get_video_meta_data`. Probes a video file via
    ffprobe (falling back to OpenCV's frame-count probe when ffprobe's `nb_frames` is
    missing/zero, as some containers don't report it). `VideoMetadata` is imported from
    `sumu.ai.utils` (already ported there, same fields as lada's dataclass)."""
    cmd = ['ffprobe', '-v', 'quiet', '-print_format', 'json', '-select_streams', 'v', '-show_streams', '-show_format', path]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, startupinfo=_get_subprocess_startup_info())
    out, err = p.communicate()
    if p.returncode != 0:
        raise Exception(f"error running ffprobe: {err.strip()}. Code: {p.returncode}, cmd: {cmd}")
    json_output = json.loads(out)
    json_video_stream = json_output["streams"][0]
    json_video_format = json_output["format"]

    value = [int(num) for num in json_video_stream['avg_frame_rate'].split("/")]
    # Can be 0/0 for some files as ffprobe isn't always able to determine the number of frames.
    average_fps = value[0] / value[1] if len(value) == 2 and value[1] != 0 else value[0]

    value = [int(num) for num in json_video_stream['r_frame_rate'].split("/")]
    fps = value[0] / value[1] if len(value) == 2 else value[0]
    fps_exact = Fraction(value[0], value[1])

    value = [int(num) for num in json_video_stream['time_base'].split("/")]
    time_base = Fraction(value[0], value[1])

    frame_count = json_video_stream.get('nb_frames')
    if not frame_count:
        cap = cv2.VideoCapture(path)
        frame_count = cap.get(cv2.CAP_PROP_FRAME_COUNT)
        cap.release()
    frame_count = int(frame_count)

    start_pts = json_video_stream.get('start_pts')

    metadata = VideoMetadata(
        video_file=path,
        video_height=int(json_video_stream['height']),
        video_width=int(json_video_stream['width']),
        video_fps=fps,
        average_fps=average_fps,
        video_fps_exact=fps_exact,
        codec_name=json_video_stream['codec_name'],
        frames_count=frame_count,
        duration=float(json_video_stream.get('duration', json_video_format['duration'])),
        time_base=time_base,
        start_pts=start_pts,
    )
    return metadata


def offset_ns_to_frame_num(offset_ns, video_fps_exact):
    """Ported verbatim (照搬) from lada. Pure arithmetic, no dependency on VideoReader."""
    return int(Fraction(offset_ns, 1_000_000_000) * video_fps_exact)


def pts_to_frame_num(pts, time_base, video_fps_exact):
    """Ported verbatim (照搬) from lada. Convert a decoded frame's raw pts (in time_base
    units) to a frame number, matching EXACTLY how the appsrc derives a GStreamer buffer's
    offset for the same pts (pts -> ns with int truncation -> frame_num). Using the same
    two-step rounding guarantees a frame decoded by the AI pipeline lands on the same integer
    frame number as the passthrough side computes for that identical pts -- the playhead and
    the AI output position share one coordinate system.

    In sumu this is the same rounding convention native `Player`'s frame numbering must agree
    with if any Python-side code independently derives a frame number from a raw pts (e.g.
    cross-checking against ffprobe/PyAV-based tooling); the native Player itself does its own
    equivalent PTS->frame_num anchoring in C++ (I5) and does not call into this function."""
    frame_timestamp_ns = int((pts * time_base) * 1_000_000_000)
    return offset_ns_to_frame_num(frame_timestamp_ns, video_fps_exact)


def first_decoded_frame_num_after_seek(offset_ns, video_fps_exact) -> int:
    """Adapted (NOT a verbatim port) from lada's `first_decoded_frame_num_after_seek`.

    Flagging this discrepancy explicitly rather than silently working around it (matching the
    project's established honesty precedent in docs/spike3_nv12_interop.md's own "Discrepancy
    found in the task's premise" section): lada's original is NOT actually a pure function --
    it constructs a `VideoReader` (`with VideoReader(video_file, device=device,
    prefer_fast_seek=prefer_fast_seek) as vr: ... vr.seek(start_ns) ...`) on its PyAV-probe
    fallback path, to work around PyAV's BACKWARD seek landing on the nearest keyframe
    at-or-before the requested point rather than the requested point itself. Porting that
    branch verbatim would require porting `VideoReader`, which this task explicitly excludes
    (production decode does not go through PyAV/pynvc).

    sumu doesn't need that probe at all: native `Player.seek(frame_num)` (native/src/player.cpp)
    already anchors to the real decoded PTS internally (I5) and returns the true landed frame
    number directly from `Decoder::seek_to_frame`'s own PTS-based search -- see
    docs/native_core.md's "seek() = reposition" section, and the 4a regression's seek-stress
    result (20/20 `actual == target`, i.e. frame-exact against the *requested* frame in that
    test content's GOP structure). So the "probe-decode to discover where a backward-keyframe
    seek actually landed" problem lada's version solves does not exist in sumu's architecture
    -- callers should use `Player.seek()`'s own return value as the ground truth, not this
    function.

    What's kept from lada here is only the trivially pure fast-path math (lada's own
    `_pynvc_backend_available` branch: pynvc/frame-accurate seeks land exactly on
    `offset_ns_to_frame_num(start_ns, video_fps_exact)`, no probe needed) -- useful as a
    quick estimate (e.g. for pre-seek UI/logging) when no live `Player` handle is available
    yet to ask directly. This is an intentional simplification, not an oversight: do not treat
    this function's return value as authoritative for anything that needs true frame-accuracy
    against sumu's own decode; use `Player.seek()`'s return value for that.
    """
    return offset_ns_to_frame_num(offset_ns, video_fps_exact)
