"""Decode CS view-space and Remix world-space diagnostic normal buffers."""

import numpy as np


def signed_oct_to_normal(encoded):
    encoded = np.asarray(encoded, dtype=np.float64)
    normal = np.concatenate((encoded, (1 - np.abs(encoded).sum(axis=-1))[..., None]), axis=-1)
    t = np.maximum(-normal[..., 2:3], 0)
    normal[..., :2] += np.where(normal[..., :2] >= 0, -t, t)
    return normal / np.maximum(np.linalg.norm(normal, axis=-1, keepdims=True), 1e-12)


def native_world_normals(pixels, row_major_view):
    # Common/GBuffer.hlsli DecodeNormal negates the decoded octahedron.
    view_normal = -signed_oct_to_normal(pixels[..., :2] * 2 - 1)
    rotation = np.asarray(row_major_view).reshape(4, 4)[:3, :3]
    return view_normal @ rotation.T


def remix_world_normals(pixels):
    # packing_helpers.h uses biased unsigned storage, NOT Vulkan's two's-
    # complement SNORM: 0 -> -1, 32767 -> 0, 65534 -> +1.
    packed = pixels[..., 0].astype(np.uint32)
    words = np.stack((packed & 0xFFFF, packed >> 16), axis=-1)
    return signed_oct_to_normal(words.astype(np.float64) / 65534 * 2 - 1)


def angle_degrees(first, second):
    return np.degrees(np.arccos(np.clip(np.sum(first * second, axis=-1), -1, 1)))
