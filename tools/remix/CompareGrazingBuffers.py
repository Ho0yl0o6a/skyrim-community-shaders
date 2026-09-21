"""Same-frame normal/facing diagnostics; regions are probes, not parity gates."""

import argparse
import json
from pathlib import Path

import numpy as np

from CameraBuffers import coincident_pixel_centres, native_pixel_coordinates
from CompareBuffers import validate_capture_pair
from NormalBuffers import angle_degrees, native_world_normals, remix_world_normals
from ReadBuffers import read_dds


def view_directions(runtime, shape):
    h, w = shape
    yy, xx = np.indices(shape)
    clip = np.stack((2 * (xx + .5) / w - 1, 1 - 2 * (yy + .5) / h,
                     np.ones(shape), np.ones(shape)), axis=-1)
    inverse = np.asarray(runtime['projectionToViewJittered']).reshape(4, 4).T
    points = clip @ inverse.T
    directions = points[..., :3] / points[..., 3:4]
    rotation = np.asarray(runtime['viewToWorld']).reshape(4, 4).T[:3, :3]
    directions = directions @ rotation.T
    return -directions / np.linalg.norm(directions, axis=-1, keepdims=True)


def compare(directory, baseline_directory=None):
    native = json.loads(next(directory.glob('native-*.json')).read_text())
    target = json.loads(next(directory.glob('rtx-*.json')).read_text())
    manifest = json.loads((directory / 'manifest.json').read_text(encoding='utf-8-sig'))
    if not validate_capture_pair(native, target) or manifest['debugView'] != 808:
        raise ValueError('Same-frame debug808 pair required')
    if manifest['cameraBefore'] != manifest['cameraAfter']:
        raise ValueError('Camera moved during capture')
    depth_path = next(directory.glob('gbufferLinearZ_*.dds'))
    runtime = json.loads(Path(str(depth_path) + '.camera.json').read_text())
    if runtime['depthBuffer'] != depth_path.name:
        raise ValueError('Camera sidecar mismatch')
    nd, _ = read_dds(next(directory.glob('*-depth.dds')))
    rd, _ = read_dds(depth_path)
    x, y = native_pixel_coordinates(native, runtime, rd.shape[:2], nd.shape[:2])
    if not coincident_pixel_centres(x, y, nd.shape[:2]):
        raise ValueError('Primary rays differ')
    nn, _ = read_dds(next(directory.glob('*-normalRoughness.dds')))
    rn, _ = read_dds(next(directory.glob('gbufferWorldNormals_*.dds')))
    debug, _ = read_dds(next(directory.glob('rtxImageDebugView_*.dds')))
    if any(a.shape[:2] != nd.shape[:2] for a in (nn, rn, debug)):
        raise ValueError('Buffer extents differ')
    nnormal = native_world_normals(nn, native['shadowView'])
    rnormal = remix_world_normals(rn)
    view = view_directions(runtime, rd.shape[:2])
    n_dot = np.sum(nnormal * view, axis=-1)
    r_dot = np.sum(rnormal * view, axis=-1)
    angle = angle_degrees(nnormal, rnormal)
    p = np.asarray(native['shadowProjection'], dtype=np.float32).astype(np.float64)
    delta = np.abs(rd[..., 0] - p[14] / (nd[..., 0] - p[10]))
    red = (debug[..., 0] > .9) & (debug[..., 1] < .1) & (debug[..., 2] < .1)
    green = (debug[..., 1] > .9) & (debug[..., 0] < .1) & (debug[..., 2] < .1)
    opaque = red | green
    yy, xx = np.indices(opaque.shape)
    # Explicit current-view probe, not a material mask or universal region.
    foreground = (xx >= 1000) & (yy >= 800)
    masks = {'opaque': opaque, 'red': red, 'foregroundOpaque': foreground & opaque,
             'foregroundRed': foreground & red}
    if baseline_directory is not None:
        baseline = json.loads((baseline_directory / 'manifest.json').read_text(encoding='utf-8-sig'))
        if (baseline['debugView'] != 808 or baseline['cameraBefore'] != manifest['cameraBefore'] or
                baseline['cameraAfter'] != manifest['cameraBefore']):
            raise ValueError('Baseline camera/debug view differs')
        previous, _ = read_dds(next(baseline_directory.glob('rtxImageDebugView_*.dds')))
        if previous.shape != debug.shape:
            raise ValueError('Baseline extent differs')
        prior_red = (previous[..., 0] > .9) & (previous[..., 1] < .1) & (previous[..., 2] < .1)
        masks['priorRed'] = prior_red
        masks['foregroundPriorRed'] = foreground & prior_red
    masks.update({key + 'DepthPoint1Diagnostic': value & (delta < .1)
                  for key, value in list(masks.items())})
    result = {'scope': 'Same-frame registered diagnostic only; red is Ns.V < materialEpsilon, not necessarily negative.',
              'pair': {key: native[key] for key in ('pid', 'frame', 'request')},
              'camera': manifest['cameraBefore'], 'regions': {}}
    for key, mask in masks.items():
        record = {'pixels': int(mask.sum())}
        if mask.any():
            record.update({
                'nativeNdotV_P0P1P50P99P100': np.percentile(n_dot[mask], [0, 1, 50, 99, 100]).tolist(),
                'remixNdotV_P0P1P50P99P100': np.percentile(r_dot[mask], [0, 1, 50, 99, 100]).tolist(),
                'nativeNegativeFraction': float(np.mean(n_dot[mask] < 0)),
                'remixNegativeFraction': float(np.mean(r_dot[mask] < 0)),
                'angleP50P90P99Max': np.percentile(angle[mask], [50, 90, 99, 100]).tolist(),
                'absoluteDepthP50P90P99Max': np.percentile(delta[mask], [50, 90, 99, 100]).tolist()})
        result['regions'][key] = record
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--baseline-directory', type=Path)
    args = parser.parse_args()
    report = json.dumps(compare(args.directory, args.baseline_directory), indent=2, allow_nan=False)
    if args.output:
        args.output.write_text(report + '\n', encoding='utf-8')
    print(report)
