"""Inspect hardware/reconstructed hit diagnostics in a registered native pair."""

import argparse
import json
from pathlib import Path

import numpy as np

from CameraBuffers import coincident_pixel_centres, native_pixel_coordinates
from CompareBuffers import validate_capture_pair
from ReadBuffers import read_dds


def compare(directory):
    native = json.loads(next(directory.glob('native-*.json')).read_text())
    target = json.loads(next(directory.glob('rtx-*.json')).read_text())
    manifest = json.loads((directory / 'manifest.json').read_text(encoding='utf-8-sig'))
    if not validate_capture_pair(native, target) or manifest['debugView'] != 817:
        raise ValueError('Registered Pair817 required')
    if manifest['cameraBefore'] != manifest['cameraAfter']:
        raise ValueError('Camera moved')
    depth_path = next(directory.glob('gbufferLinearZ_*.dds'))
    runtime = json.loads(Path(str(depth_path) + '.camera.json').read_text())
    if runtime['depthBuffer'] != depth_path.name:
        raise ValueError('Wrong camera sidecar')
    nd, _ = read_dds(next(directory.glob('*-depth.dds')))
    debug, _ = read_dds(next(directory.glob('rtxImageDebugView_*.dds')))
    if nd.shape[:2] != (1080, 1920) or debug.shape != (1080, 1920, 4):
        raise ValueError('Expected full-resolution RGBA diagnostic')
    x, y = native_pixel_coordinates(native, runtime, debug.shape[:2], nd.shape[:2])
    if not coincident_pixel_centres(x, y, nd.shape[:2]):
        raise ValueError('Primary rays differ')
    p = np.asarray(native['shadowProjection'], dtype=np.float32).astype(np.float64)
    nz = p[14] / (nd[..., 0] - p[10])
    fields = {'positionError3D': debug[..., 0], 'hardwareDepth': debug[..., 1],
              'reconstructedDepth': debug[..., 2],
              'nativeDepth': nz, 'hardwareMinusNativeDepth': debug[..., 1] - nz,
              'reconstructedMinusNativeDepth': debug[..., 2] - nz}
    result = {'directory': str(directory.resolve()),
              'scope': 'Manually selected face probes; verify view visually; no fidelity gate. Exported debug alpha is not preserved.',
              'pair': {key: native[key] for key in ('pid', 'frame', 'request')},
              'regions': {}}
    for name, (x0, y0, x1, y1) in {
        'band': (725, 378, 770, 386), 'forehead': (725, 350, 770, 360),
        'face': (670, 310, 800, 510),
    }.items():
        roi = np.s_[y0:y1, x0:x1]
        result['regions'][name] = {'boundsXYXYExclusive': [x0, y0, x1, y1],
            'minMedianP99Max': {key: np.percentile(value[roi], [0, 50, 99, 100]).tolist()
                               for key, value in fields.items()}}
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
