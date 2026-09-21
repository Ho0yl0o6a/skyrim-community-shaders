"""Read uncompressed diagnostic DDS buffers without applying an implicit gamma transform."""

import argparse
import json
import struct
from pathlib import Path

import numpy as np
from PIL import Image


def read_dds(path, remix_vulkan_packing=False):
    data = Path(path).read_bytes()
    if data[:4] != b"DDS " or len(data) < 128:
        raise ValueError(f"Not a DDS: {path}")
    height, width, pitch = struct.unpack_from("<3I", data, 12)
    if not height or not width:
        raise ValueError("Invalid DDS dimensions")
    flags, fourcc, bits, *masks = struct.unpack_from("<7I", data, 80)
    offset = 128
    fmt = None
    gli_format = None
    if fourcc in (struct.unpack("<I", b"DX10")[0], struct.unpack("<I", b"GLI1")[0]):
        if len(data) < 148:
            raise ValueError("Truncated DDS extended header")
        extension_format, dimension, misc, array_size, _ = struct.unpack_from("<5I", data, 128)
        if dimension != 3 or array_size != 1:
            raise ValueError("Only single 2D diagnostic buffers are supported")
        if fourcc == struct.unpack("<I", b"GLI1")[0]:
            gli_format = extension_format
            if gli_format != 56 or bits != 32 or masks != [0x3FF00000, 0xFFC00, 0x3FF, 0xC0000000]:
                raise ValueError(f"Unsupported GLI format: {gli_format}")
            # AssetExporter casts VkFormat 64 to GLI's BGR10A2 enum, then
            # copies A2B10G10R10 bytes unchanged. Only opt in for that exporter.
            if remix_vulkan_packing:
                masks = [0x3FF, 0xFFC00, 0x3FF00000, 0xC0000000]
        else:
            fmt = extension_format
        offset = 148
    if fmt in (24, 44, 45, 46, 39, 40, 41, 42, 28, 29, 87, 91):
        bytes_per_pixel = 4
    elif fmt in (10, 11, 16):
        bytes_per_pixel = 8
    elif fmt == 2:
        bytes_per_pixel = 16
    elif gli_format == 56 or (fmt is None and flags & 0x40 and bits == 32):
        bytes_per_pixel = 4
    elif fmt is None and fourcc in (113, 114, 116):
        bytes_per_pixel = {113: 8, 114: 4, 116: 16}[fourcc]
    else:
        raise ValueError(f"Unsupported DDS format: DXGI={fmt}, fourCC={fourcc}, bits={bits}")
    row_bytes = width * bytes_per_pixel
    gli_placeholder_pitch = gli_format is not None and pitch == 32
    row_pitch = pitch if pitch >= row_bytes and not gli_placeholder_pitch else row_bytes
    if offset + row_pitch * height > len(data):
        raise ValueError("Truncated DDS payload")
    rows = np.frombuffer(data, np.uint8, count=row_pitch * height, offset=offset).reshape(height, row_pitch)
    payload = rows[:, :row_bytes].copy()
    words = payload.view("<u4").reshape(height, width, -1)
    if fmt == 24:
        packed = words[..., 0]
        pixels = np.stack([(packed >> s) & 1023 for s in (0, 10, 20)], axis=-1).astype(np.float32) / 1023.0
    elif fmt in (44, 45, 46):
        pixels = ((words[..., 0] & 0xFFFFFF).astype(np.float64) / 16777215.0)[..., None]
    elif fmt in (39, 40, 41) or (fmt is None and fourcc == 114):
        pixels = payload.view("<f4").reshape(height, width, 1)
    elif fmt == 42:
        pixels = words.astype(np.float64)
    elif fmt in (10, 11) or (fmt is None and fourcc == 113):
        pixels = payload.view("<f2" if fmt != 11 else "<u2").reshape(height, width, 4).astype(np.float32)
        if fmt == 11:
            pixels /= 65535.0
    elif fmt in (2, 16) or (fmt is None and fourcc == 116):
        pixels = payload.view("<f4").reshape(height, width, bytes_per_pixel // 4)
    elif fmt in (28, 29, 87, 91):
        pixels = payload.reshape(height, width, 4).astype(np.float32) / 255.0
        if fmt in (87, 91):
            pixels = pixels[..., [2, 1, 0, 3]]
    else:
        packed = words[..., 0]
        channels = []
        for mask in masks[:3]:
            if not mask:
                channels.append(np.zeros_like(packed, dtype=np.float32))
                continue
            shift = (mask & -mask).bit_length() - 1
            channels.append(((packed & mask) >> shift).astype(np.float32) / (mask >> shift))
        pixels = np.stack(channels, axis=-1)
    return pixels, {"width": width, "height": height, "dxgiFormat": fmt, "gliFormat": gli_format,
                    "fourCC": fourcc, "rowPitch": row_pitch,
                    "remixPackingCorrection": gli_format == 56 and remix_vulkan_packing}


def summarize(path, preview=False, remix_vulkan_packing=False):
    pixels, info = read_dds(path, remix_vulkan_packing)
    channels = pixels.reshape(-1, pixels.shape[-1])
    info.update(path=str(path), finite=bool(np.isfinite(pixels).all()),
                percentiles=np.nanpercentile(channels, [0, 10, 50, 90, 99, 100], axis=0).tolist())
    if preview and pixels.shape[-1] >= 3:
        target = Path(path).with_suffix(".raw.png")
        Image.fromarray(np.rint(np.clip(pixels[..., :3], 0, 1) * 255).astype(np.uint8)).save(target)
        info["preview"] = str(target)
        info["previewEncoding"] = "Raw values mapped to 8-bit, no gamma or tone mapping; not a lit image."
    return info


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", type=Path, nargs="+")
    parser.add_argument("--preview", action="store_true")
    parser.add_argument("--remix-vulkan-packing", action="store_true",
                        help="Correct the known VkFormat-to-GLI packed-10 exporter mismatch")
    args = parser.parse_args()
    print(json.dumps([summarize(p, args.preview, args.remix_vulkan_packing) for p in args.files], indent=2))
