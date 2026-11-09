# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Verification for the decoder's frame-normalization contract (native/src/video_normalizer.*,
# native/src/decoder.cpp): every downstream consumer is promised display-oriented, BT.709,
# limited-range, 8-bit NV12.
#
#   static   -- the normalizer's HLSL and present.hlsl compiled through d3dcompiler_47 (pure
#               CPU, no device), present.hlsl's NV12->RGB coefficients checked against the
#               BT.709 values, and the display-matrix inverse map checked against exact forward
#               transforms for all 8 rotation/mirror combinations.
#   gpu      -- synthetic materials (601 / 709 / full range / 10-bit / PQ / HLG, plus all eight
#               rotation/mirror display-matrix combinations), authored as exact YCbCr planes and
#               encoded losslessly, decoded through the real sumu_core.HeadlessDecode (d3d11va ->
#               VideoNormalizer -> CUDA NV12). Three independent references:
#                 (a) a numpy mirror of the shader math (tolerance ~1 code) -- catches a shader
#                     that does not implement the intended formula;
#                 (b) plain `ffmpeg` decode of the same file to RGB24 (ffmpeg's own
#                     matrix/range handling + its own transpose/hflip filters) -- catches a wrong
#                     matrix, wrong range handling or wrong rotation direction, since it shares no
#                     code with sumu. Not applied to HDR materials (ffmpeg CLI would clip instead
#                     of tone map);
#                 (c) a point-mapping verdict computed from the material's REAL 2x2 display
#                     matrix read back out of the container: where the matrix says each authored
#                     quadrant must land is compared with where it actually landed. Rotation and
#                     mirror labels are never trusted for this.
#   player   -- sumu_core.Player regression: dims() reports the normalized display size,
#               playback advances, seek lands and presenting resumes. --ai additionally runs the
#               real Scheduler on a separate long material (78 frames at 640x360, cold_start_s=0,
#               clip_length=3) and asserts frames_detected/no scheduler errors,
#               then pushes one frame through the real AI present bridge
#               (get_cuda_nv12_by_frame -> BGR -> RGBA -> push_ai_frame) and asserts it reaches
#               present. The material is synthetic, so this proves the AI channel runs and its
#               colours survive that round trip -- NOT that mosaics get restored (that needs real
#               mosaic content, covered by scripts/run_player.py on the standard test video).
#
# A missing dependency (sumu_core, ffmpeg) or a failing section is reported as FAIL, never as a
# skip that still ends in ALL CHECKS PASSED.
#
# Not covered here (needs eyes or other tools): present.hlsl's on-screen result (compiled and
# coefficient-checked only; the drawn image is verified by scripts/shot_*.py), HDR tone-map
# taste against other players (only monotonicity/brightness sanity is asserted), and
# mastering-display/MaxCLL metadata (ignored by design).
#
# Usage:
#   .venv/Scripts/python.exe scripts/verify_video_formats.py                # static+gpu+player
#   .venv/Scripts/python.exe scripts/verify_video_formats.py --static-only
#   .venv/Scripts/python.exe scripts/verify_video_formats.py --no-player --keep
#
# Materials land in native/trace/verify_video_formats/ (removed unless --keep).

import argparse
import ctypes
import json
import os
import re
import shutil
import subprocess
import sys

import numpy as np

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)
MATERIAL_DIR = os.path.join(_REPO, "native", "trace", "verify_video_formats")

# ---- tone-map constants: MUST match native/src/video_normalizer.cpp ---------------------------
SDR_WHITE_NITS = 100.0
HLG_PEAK_NITS = 1000.0
TONE_KNEE = 0.85
TONE_SOFT = 0.0175
TONE_VMAX = 100.0

SRC_W, SRC_H = 320, 180
FRAMES = 6

LUMA = {
    "bt709": (0.2126, 0.7152, 0.0722),
    "bt601": (0.299, 0.587, 0.114),
    "bt2020": (0.2627, 0.6780, 0.0593),
}

# Authored SDR pattern (non-linear BT.709 R'G'B'), one colour per quadrant plus two hue markers.
PATCHES = {
    "tl": (0.85, 0.10, 0.10),
    "tr": (0.10, 0.75, 0.15),
    "bl": (0.10, 0.15, 0.85),
    "br": (0.92, 0.92, 0.92),
}
# HDR pattern: linear BT.2020 light in nits, one luminance per quadrant.
HDR_NITS = {"tl": 100.0, "tr": 4000.0, "bl": 100.0, "br": 20.0}

_failures = []


def check(name, cond, detail=""):
    print(f"[{'PASS' if cond else 'FAIL'}] {name}{(' -- ' + detail) if detail else ''}")
    if not cond:
        _failures.append(name)
    return cond


def note(text):
    print(f"[note] {text}")


# ============================================================================================
# colour math (mirrors native/src/video_normalizer.cpp and native/shaders/present.hlsl)
# ============================================================================================

def ycbcr_to_rgb(y, cb, cr, luma):
    kr, kg, kb = luma
    r = y + cr * 2.0 * (1.0 - kr)
    b = y + cb * 2.0 * (1.0 - kb)
    g = (y - kr * r - kb * b) / kg
    return r, g, b


def rgb_to_ycbcr(r, g, b, luma):
    kr, kg, kb = luma
    y = kr * r + kg * g + kb * b
    return y, (b - y) / (2.0 * (1.0 - kb)), (r - y) / (2.0 * (1.0 - kr))


def sdr_eotf(v):
    v = np.maximum(v, 0.0)
    return np.where(v < 0.081, v / 4.5, ((v + 0.099) / 1.099) ** (1.0 / 0.45))


def sdr_oetf(v):
    v = np.maximum(v, 0.0)
    return np.where(v < 0.018, 4.5 * v, 1.099 * np.power(v, 0.45) - 0.099)


def pq_eotf(e):
    p = np.power(np.maximum(e, 0.0), 1.0 / 78.84375)
    return np.power(np.maximum(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p),
                    1.0 / 0.1593017578125)


def pq_inverse_eotf(lin):
    p = np.power(np.maximum(lin, 0.0), 0.1593017578125)
    return np.power((0.8359375 + 18.8515625 * p) / (1.0 + 18.6875 * p), 78.84375)


def hlg_inv_oetf(e):
    a, b, c = 0.17883277, 0.28466892, 0.55991073
    return np.where(e <= 0.5, e * e / 3.0, (np.exp((e - c) / a) + b) / 12.0)


def hlg_oetf(e):
    a, b, c = 0.17883277, 0.28466892, 0.55991073
    return np.where(e <= 1.0 / 12.0, np.sqrt(3.0 * e), a * np.log(np.maximum(12.0 * e - b, 1e-9)) + c)


def tonemap_lum(v):
    lognorm = np.log(1.0 + (TONE_VMAX - TONE_KNEE) / TONE_SOFT)
    shoulder = TONE_KNEE + (1.0 - TONE_KNEE) * np.log(
        np.maximum(1.0 + (v - TONE_KNEE) / TONE_SOFT, 1e-9)) / lognorm
    return np.where(v <= TONE_KNEE, v, shoulder)


def fit_peak(lin, mapped):
    """Desaturate towards the tone-mapped luminance until the peak channel fits [0,1]."""
    peak = lin.max(axis=-1)
    fit = (peak > 1.0) & (peak > mapped + 1e-6)
    if np.any(fit):
        p = peak[fit][:, None]
        m = mapped[fit][:, None]
        lin = lin.copy()
        lin[fit] = m + (lin[fit] - m) * ((1.0 - m) / (p - m))
    return np.maximum(lin, 0.0)


PRIMARIES_2020_TO_709 = np.array([
    [1.6605, -0.5876, -0.0728],
    [-0.1246, 1.1329, -0.0083],
    [-0.0182, -0.1006, 1.1187],
])


