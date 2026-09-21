"""Synthetic packed-value fixtures for the diagnostic DDS reader."""

import struct
import tempfile
import unittest
from pathlib import Path

import numpy as np

from ReadBuffers import read_dds
from NormalBuffers import angle_degrees, native_world_normals, remix_world_normals, signed_oct_to_normal


def fixture(payload, fourcc=b"DX10", fmt=24, width=1, height=1, pitch=4):
    header = bytearray(148 if fourcc in (b"DX10", b"GLI1") else 128)
    header[:4] = b"DDS "
    struct.pack_into("<I", header, 4, 124)
    struct.pack_into("<3I", header, 12, height, width, pitch)
    code = int.from_bytes(fourcc, "little") if isinstance(fourcc, bytes) else fourcc
    masks = (0x3FF00000, 0xFFC00, 0x3FF, 0xC0000000)
    struct.pack_into("<7I", header, 80, 4, code, 32, *masks)
    if len(header) == 148:
        struct.pack_into("<5I", header, 128, fmt, 3, 0, 1, 0)
    return header + payload


class ReaderTests(unittest.TestCase):
    def read(self, data, **kwargs):
        with tempfile.TemporaryDirectory(prefix="remix-dds-test-") as directory:
            path = Path(directory) / "fixture.dds"
            path.write_bytes(data)
            return read_dds(path, **kwargs)

    def test_dx10_channel_order(self):
        pixels, _ = self.read(fixture(struct.pack("<I", 1023 | (511 << 10) | (7 << 20))))
        np.testing.assert_allclose(pixels[0, 0], [1, 511 / 1023, 7 / 1023])

    def test_gli_header_and_explicit_exporter_correction(self):
        data = fixture(struct.pack("<I", 1023 | (511 << 10) | (7 << 20)), b"GLI1", 56)
        standard, _ = self.read(data)
        corrected, info = self.read(data, remix_vulkan_packing=True)
        np.testing.assert_allclose(standard[0, 0], [7 / 1023, 511 / 1023, 1])
        np.testing.assert_allclose(corrected[0, 0], [1, 511 / 1023, 7 / 1023])
        self.assertTrue(info["remixPackingCorrection"])
        self.assertIsNone(info["dxgiFormat"])

    def test_stencil_is_not_depth(self):
        pixels, _ = self.read(fixture(struct.pack("<I", 0xAB800000), fmt=44))
        self.assertAlmostEqual(pixels[0, 0, 0], 0x800000 / 0xFFFFFF)

    def test_legacy_float(self):
        pixels, _ = self.read(fixture(struct.pack("<f", 1234.5), fourcc=114))
        self.assertEqual(pixels[0, 0, 0], 1234.5)

    def test_padded_rows(self):
        data = fixture(struct.pack("<4I", 1023, 0xFFFFFFFF, 511, 0xFFFFFFFF), height=2, pitch=8)
        pixels, info = self.read(data)
        np.testing.assert_allclose(pixels[:, 0, 0], [1, 511 / 1023])
        self.assertEqual(info["rowPitch"], 8)

    def test_gli_placeholder_pitch(self):
        data = fixture(struct.pack("<2I", 1023, 511), b"GLI1", 56, width=2, pitch=32)
        pixels, info = self.read(data, remix_vulkan_packing=True)
        np.testing.assert_allclose(pixels[0, :, 0], [1, 511 / 1023])
        self.assertEqual(info["rowPitch"], 8)

    def test_truncated_payload_and_header(self):
        with self.assertRaisesRegex(ValueError, "Truncated DDS payload"):
            self.read(fixture(b"\x00\x00"))
        with self.assertRaisesRegex(ValueError, "Truncated DDS extended header"):
            self.read(fixture(b"")[:140])
        with self.assertRaisesRegex(ValueError, "Truncated DDS payload"):
            self.read(fixture(b"\x00" * 12, height=2, pitch=8))

    def test_unknown_gli_rejected(self):
        with self.assertRaisesRegex(ValueError, "Unsupported GLI"):
            self.read(fixture(b"\x00" * 4, b"GLI1", 57))

    def test_normal_oct_axes_and_negative_hemisphere(self):
        encoded = np.array([[0, 0], [1, 0], [0, -1], [1, 1]])
        expected = np.array([[0, 0, 1], [1, 0, 0], [0, -1, 0], [0, 0, -1]])
        np.testing.assert_allclose(signed_oct_to_normal(encoded), expected)

    def test_remix_normal_signed_packing(self):
        packed = np.array([[0x7FFF7FFF], [0x7FFFFFFE], [0x00007FFF], [0xFFFEFFFE]], dtype=np.uint32)
        expected = np.array([[0, 0, 1], [1, 0, 0], [0, -1, 0], [0, 0, -1]])
        np.testing.assert_allclose(remix_world_normals(packed), expected)

    def test_native_normal_sign_and_view_rotation(self):
        pixels = np.array([[.5, .5, 1], [1, .5, 1]])
        rotation = np.array([[0, 1, 0, 0], [-1, 0, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]])
        np.testing.assert_allclose(native_world_normals(pixels, rotation.ravel()), [[0, 0, -1], [0, 1, 0]])

    def test_normal_angles(self):
        np.testing.assert_allclose(angle_degrees(np.array([[0, 0, 1]] * 3),
                                                np.array([[0, 0, 1], [1, 0, 0], [0, 0, -1]])), [0, 90, 180])


if __name__ == "__main__":
    unittest.main()
