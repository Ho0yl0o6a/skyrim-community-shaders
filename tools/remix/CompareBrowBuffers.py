"""Registered face-band probes, not a whole-character fidelity test."""

import argparse
import json
from pathlib import Path

import numpy as np

from CameraBuffers import coincident_pixel_centres, native_pixel_coordinates
from CompareBuffers import validate_capture_pair
from CompareGrazingBuffers import view_directions
from NormalBuffers import angle_degrees, native_world_normals, remix_world_normals
from ReadBuffers import read_dds


def compare(directory):
    native = json.loads(next(directory.glob('native-*.json')).read_text())
    target = json.loads(next(directory.glob('rtx-*.json')).read_text())
    manifest = json.loads((directory / 'manifest.json').read_text(encoding='utf-8-sig'))
    if not validate_capture_pair(native, target) or manifest['debugView'] != 0:
        raise ValueError('Same-frame lit pair required')
    if manifest['cameraBefore'] != manifest['cameraAfter']:
        raise ValueError('Camera moved')
    depth_path = next(directory.glob('gbufferLinearZ_*.dds'))
    runtime = json.loads(Path(str(depth_path) + '.camera.json').read_text())
    if runtime['depthBuffer'] != depth_path.name:
        raise ValueError('Camera sidecar mismatch')
    nd, _ = read_dds(next(directory.glob('*-depth.dds')))
    rd, _ = read_dds(depth_path)
    if nd.shape[:2] != (1080, 1920) or rd.shape[:2] != nd.shape[:2]:
        raise ValueError('These manually selected ROIs require 1920x1080')
    x, y = native_pixel_coordinates(native, runtime, rd.shape[:2], nd.shape[:2])
    if not coincident_pixel_centres(x, y, nd.shape[:2]):
        raise ValueError('Primary rays differ')
    nn, _ = read_dds(next(directory.glob('*-normalRoughness.dds')))
    rn, _ = read_dds(next(directory.glob('gbufferWorldNormals_*.dds')))
    if any(a.shape[:2] != nd.shape[:2] for a in (nn, rn)):
        raise ValueError('Normal extents differ')
    nn = native_world_normals(nn, native['shadowView'])
    rn = remix_world_normals(rn)
    view = view_directions(runtime, rd.shape[:2])
    projection = np.asarray(native['shadowProjection'], dtype=np.float32).astype(np.float64)
    nz = projection[14] / (nd[..., 0] - projection[10])
    fields = {
        'nativeNdotV': np.sum(nn * view, axis=-1),
        'remixNdotV': np.sum(rn * view, axis=-1),
        'normalAngleDegrees': angle_degrees(nn, rn),
        'nativeDepth': nz,
        'remixDepth': rd[..., 0],
        'remixMinusNativeDepth': rd[..., 0] - nz,
    }
    radiance = {}
    for name in ('gbufferAlbedo', 'noisyDiffuse', 'noisySpecular',
                 'denoisedDiffuse', 'denoisedSpecular', 'rtxImagePostComposite'):
        radiance[name], _ = read_dds(next(directory.glob(name + '_*.dds')),
                                    remix_vulkan_packing=name == 'gbufferAlbedo')
        if radiance[name].shape[:2] != nd.shape[:2]:
            raise ValueError('Radiance extent differs')
    result = {
        'scope': 'Explicit manually selected face-band/control probes; verify framing visually. No depth gating.',
        'directory': str(directory.resolve()),
        'pair': {key: native[key] for key in ('pid', 'frame', 'request')},
        'camera': manifest['cameraBefore'],
        'regions': {},
    }
    for name, (x0, y0, x1, y1) in {
        'band': (725, 378, 770, 386), 'forehead': (725, 350, 770, 360)
    }.items():
        roi = np.s_[y0:y1, x0:x1]
        result['regions'][name] = {
            'boundsXYXYExclusive': [x0, y0, x1, y1],
            'pixels': (x1 - x0) * (y1 - y0),
            'minMedianMax': {key: np.percentile(value[roi], [0, 50, 100]).tolist()
                             for key, value in fields.items()},
            'channelMedians': {key: np.median(value[roi], axis=(0, 1)).tolist()
                               for key, value in radiance.items()},
        }
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    report = json.dumps(compare(args.directory), indent=2, allow_nan=False)
    if args.output:
        args.output.write_text(report + '\n', encoding='utf-8')
    print(report)