def to_rec709_primaries(rgb):
    """Linear source BT.2020 RGB -> linear BT.709, desaturating out-of-gamut results towards
    their own BT.709 luminance (so luminance survives the conversion)."""
    out = rgb @ PRIMARIES_2020_TO_709.T
    lum = out @ np.array(LUMA["bt709"])
    lo = out.min(axis=-1)
    fix = (lo < 0.0) & (lum > 1e-5)
    if np.any(fix):
        scale = lum[fix] / np.maximum(lum[fix] - lo[fix], 1e-6)
        out[fix] = lum[fix, None] + (out[fix] - lum[fix, None]) * scale[:, None]
    return np.maximum(out, 0.0)


def normalize_to_rgb(y, cb, cr, spec):
    """Source YCbCr codes -> non-linear BT.709 R'G'B' in [0,1], exactly as the shader does."""
    units = 1023.0 if spec["bit_depth"] == 10 else 255.0
    y_off = 0.0 if spec["full_range"] else (64.0 if spec["bit_depth"] == 10 else 16.0)
    y_span = units if spec["full_range"] else (876.0 if spec["bit_depth"] == 10 else 219.0)
    c_centre = 512.0 if spec["bit_depth"] == 10 else 128.0
    c_span = units if spec["full_range"] else (896.0 if spec["bit_depth"] == 10 else 224.0)

    yy = (y - y_off) / y_span
    ccb = (cb - c_centre) / c_span
    ccr = (cr - c_centre) / c_span
    r, g, b = ycbcr_to_rgb(yy, ccb, ccr, LUMA[spec["matrix"]])

    transfer = spec["transfer"]
    if transfer == "pq":
        lin = np.stack([pq_eotf(r), pq_eotf(g), pq_eotf(b)], axis=-1) * (10000.0 / SDR_WHITE_NITS)
    elif transfer == "hlg":
        e = np.stack([hlg_inv_oetf(r), hlg_inv_oetf(g), hlg_inv_oetf(b)], axis=-1)
        ys = e @ np.array(LUMA["bt2020"])
        lin = HLG_PEAK_NITS * np.power(np.maximum(ys, 1e-6), 0.2)[..., None] * e / SDR_WHITE_NITS
    else:
        lin = np.stack([sdr_eotf(r), sdr_eotf(g), sdr_eotf(b)], axis=-1)

    if spec["primaries_2020"]:
        lin = to_rec709_primaries(lin)
    if transfer != "sdr":
        lum = lin @ np.array(LUMA["bt709"])
        mapped = tonemap_lum(lum)
        scale = np.where(lum > 1e-5, mapped / np.maximum(lum, 1e-12), 1.0)
        lin = fit_peak(lin * scale[..., None], mapped)
    return np.clip(np.stack([sdr_oetf(lin[..., 0]), sdr_oetf(lin[..., 1]),
                             sdr_oetf(lin[..., 2])], axis=-1), 0.0, 1.0)


def encode_nv12_709_limited(rgb):
    y, cb, cr = rgb_to_ycbcr(rgb[..., 0], rgb[..., 1], rgb[..., 2], LUMA["bt709"])
    return (np.rint(16.0 + 219.0 * np.clip(y, 0.0, 1.0)),
            np.rint(128.0 + 224.0 * np.clip(cb, -0.5, 0.5)),
            np.rint(128.0 + 224.0 * np.clip(cr, -0.5, 0.5)))


def decode_nv12_709_limited(y_code, cb_code, cr_code):
    """What present.hlsl draws for a normalized frame."""
    yy = (y_code - 16.0) * (255.0 / 219.0) / 255.0
    cb = (cb_code - 128.0) * (255.0 / 224.0) / 255.0
    cr = (cr_code - 128.0) * (255.0 / 224.0) / 255.0
    r, g, b = ycbcr_to_rgb(yy, cb, cr, LUMA["bt709"])
    return np.clip(np.stack([r, g, b], axis=-1), 0.0, 1.0)


def upsample_nearest(plane):
    return np.repeat(np.repeat(plane, 2, axis=0), 2, axis=1)


# ============================================================================================
# display matrix (mirrors Decoder::resolve_display_transform / VideoNormalizer::fill_shader_params)
# ============================================================================================

def forward_matrix(rotation_cw, hflip):
    """Source->display linear part A = R(theta) * F_hflip, y-down frame, as [[a, c], [b, d]]."""
    t = np.deg2rad(rotation_cw)
    a, b, c, d = np.cos(t), np.sin(t), -np.sin(t), np.cos(t)
    if hflip:
        a, b = -a, -b
    return np.array([[a, c], [b, d]])


def extract_transform(a, b, c, d):
    det = a * d - b * c
    degrees = np.degrees(np.arctan2(b, a) if det > 0 else np.arctan2(-b, -a))
    return int(round(degrees / 90.0)) * 90 % 360, det < 0


def inverse_map(rotation_cw, hflip, src_w, src_h, dst_w, dst_h):
    A = forward_matrix(rotation_cw, hflip)
    bx0, bx1 = A[0, 0] * src_w / dst_w, A[0, 1] * src_h / dst_w
    by0, by1 = A[1, 0] * src_w / dst_h, A[1, 1] * src_h / dst_h
    inv = 1.0 / (bx0 * by1 - bx1 * by0)
    iu0, iu1 = by1 * inv, -bx1 * inv
    iv0, iv1 = -by0 * inv, bx0 * inv
    return (iu0, iu1, 0.5 - 0.5 * (iu0 + iu1)), (iv0, iv1, 0.5 - 0.5 * (iv0 + iv1))


def sample_bilinear(plane, tx, ty):
    h, w = plane.shape
    px = np.asarray(tx, dtype=np.float64) * w - 0.5
    py = np.asarray(ty, dtype=np.float64) * h - 0.5
    x0, y0 = np.floor(px).astype(np.int64), np.floor(py).astype(np.int64)
    fx, fy = px - x0, py - y0
    x0c, x1c = np.clip(x0, 0, w - 1), np.clip(x0 + 1, 0, w - 1)
    y0c, y1c = np.clip(y0, 0, h - 1), np.clip(y0 + 1, 0, h - 1)
    top = (1.0 - fx) * plane[y0c, x0c] + fx * plane[y0c, x1c]
    bot = (1.0 - fx) * plane[y1c, x0c] + fx * plane[y1c, x1c]
    return (1.0 - fy) * top + fy * bot


def reference_normalize(planes, spec, tex_size=None):
    """CPU mirror of VideoNormalizer::process(): source YCbCr codes -> 8-bit 709-limited NV12.

    `tex_size` is the decoder surface size when it is padded past the coded size (HEVC pools
    often are); the reference then replicates the edge rows, which the visible-area crop never
    samples, so it only affects the uv scale factors -- exactly like srcRect in the shader."""
    y_src = planes["y"].astype(np.float64)
    cb_src = planes["cb"].astype(np.float64)
    cr_src = planes["cr"].astype(np.float64)
    if tex_size:
        tex_w, tex_h = tex_size
        if tex_h > y_src.shape[0] or tex_w > y_src.shape[1]:
            y_src = np.pad(y_src, ((0, max(0, tex_h - y_src.shape[0])),
                                   (0, max(0, tex_w - y_src.shape[1]))), mode="edge")
            cb_src = np.pad(cb_src, ((0, max(0, tex_h // 2 - cb_src.shape[0])),
                                     (0, max(0, tex_w // 2 - cb_src.shape[1]))), mode="edge")
            cr_src = np.pad(cr_src, ((0, max(0, tex_h // 2 - cr_src.shape[0])),
                                     (0, max(0, tex_w // 2 - cr_src.shape[1]))), mode="edge")
    h_tex, w_tex = y_src.shape
    sx = min(1.0, spec["src_width"] / w_tex)
    sy = min(1.0, spec["src_height"] / h_tex)
    row_u, row_v = inverse_map(spec["rotation_cw"], spec["hflip"], spec["src_width"],
                               spec["src_height"], spec["dst_width"], spec["dst_height"])

    def one_pass(dst_w, dst_h, half_texel):
        x = np.arange(dst_w, dtype=np.float64)
        y = np.arange(dst_h, dtype=np.float64)
        ud = np.tile((x + 0.5) / dst_w, (dst_h, 1))
        vd = np.tile(((y + 0.5) / dst_h)[:, None], (1, dst_w))
        if half_texel:
            ud = ud + 0.5 / spec["dst_width"]
            vd = vd + 0.5 / spec["dst_height"]
        us = (row_u[0] * ud + row_u[1] * vd + row_u[2]) * sx
        vs = (row_v[0] * ud + row_v[1] * vd + row_v[2]) * sy
        return normalize_to_rgb(sample_bilinear(y_src, us, vs), sample_bilinear(cb_src, us, vs),
                                sample_bilinear(cr_src, us, vs), spec)

    y_out, _, _ = encode_nv12_709_limited(one_pass(spec["dst_width"], spec["dst_height"], False))
    _, cb_out, cr_out = encode_nv12_709_limited(
        one_pass(spec["dst_width"] // 2, spec["dst_height"] // 2, True))
    return y_out, cb_out, cr_out


# ============================================================================================
# materials
# ============================================================================================

def authored_pattern(kind):
    h, w = SRC_H, SRC_W
    img = np.zeros((h, w, 3), dtype=np.float64)
    quads = {
        "tl": (slice(0, h // 2), slice(0, w // 2)),
        "tr": (slice(0, h // 2), slice(w // 2, w)),
        "bl": (slice(h // 2, h), slice(0, w // 2)),
        "br": (slice(h // 2, h), slice(w // 2, w)),
    }
    if kind == "sdr":
        for key, rect in quads.items():
            img[rect] = PATCHES[key]
        img[4:20, 20:70] = (0.80, 0.80, 0.15)      # yellow-ish
        img[h - 20:h - 4, w - 70:w - 20] = (0.15, 0.80, 0.80)  # cyan-ish
        img[0:8, 0:8] = (0.0, 0.0, 0.0)            # black level probe
        img[0:8, w - 8:w] = (1.0, 1.0, 1.0)        # white level probe
        return img
    lum = np.zeros((h, w, 3), dtype=np.float64)
    for key, rect in quads.items():
        lum[rect] = HDR_NITS[key] / SDR_WHITE_NITS
    lum[10:40, 20:70] = np.array([4.0, 0.4, 0.4])          # extreme saturated BT.2020 red
    lum[50:75, 80:130] = np.array([0.45, 0.06, 0.06])      # moderate saturated red (hue probe)
    lum[h - 40:h - 10, 20:70] = np.array([0.4, 0.4, 4.0])  # saturated BT.2020 blue
    return lum


def author_planes(spec):
    if spec["transfer"] == "sdr":
        rgb = authored_pattern("sdr")
    else:
        lin = authored_pattern("hdr") * SDR_WHITE_NITS
        if spec["transfer"] == "pq":
            rgb = pq_inverse_eotf(lin / 10000.0)
        else:
            # HLG authoring: invert OOTF (L_D = peak * Y_S^(gamma-1) * E_S, gamma = 1.2 for a
            # 1000-nit nominal peak) then the OETF, so the decoded display light round-trips.
            gamma = 1.2
            yd = lin / HLG_PEAK_NITS
            scene_lum = np.maximum(yd @ np.array(LUMA["bt2020"]), 1e-9) ** (1.0 / gamma)
            scene = np.clip(yd / scene_lum[..., None] ** (gamma - 1.0), 0.0, 1.0)
            rgb = hlg_oetf(scene)
    y, cb, cr = rgb_to_ycbcr(rgb[..., 0], rgb[..., 1], rgb[..., 2], LUMA[spec["matrix"]])
    units = 1023.0 if spec["bit_depth"] == 10 else 255.0
    if spec["full_range"]:
        y_code, cb_code, cr_code = y * units, (cb + 0.5) * units, (cr + 0.5) * units
    else:
        y_lo, y_span = (64.0, 876.0) if spec["bit_depth"] == 10 else (16.0, 219.0)
        c_mid = 512.0 if spec["bit_depth"] == 10 else 128.0
        c_span = 896.0 if spec["bit_depth"] == 10 else 224.0
        y_code, cb_code, cr_code = y_lo + y_span * y, c_mid + c_span * cb, c_mid + c_span * cr
    clip = lambda a: np.rint(np.clip(a, 0, units)).astype(np.int32)
    return {"y": clip(y_code), "cb": clip(cb_code)[::2, ::2], "cr": clip(cr_code)[::2, ::2]}


def write_material(spec, path):
    planes = author_planes(spec)
    depth = spec["bit_depth"]
    tmp = path + ".raw"
    with open(tmp, "wb") as fh:
        for _ in range(FRAMES):
            # Raw yuv420p10le holds plain 10-bit codes (0..1023); the <<6 left alignment is a
            # P010 *surface* detail, not a file-layout one.
            y = planes["y"].astype(np.uint16 if depth == 10 else np.uint8)
            cb = planes["cb"].astype(np.uint16 if depth == 10 else np.uint8)
            cr = planes["cr"].astype(np.uint16 if depth == 10 else np.uint8)
            # Planar (I420) layout: full luma, then the U plane, then the V plane.
            fh.write(np.ascontiguousarray(y).tobytes())
            fh.write(np.ascontiguousarray(cb).tobytes())
            fh.write(np.ascontiguousarray(cr).tobytes())

    x265 = ":".join([
        "colorprim=" + spec["x265_primaries"], "transfer=" + spec["x265_transfer"],
        "colormatrix=" + spec["x265_matrix"],
        "range=" + ("full" if spec["full_range"] else "limited"),
        "lossless=1", "log-level=error",
    ])
    subprocess.run([
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        # The colour options are INPUT options on purpose: they tag the raw frames so no
        # implicit 601-default swscale conversion gets inserted between demuxer and encoder
        # (the samples are authored in exactly the space being encoded).
        "-color_primaries", spec["primaries"], "-color_trc", spec["transfer_name"],
        "-colorspace", spec["colorspace_name"],
        "-color_range", "pc" if spec["full_range"] else "tv",
        "-f", "rawvideo", "-pix_fmt", "yuv420p10le" if depth == 10 else "yuv420p",
        "-s", f"{SRC_W}x{SRC_H}", "-r", "10", "-i", tmp,
        "-c:v", "libx265", "-x265-params", x265,
        path,
    ], check=True)
    os.remove(tmp)

    probe = json.loads(subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_streams",
         "-print_format", "json", path], check=True, capture_output=True, text=True).stdout
    )["streams"][0]
    got = (probe.get("color_space"), probe.get("color_transfer"), probe.get("color_primaries"),
           probe.get("color_range"))
    want = (spec["colorspace_name"], spec["transfer_name"], spec["primaries"],
            "pc" if spec["full_range"] else "tv")
    check(f"{spec['name']}: encoder kept the tags", got == want, f"want={want} got={got}")


def ffprobe_display_matrix(path):
    """The stream's real display matrix: dict(a, b, c, d, x, y) with p' = a p + c q + x and
    q' = b p + d q + y (libavutil/display.h packs (a, b, u)(c, d, v)(x, y, w) as 16.16 fixed
    point). None when the stream carries no DISPLAYMATRIX side data."""
    probe = json.loads(subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream_side_data",
         "-print_format", "json", path], check=True, capture_output=True, text=True).stdout)
    for side in probe.get("streams", [{}])[0].get("side_data_list", []):
        text = side.get("displaymatrix")
        if not text:
            continue
        rows = [[int(n) for n in re.findall(r"-?\d+", line.split(":", 1)[-1])]
                for line in text.splitlines() if line.strip()]
        rows = [row for row in rows if len(row) == 3]
        if len(rows) == 3:
            return {"a": rows[0][0] / 65536.0, "b": rows[0][1] / 65536.0,
                    "c": rows[1][0] / 65536.0, "d": rows[1][1] / 65536.0,
                    "x": rows[2][0] / 65536.0, "y": rows[2][1] / 65536.0}
    return None


def expected_matrix(rotation_cw, hflip):
    """R(rotation_cw) * F_hflip as the same (a, b, c, d) packing the container uses."""
    A = forward_matrix(rotation_cw, hflip)
    return {"a": A[0, 0], "b": A[1, 0], "c": A[0, 1], "d": A[1, 1]}


def matrix_matches(matrix, rotation_cw, hflip, tol=1e-3):
    if not matrix:
        return False
    want = expected_matrix(rotation_cw, hflip)
    return all(abs(matrix[key] - want[key]) <= tol for key in ("a", "b", "c", "d"))


def ffmpeg_rotation_option(rotation_cw, hflip):
    """ffmpeg's -display_rotation takes counter-clockwise degrees and, once -display_hflip is
    also present, the sign inverts (verified by reading the written matrix back for all eight
    combinations -- see run_materials)."""
    return rotation_cw % 360 if hflip else (360 - rotation_cw) % 360


def add_display_matrix(src_path, dst_path, rotation_cw, hflip):
    """Write a material whose DISPLAYMATRIX is exactly R(rotation_cw) * F_hflip, and return the
    matrix ffprobe reads back. ffmpeg's -display_rotation sign convention depends on whether a
    mirror is also present, so both conventions are tried and the read-back decides -- the fixture
    can never be silently off, which is what made the earlier mirrored material encode R270*F
    while its label said R90*F."""
    angle = ffmpeg_rotation_option(rotation_cw, hflip)
    candidates = list(dict.fromkeys([angle, (360 - rotation_cw) % 360, rotation_cw % 360]))
    readback = None
    for candidate in candidates:
        # -display_rotation / -display_hflip are INPUT options in ffmpeg 7.x: they set the display
        # matrix the demuxer reports, and -c copy carries it into the output container.
        cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y"]
        if candidate:
            cmd += ["-display_rotation", str(candidate)]
        if hflip:
            cmd += ["-display_hflip"]
        subprocess.run(cmd + ["-i", src_path, "-c", "copy", dst_path], check=True)
        readback = ffprobe_display_matrix(dst_path)
        if matrix_matches(readback, rotation_cw, hflip):
            return readback, candidate
    return readback, None


def build_specs():
    base = {
        "bit_depth": 8, "full_range": False, "matrix": "bt709", "transfer": "sdr",
        "primaries_2020": False, "colorspace_name": "bt709", "transfer_name": "bt709",
        "primaries": "bt709", "x265_matrix": "bt709", "x265_transfer": "bt709",
        "x265_primaries": "bt709", "rotation_cw": 0, "hflip": False,
        "src_width": SRC_W, "src_height": SRC_H, "base": None, "display_matrix": None,
    }

    def make(name, **over):
        spec = dict(base)
        spec.update(over)
        spec["name"] = name
        swap = spec["rotation_cw"] % 180 != 0
        spec["dst_width"] = spec["src_height"] if swap else spec["src_width"]
        spec["dst_height"] = spec["src_width"] if swap else spec["src_height"]
        spec["path"] = os.path.join(MATERIAL_DIR, name + ".mp4")
        return spec

    b601 = dict(matrix="bt601", colorspace_name="smpte170m", x265_matrix="smpte170m")
    hdr = dict(bit_depth=10, matrix="bt2020", primaries_2020=True, colorspace_name="bt2020nc",
               primaries="bt2020", x265_matrix="bt2020nc", x265_primaries="bt2020")
    specs = [
        make("bt709_limited_8bit"),
        make("bt601_limited_8bit", **b601),
        make("bt709_fullrange_8bit", full_range=True),
        make("bt709_limited_10bit", bit_depth=10),
        make("pq2020_10bit", transfer="pq", transfer_name="smpte2084", x265_transfer="smpte2084",
             **hdr),
        make("hlg2020_10bit", transfer="hlg", transfer_name="arib-std-b67",
             x265_transfer="arib-std-b67", **hdr),
    ]
    # All eight rotation/mirror combinations as real materials (a display matrix in the
    # container), so the decoder's transform is exercised both ways.
    for rotation in (0, 90, 180, 270):
        for hflip in (False, True):
            if rotation == 0 and not hflip:
                continue
            name = f"rot{rotation}" + ("_hflip" if hflip else "")
            specs.append(make(name, rotation_cw=rotation, hflip=hflip,
                              base="bt709_limited_8bit"))
    specs.append(make("bt601_rot270", rotation_cw=270, base="bt601_limited_8bit", **b601))
    return specs


def run_materials(specs, reuse):
    os.makedirs(MATERIAL_DIR, exist_ok=True)
    paths = {s["name"]: s["path"] for s in specs}
    for spec in specs:
        if spec["base"]:
            continue
        if reuse and os.path.exists(spec["path"]):
            continue
        write_material(spec, spec["path"])

    for spec in specs:
        if not spec["base"]:
            continue
        if not (reuse and os.path.exists(spec["path"])):
            readback, angle = add_display_matrix(paths[spec["base"]], spec["path"],
                                                 spec["rotation_cw"], spec["hflip"])
            spec["display_matrix"] = readback
            check(f"{spec['name']}: material carries R({spec['rotation_cw']})*F_hflip",
                  matrix_matches(readback, spec["rotation_cw"], spec["hflip"], tol=1e-3),
                  f"readback={{a:{readback['a']:+.0f} b:{readback['b']:+.0f} "
                  f"c:{readback['c']:+.0f} d:{readback['d']:+.0f}}} via -display_rotation "
                  f"{angle}" if readback else "no display matrix written")
        elif spec["display_matrix"] is None:
            spec["display_matrix"] = ffprobe_display_matrix(spec["path"])
            check(f"{spec['name']}: reused material carries R({spec['rotation_cw']})*F_hflip",
                  matrix_matches(spec["display_matrix"], spec["rotation_cw"], spec["hflip"]))



# ============================================================================================
# static checks
# ============================================================================================

def extract_hlsl_from_cpp(path):
    text = open(path, encoding="utf-8").read()
    return text[text.index('R"HLSL(') + len('R"HLSL('):text.index(')HLSL"')]


_D3DCOMPILE = None


def d3dcompile():
    global _D3DCOMPILE
    if _D3DCOMPILE is not None:
        return _D3DCOMPILE
    lib = None
    for name in ("d3dcompiler_47.dll", "d3dcompiler_43.dll"):
        try:
            lib = ctypes.WinDLL(name)
            break
        except OSError:
            continue
    if lib is None:
        return None
    fn = lib.D3DCompile
    fn.restype = ctypes.c_long
    fn.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_void_p,
                   ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint,
                   ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p)]
    _D3DCOMPILE = fn
    return fn


def _blob_bytes(blob):
    if not blob:
        return b""
    vtbl = ctypes.cast(blob, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p)))[0]
    get_pointer = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vtbl[3])
    get_size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vtbl[4])
    size = get_size(blob)
    data = ctypes.string_at(get_pointer(blob), size) if size else b""
    ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vtbl[2])(blob)  # Release
    return data


def compile_hlsl(source, entry, profile):
    fn = d3dcompile()
    if fn is None:
        return None, "d3dcompiler_47 not available"
    code, errors = ctypes.c_void_p(), ctypes.c_void_p()
    hr = fn(source.encode("utf-8"), len(source), b"verify.hlsl", None, None,
            entry.encode(), profile.encode(), 0, 0, ctypes.byref(code), ctypes.byref(errors))
    _blob_bytes(code)
    message = _blob_bytes(errors)
    return hr == 0, message.decode("utf-8", "replace").strip()


def static_present_coefficients():
    text = open(os.path.join(_REPO, "native", "shaders", "present.hlsl"), encoding="utf-8").read()
    expected = {
        "r": [255.0 / 219.0 * 256.0, (255.0 / 224.0) * 2.0 * (1.0 - 0.2126) * 256.0],
        "g": [255.0 / 219.0 * 256.0,
              (255.0 / 224.0) * 2.0 * (1.0 - 0.0722) * 0.0722 / 0.7152 * 256.0,
              (255.0 / 224.0) * 2.0 * (1.0 - 0.2126) * 0.2126 / 0.7152 * 256.0],
        "b": [255.0 / 219.0 * 256.0, (255.0 / 224.0) * 2.0 * (1.0 - 0.0722) * 256.0],
    }
    ok = True
    for channel, values in expected.items():
        line = next((l for l in text.splitlines() if l.strip().startswith(f"float {channel} =")), "")
        # Drop the clamp() tail (0.0, 255.0) so only the coefficient expression is parsed.
        expression = line.split("((", 1)[-1].split(",")[0]
        numbers = [float(v) for v in re.findall(r"\d+\.\d+", expression)]
        magnitudes = numbers[:-1] if numbers and abs(numbers[-1] - 256.0) < 1e-9 else numbers
        if len(magnitudes) != len(values):
            ok = False
            print(f"       channel {channel}: unexpected expression {line.strip()}")
            continue
        for got, want in zip(magnitudes, values):
            if abs(got - want) > 0.01:
                ok = False
                print(f"       channel {channel}: {got} != BT.709 {want:.4f}")
    check("present.hlsl NV12->RGB uses the BT.709 coefficients", ok)
    check("present.hlsl keeps the b0 SliceCB ABI",
          "cbuffer SliceCB : register(b0)" in text and "uint arraySlice;" in text
          and "uint3 _pad;" in text)


def static_display_matrix_model():
    ok = True
    for rotation in (0, 90, 180, 270):
        for hflip in (False, True):
            A = forward_matrix(rotation, hflip)
            got = extract_transform(A[0, 0], A[1, 0], A[0, 1], A[1, 1])
            if got != (rotation, hflip):
                ok = False
                print(f"       extract mismatch want=({rotation},{hflip}) got={got}")
                continue
            w, h = 64, 36
            dw, dh = (h, w) if rotation % 180 else (w, h)
            row_u, row_v = inverse_map(rotation, hflip, w, h, dw, dh)
            corners = np.array([[0, 0], [w, 0], [0, h], [w, h]], dtype=np.float64) @ A.T
            t = -corners.min(axis=0)
            for (xs, ys) in ((0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1), (7, 11)):
                q = A @ np.array([xs + 0.5, ys + 0.5]) + t
                ud, vd = q[0] / dw, q[1] / dh
                us = row_u[0] * ud + row_u[1] * vd + row_u[2]
                vs = row_v[0] * ud + row_v[1] * vd + row_v[2]
                if abs(us * w - 0.5 - xs) > 1e-6 or abs(vs * h - 0.5 - ys) > 1e-6:
                    ok = False
                    print(f"       inverse map mismatch rot={rotation} flip={hflip} "
                          f"src=({xs},{ys}) -> ({us * w - 0.5:.4f},{vs * h - 0.5:.4f})")
    check("display-matrix inverse map is exact for all 8 rotation/mirror cases", ok)


def run_static():
    print("\n== static: HLSL compiles / BT.709 coefficients / display-matrix model ==")
    hlsl = {
        "normalizer": extract_hlsl_from_cpp(os.path.join(_REPO, "native", "src",
                                                         "video_normalizer.cpp")),
        "present": open(os.path.join(_REPO, "native", "shaders", "present.hlsl"),
                        encoding="utf-8").read(),
    }
    stages = [("normalizer", "VSMain", "vs_5_0"), ("normalizer", "PSMain_Y", "ps_5_0"),
              ("normalizer", "PSMain_UV", "ps_5_0"), ("normalizer", "PSMain_RGB", "ps_5_0"),
              ("present", "VSMain", "vs_5_0"), ("present", "PSMain_NV12", "ps_5_0"),
              ("present", "PSMain_AI", "ps_5_0")]
    unavailable = False
    for group, entry, profile in stages:
        ok, message = compile_hlsl(hlsl[group], entry, profile)
        if ok is None:
            unavailable = True
            break
        check(f"{group}: {entry} compiles for {profile}", ok,
              "" if ok else message.splitlines()[0] if message else "")
    if unavailable:
        note("d3dcompiler_47 unavailable -- HLSL compile checks skipped")
    static_present_coefficients()
    static_display_matrix_model()


# ============================================================================================
# GPU checks
# ============================================================================================

def gpu_decode_nv12(path, frame_index=1):
    """Decode through the real native path. The CUDA buffer belongs to HeadlessDecode, so the
    wrap + host copy must happen while it is still open -- close() releases the buffer."""
    import sumu_core
    from sumu.ai.utils.cuda_dlpack import wrap_nv12_cuda_buffer_as_tensor

    dec = sumu_core.HeadlessDecode()
    try:
        dec.open(path)
        frame = None
        for _ in range(frame_index + 1):
            got = dec.next_frame()
            if got.get("eof"):
                break
            frame = got
        if frame is None:
            raise RuntimeError(f"no frames decoded from {path}")
        w, h = frame["width"], frame["height"]
        nv12 = wrap_nv12_cuda_buffer_as_tensor(frame["dev_ptr"], w, h, frame["pitch_bytes"])
        arr = nv12[:, :].cpu().numpy().copy()  # verification-only readback, copied before close
    finally:
        dec.close()
    uv = arr[h:h + h // 2, :w].reshape(h // 2, w // 2, 2).astype(np.float64)
    return ({"y": arr[:h, :w].astype(np.float64), "cb": uv[..., 0], "cr": uv[..., 1]}, w, h)


def ffmpeg_rgb_reference(path, w, h, extra_filter=None):
    """Independent reference: ffmpeg CLI decode to RGB24 (its own matrix/range + autorotate)."""
    chain = ",".join(part for part in (extra_filter, f"scale={w}:{h}:flags=neighbor") if part)
    out = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-frames:v", "1",
                          "-vf", chain, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
                         check=True, capture_output=True).stdout
    return np.frombuffer(out, np.uint8).reshape(h, w, 3).astype(np.float64) / 255.0


def source_to_dest_quadrants(matrix, src_w, src_h, dst_w, dst_h):
    """Source quadrant -> destination quadrant, taken straight from the material's display
    matrix linear part (normalized so the source rect maps onto the destination rect). This is
    the independent point-mapping check: no rotation/flip labels involved."""
    bx0, bx1 = matrix["a"] * src_w / dst_w, matrix["c"] * src_h / dst_w
    by0, by1 = matrix["b"] * src_w / dst_h, matrix["d"] * src_h / dst_h
    mapping = {}
    for name, (u, v) in (("tl", (0.25, 0.25)), ("tr", (0.75, 0.25)),
                         ("bl", (0.25, 0.75)), ("br", (0.75, 0.75))):
        p = 0.5 + bx0 * (u - 0.5) + bx1 * (v - 0.5)
        q = 0.5 + by0 * (u - 0.5) + by1 * (v - 0.5)
        mapping[name] = ("tl", "tr", "bl", "br")[(0 if q < 0.5 else 1) * 2 + (0 if p < 0.5 else 1)]
    return mapping


def classify_quadrants(rgb, rects):
    """Nearest authored patch colour per destination quadrant (markers cannot flip the answer:
    the four patch colours are far apart)."""
    out = {}
    for name, rect in rects.items():
        mean = patch_mean(rgb, rect)
        out[name] = min(PATCHES, key=lambda k: float(np.abs(mean - np.array(PATCHES[k])).sum()))
    return out


def quad_rects(h, w, inset=8):
    return {
        "tl": (slice(inset, h // 2 - inset), slice(inset, w // 2 - inset)),
        "tr": (slice(inset, h // 2 - inset), slice(w // 2 + inset, w - inset)),
        "bl": (slice(h // 2 + inset, h - inset), slice(inset, w // 2 - inset)),
        "br": (slice(h // 2 + inset, h - inset), slice(w // 2 + inset, w - inset)),
    }


def hdr_rects(h, w):
    """Measurement rects for the HDR pattern, clear of the saturated markers."""
    return {
        "diffuse_white": (slice(2, 26), slice(132, 158)),  # 100 nits, TL quadrant
        "peak": (slice(20, 70), slice(200, 300)),          # 4000 nits, TR quadrant
        "dim": (slice(100, 170), slice(200, 300)),         # 20 nits, BR quadrant
        "red_extreme": (slice(14, 36), slice(26, 64)),     # 4x SDR white saturated red
        "red_moderate": (slice(54, 71), slice(86, 124)),   # saturated red that must keep hue
        "blue": (slice(h - 36, h - 14), slice(26, 64)),    # saturated blue
    }


def patch_mean(rgb, rect):
    return rgb[rect].reshape(-1, 3).mean(axis=0)


def run_gpu(specs):
    print("\n== gpu: HeadlessDecode -> VideoNormalizer -> CUDA NV12 ==")
    try:
        import sumu_core  # noqa: F401
    except Exception as exc:
        check("gpu: import sumu_core", False, str(exc))
        return

    path_by_name = {s["name"]: s["path"] for s in specs}
    for spec in specs:
        name = spec["name"]
        print(f"\n-- {name} --")
        try:
            planes, w, h = gpu_decode_nv12(spec["path"])
        except Exception as exc:
            check(f"{name}: decode", False, str(exc))
            continue

        check(f"{name}: output size is the normalized display size",
              (w, h) == (spec["dst_width"], spec["dst_height"]),
              f"got {w}x{h}, want {spec['dst_width']}x{spec['dst_height']}")

        src = author_planes(spec)
        # The decoder's D3D11VA pool may be padded past the coded size; try the plausible
        # surface sizes and keep the best match (the reported one is informative in itself).
        candidates = [None]
        for align in (8, 16, 32, 64):
            h_pad = -(-spec["src_height"] // align) * align
            w_pad = -(-spec["src_width"] // align) * align
            if (w_pad, h_pad) != (spec["src_width"], spec["src_height"]):
                candidates.append((w_pad, h_pad))
        best = None
        for tex_size in candidates:
            ref_y, ref_cb, ref_cr = reference_normalize(src, spec, tex_size)
            if ref_y.shape != planes["y"].shape:
                continue
            score = float(np.abs(planes["y"] - ref_y).mean())
            if best is None or score < best[0]:
                best = (score, tex_size, (ref_y, ref_cb, ref_cr))
        if best is None:
            check(f"{name}: reference shape", False, f"{planes['y'].shape} vs reference")
            continue
        if best[1]:
            note(f"{name}: decoder surface padded to {best[1][0]}x{best[1][1]} "
                 f"(visible crop applied)")
        ref_y, ref_cb, ref_cr = best[2]

        y_mae = float(np.abs(planes["y"] - ref_y).mean())
        cb_mae = float(np.abs(planes["cb"] - ref_cb).mean())
        cr_mae = float(np.abs(planes["cr"] - ref_cr).mean())
        y_max = float(np.abs(planes["y"] - ref_y).max())
        check(f"{name}: shader matches the formula reference (MAE <= 1.2 codes)",
              y_mae <= 1.2 and cb_mae <= 1.2 and cr_mae <= 1.2,
              f"y={y_mae:.2f} cb={cb_mae:.2f} cr={cr_mae:.2f} y_max={y_max:.0f}")

        out_rgb = decode_nv12_709_limited(planes["y"], upsample_nearest(planes["cb"]),
                                          upsample_nearest(planes["cr"]))

        orientation_dev = None
        ffmpeg_mae = None
        rotation_fallback = None
        if spec["transfer"] == "sdr":
            # Reference image: ffmpeg applying the transform itself. The filter chain is derived
            # from the material's REAL display matrix, never from the spec's rotation/flip
            # labels, so a mislabelled fixture cannot silently agree with a wrong expectation.
            matrix = spec.get("display_matrix")
            ref_path, ref_filter = spec["path"], None
            if spec["base"]:
                ref_path = path_by_name[spec["base"]]
                steps = []
                if matrix:
                    rotation, mirror = extract_transform(matrix["a"], matrix["b"],
                                                         matrix["c"], matrix["d"])
                    rotation_fallback = (rotation, mirror)
                    if mirror:
                        steps.append("hflip")
                    steps += ["transpose=clock"] * ((rotation // 90) % 4)
                ref_filter = ",".join(steps) or None
            try:
                ref_rgb = ffmpeg_rgb_reference(ref_path, w, h, ref_filter)
                ffmpeg_mae = float(np.abs(out_rgb - ref_rgb).mean() * 255.0)
                check(f"{name}: matches ffmpeg's independent decode (MAE <= 3 codes)",
                      ffmpeg_mae <= 3.0, f"mae={ffmpeg_mae:.2f} codes")
                rects = quad_rects(h, w)
                worst = 0.0
                for key, rect in rects.items():
                    worst = max(worst, float(np.abs(patch_mean(out_rgb, rect)
                                                    - patch_mean(ref_rgb, rect)).max()))
                orientation_dev = worst
                check(f"{name}: per-quadrant colour/orientation agrees with ffmpeg",
                      worst <= 0.04, f"worst quadrant deviation {worst:.3f}")
            except Exception as exc:
                check(f"{name}: ffmpeg independent reference", False, str(exc))

            # Independent point-mapping verdict: where the container's own display matrix says
            # each authored quadrant must land, read back from the decoded frame. Classifying the
            # output gives dest->patch, so invert it to compare against source->dest.
            matrix = spec.get("display_matrix")
            predicted = (source_to_dest_quadrants(matrix, spec["src_width"], spec["src_height"],
                                                  w, h)
                         if matrix else {q: q for q in ("tl", "tr", "bl", "br")})
            observed = {patch: dest for dest, patch
                        in classify_quadrants(out_rgb, quad_rects(h, w)).items()}
            check(f"{name}: quadrant placement follows the container's display matrix",
                  observed == predicted, f"predicted {predicted} observed {observed}"
                  + (f" (matrix decomposes to cw={rotation_fallback[0]} "
                     f"mirror={rotation_fallback[1]})" if rotation_fallback else ""))

        if spec["transfer"] == "sdr" and spec["rotation_cw"] == 0 and not spec["hflip"]:
            # Black/white probes only sit in known corners without a display transform; the
            # rotated materials are covered by the ffmpeg comparison above instead.
            black = float(planes["y"][0:6, 0:6].mean())
            white = float(planes["y"][0:6, w - 6:w].mean())
            check(f"{name}: black/white land on studio range",
                  abs(black - 16.0) <= 1.5 and abs(white - 235.0) <= 1.5,
                  f"black luma {black:.1f} white luma {white:.1f} "
                  f"(source {'full' if spec['full_range'] else 'limited'} range)")

        if spec["transfer"] in ("pq", "hlg"):
            rects = hdr_rects(h, w)
            lum = out_rgb @ np.array(LUMA["bt709"])
            white_patch = float(lum[rects["diffuse_white"]].mean())
            peak_patch = float(lum[rects["peak"]].mean())
            dim_patch = float(lum[rects["dim"]].mean())
            check(f"{name}: tone map keeps diffuse white below clipping", white_patch < 0.99,
                  f"100-nit -> {white_patch:.3f}")
            check(f"{name}: tone map is monotone in input luminance",
                  peak_patch > white_patch > dim_patch,
                  f"20-nit {dim_patch:.3f} < 100-nit {white_patch:.3f} < 4000-nit {peak_patch:.3f}")
            check(f"{name}: tone map does not crush diffuse white", white_patch > 0.60,
                  f"100-nit -> {white_patch:.3f}")

            # Saturated BT.2020 patches: the authored luminance has to survive the primary
            # conversion and the tone map (a hard out-of-gamut clamp or wrong desaturation
            # weights show up here as a luminance shift).
            for label, rect, authored in (
                ("extreme red", rects["red_extreme"], (4.0, 0.4, 0.4)),
                ("moderate red", rects["red_moderate"], (0.45, 0.06, 0.06)),
                ("blue", rects["blue"], (0.4, 0.4, 4.0)),
            ):
                code = patch_mean(out_rgb, rect)
                measured = float(sdr_eotf(code) @ np.array(LUMA["bt709"]))
                expected = float(tonemap_lum(np.dot(authored, np.array(LUMA["bt2020"]))))
                rel = abs(measured - expected) / max(expected, 1e-6)
                check(f"{name}: saturated {label} patch keeps its tone-mapped luminance",
                      rel <= 0.08, f"measured {measured:.3f} vs expected {expected:.3f} "
                      f"(rel {rel:.3f})")
            # A 4x-SDR-white saturated colour can only be represented near white once its
            # luminance is preserved; a colour that fits inside the range must keep its hue.
            for label, rect, dominant in (("moderate red", rects["red_moderate"], 0),
                                          ("blue", rects["blue"], 2)):
                code = patch_mean(out_rgb, rect)
                others = [i for i in range(3) if i != dominant]
                check(f"{name}: saturated {label} patch keeps its hue",
                      code[dominant] > code[others].max() + 0.10,
                      f"rgb=({code[0]:.2f}, {code[1]:.2f}, {code[2]:.2f})")

        if name == "bt709_limited_8bit":
            # The fast path must never touch the frame: the output has to be the decoded
            # surface bit for bit (lossless source material).
            same_y = int(np.abs(planes["y"] - src["y"]).max())
            check(f"{name}: fast path passes the decoded surface through untouched",
                  same_y == 0, f"max luma deviation {same_y} codes")

        spec["summary"] = {
            "size": [w, h], "y_mae": round(y_mae, 3), "cb_mae": round(cb_mae, 3),
            "cr_mae": round(cr_mae, 3),
            "ffmpeg_mae": None if ffmpeg_mae is None else round(ffmpeg_mae, 3),
            "quadrant_dev": None if orientation_dev is None else round(orientation_dev, 4),
            "y_range": [int(planes["y"].min()), int(planes["y"].max())],
        }
        print(f"       {json.dumps(spec['summary'])}")


# ============================================================================================
# player regression
# ============================================================================================

def run_player(specs, run_ai, ai_spec=None):
    print("\n== player: open / play / seek regression ==")
    try:
        import sumu_core
    except Exception as exc:
        check("player: import sumu_core", False, str(exc))
        return

    import logging
    import time

    models = None
    if run_ai:
        try:
            import torch
            from sumu.pipeline import build_models
            from sumu.scheduler import Scheduler, SchedulerConfig
            det, res, pad = build_models(torch.device("cuda"), True, allow_trt_compile=False)
            models = (det, res, pad, Scheduler, SchedulerConfig)
        except Exception as exc:
            check("player: load the AI models", False, str(exc))
            return

    for spec in specs:
        name = spec["name"]
        print(f"\n-- player {name} --")
        player = None
        try:
            player = sumu_core.Player(1280, 720, True)
            player.open(spec["path"])
            dims = tuple(player.dims())
            check(f"{name}: Player dims() reports the normalized display size",
                  dims == (spec["dst_width"], spec["dst_height"]),
                  f"got {dims}, want {(spec['dst_width'], spec['dst_height'])}")

            player.play()
            t0 = time.perf_counter()
            while time.perf_counter() - t0 < 1.5:
                player.pump_messages()
                player.ui_tick()
                player.take_ui_intents()
                time.sleep(0.02)
            play_stats = player.stats()
            check(f"{name}: playback advances", play_stats["present_count"] > 10,
                  f"present_count={play_stats['present_count']} "
                  f"current_frame={play_stats['current_frame']}")

            target = max(0, min(player.frame_count() - 2, int(player.frame_count() * 0.6)))
            actual = player.seek(target)
            check(f"{name}: seek lands on the requested frame", abs(actual - target) <= 1,
                  f"target={target} actual={actual}")
            after_seek = player.stats()["present_count"]
            t1 = time.perf_counter()
            while time.perf_counter() - t1 < 1.0:
                player.pump_messages()
                player.ui_tick()
                player.take_ui_intents()
                time.sleep(0.02)
            check(f"{name}: presenting resumes after seek",
                  player.stats()["present_count"] > after_seek,
                  f"{after_seek} -> {player.stats()['present_count']}")
        except Exception as exc:
            check(f"{name}: player run", False, str(exc))
        finally:
            if player is not None:
                try:
                    player.close()
                except Exception:
                    pass

    if run_ai:
        if models is None or ai_spec is None:
            check("ai: long material available", False, "no AI scenario material")
            return
        run_ai_scenario(sumu_core, models, ai_spec)


def run_ai_scenario(sumu_core, models, spec):
    """Drive the real Scheduler on a material long enough for AI to actually run: cold_start_s=0
    (so no frames are skipped), a short clip, ~6 s of playback."""
    import logging
    import time
    from fractions import Fraction
    from sumu.ai.utils import VideoMetadata

    det, res, pad, Scheduler, SchedulerConfig = models
    name = spec["name"]
    print(f"\n-- ai scenario {name} --")
    player = None
    sched = None
    errors = []

    class _Capture(logging.Handler):
        def emit(self, record):
            if record.levelno >= logging.ERROR:
                errors.append(record.getMessage())

    handler = _Capture()
    scheduler_logger = logging.getLogger("sumu.scheduler")
    scheduler_logger.addHandler(handler)
    try:
        player = sumu_core.Player(1280, 720, True)
        player.open(spec["path"])
        # Metadata comes from the native decoder, never from ffprobe: for a rotated stream ffprobe
        # reports the coded size while the scheduler must use the normalized display size.
        fps = player.fps()
        frame_count = player.frame_count()
        width, height = player.dims()
        meta = VideoMetadata(
            video_file=spec["path"], video_height=height, video_width=width,
            video_fps=fps, average_fps=fps, video_fps_exact=Fraction(fps).limit_denominator(1001),
            codec_name="hevc", frames_count=frame_count,
            duration=frame_count / fps if fps > 0 else 0.0,
            time_base=Fraction(1, 1000), start_pts=0,
        )
        check(f"{name}: material is long enough for AI to process", frame_count >= 30,
              f"frames={frame_count} fps={fps:.3f}")

        config = SchedulerConfig()
        config.cold_start_s = 0.0   # skip nothing: 1.0s would swallow a short clip entirely
        config.clip_length = 3
        sched = Scheduler(player, det, res, pad, meta, config)
        sched.start()
        check(f"{name}: scheduler honours cold_start_s=0",
              sched.get_stats()["cold_start_skip_frames"] == 0,
              f"cold_start_skip_frames={sched.get_stats()['cold_start_skip_frames']}")

        player.play()
        t0 = time.perf_counter()
        while time.perf_counter() - t0 < 6.0:
            player.pump_messages()
            player.ui_tick()
            player.take_ui_intents()
            if player.stats()["current_frame"] >= frame_count - 2:
                break   # restart-free: the long material is ~6s, so play it once
            time.sleep(0.02)

        stats = sched.get_stats()
        play = player.stats()
        check(f"{name}: AI actually processed frames", stats["frames_detected"] > 0,
              f"frames_detected={stats['frames_detected']} frontier={stats['ai_frontier']} "
              f"current_frame={play['current_frame']}")
        note(f"{name}: synthetic colour patches do not require mosaic detections; "
             f"clips_restored={stats['clips_restored']}")
        player.pause()

        # Push a real frame back through the AI present bridge (get_cuda_nv12_by_frame ->
        # BGR -> RGBA -> push_ai_frame) to exercise the AI present path end to end. The material
        # is synthetic, so this proves the channel works and its colours survive the round trip
        # -- it does NOT prove mosaic restoration (that needs real mosaic content, covered by
        # scripts/run_player.py on the standard test video).
        fresh_before = player.stats()["n_ai_fresh"]
        pushed = _push_passthrough_frame(player, player.stats()["current_frame"])
        check(f"{name}: AI present bridge accepts a pushed frame", pushed,
              "push_ai_frame round trip")
        t1 = time.perf_counter()
        while time.perf_counter() - t1 < 1.0:
            player.pump_messages()
            player.ui_tick()
            player.take_ui_intents()
            time.sleep(0.02)
        after = player.stats()
        check(f"{name}: AI frames reach the present path",
              after["n_ai_fresh"] > fresh_before,
              f"n_ai_fresh {fresh_before} -> {after['n_ai_fresh']} "
              f"ai_push_count={after['ai_push_count']} ai_hit_rate={after['ai_hit_rate']:.3f}")
        check(f"{name}: scheduler reported no AI errors", not errors,
              "; ".join(errors[:3]) if errors else "")
    except Exception as exc:
        check(f"{name}: ai scenario run", False, str(exc))
    finally:
        scheduler_logger.removeHandler(handler)
        if sched is not None:
            try:
                sched.stop()
            except Exception:
                pass
        if player is not None:
            try:
                player.close()
            except Exception:
                pass


def _push_passthrough_frame(player, frame_num):
    """Feed the frame the AI bridge just handed out back in as an 'AI processed' frame, so the
    full RGBA path (packing + push_ai_frame + ready-map + present) is exercised for real."""
    import time
    import torch
    from sumu.ai.utils.video_utils import _nv12_to_bgr_hwc_gpu
    from sumu.ai.utils.cuda_dlpack import wrap_nv12_cuda_buffer_as_tensor

    deadline = time.perf_counter() + 2.0
    got = None
    while time.perf_counter() < deadline:
        candidate = player.get_cuda_nv12_by_frame(frame_num)
        if candidate.get("ready"):
            got = candidate
            break
        time.sleep(0.01)
    if got is None:
        return False
    nv12 = wrap_nv12_cuda_buffer_as_tensor(got["dev_ptr"], got["width"], got["height"],
                                           got["pitch_bytes"])
    bgr = _nv12_to_bgr_hwc_gpu(nv12, got["height"], got["width"], bt709=True, full_range=False)
    h, w = got["height"], got["width"]
    rgba = torch.empty((h, w, 4), dtype=torch.uint8, device=bgr.device)
    rgba[..., 0] = bgr[..., 2]
    rgba[..., 1] = bgr[..., 1]
    rgba[..., 2] = bgr[..., 0]
    rgba[..., 3] = 255
    before = player.stats()["ai_push_count"]
    player.push_ai_frame(frame_num, rgba.contiguous().data_ptr(), w, h, w * 4)
    return player.stats()["ai_push_count"] > before


# ============================================================================================
# entry point
# ============================================================================================

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--static-only", action="store_true", help="no GPU, no material generation")
    ap.add_argument("--no-gpu", action="store_true")
    ap.add_argument("--no-player", action="store_true")
    ap.add_argument("--ai", action="store_true",
                    help="also drive the real AI scheduler in the player section (slow)")
    ap.add_argument("--keep", action="store_true", help="keep the generated materials")
    ap.add_argument("--reuse", action="store_true", help="reuse already generated materials")
    args = ap.parse_args()

    sys.path.insert(0, os.path.join(_REPO, "python", "sumu"))
    sys.path.insert(0, os.path.join(_REPO, "python"))

    run_static()
    if args.static_only:
        return finish([])

    if not shutil.which("ffmpeg") or not shutil.which("ffprobe"):
        check("ffmpeg/ffprobe on PATH", False,
              "material generation and the ffmpeg reference need both tools")
        return finish([])

    specs = build_specs()
    run_materials(specs, args.reuse)
    ai_spec = build_ai_material(specs) if args.ai else None
    if not args.no_gpu:
        run_gpu(specs)
    if not args.no_player:
        run_player(specs, args.ai, ai_spec)
    if not args.keep:
        shutil.rmtree(MATERIAL_DIR, ignore_errors=True)
    return finish(specs)


def build_ai_material(specs):
    """The AI scenario needs more than the 6-frame colour materials: cold_start_s=0 with a short
    clip still needs 30+ frames of real playback. Loop the plain BT.709 material and scale it to
    640x360 so both dimensions clear clip_size (256) instead of relying on the pad path."""
    base = next(s for s in specs if s["name"] == "bt709_limited_8bit")
    spec = dict(base)
    spec["name"] = "ai_long"
    spec["path"] = os.path.join(MATERIAL_DIR, "ai_long.mp4")
    spec["src_width"], spec["src_height"] = 640, 360
    spec["dst_width"], spec["dst_height"] = 640, 360
    subprocess.run([
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-stream_loop", "12", "-i", base["path"],
        "-vf", "scale=640:360:flags=neighbor",
        "-c:v", "libx265", "-x265-params", "lossless=1:log-level=error",
        spec["path"],
    ], check=True)
    probe = json.loads(subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-count_frames", "-show_entries",
         "stream=nb_read_frames,width,height,color_space,color_range", "-print_format", "json",
         spec["path"]], check=True, capture_output=True, text=True).stdout)["streams"][0]
    frames = int(probe.get("nb_read_frames", 0))
    check("ai_long: material is usable for the AI scenario",
          frames >= 30 and (probe.get("width"), probe.get("height")) == (640, 360)
          and probe.get("color_space") == "bt709" and probe.get("color_range") == "tv",
          f"frames={frames} {probe.get('width')}x{probe.get('height')} "
          f"{probe.get('color_space')}/{probe.get('color_range')}")
    return spec


def finish(specs):
    print()
    if specs:
        out = os.path.join(_HERE, "trace", "verify_video_formats.json")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "w", encoding="utf-8") as fh:
            json.dump([{"name": s["name"], "summary": s.get("summary")} for s in specs], fh,
                      indent=2)
        print(f"result summary -> {out}")
    if _failures:
        print(f"\nFAILED ({len(_failures)}): " + "; ".join(_failures))
        return 1
    print("\nALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
