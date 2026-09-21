"""Frozen face-layer diagnostics; not character fidelity certification."""

import argparse
import json
from pathlib import Path

import numpy as np

from CompareHitBuffers import compare
from CompareBuffers import validate_capture_pair
from ReadBuffers import read_dds


def load(directory, view):
    manifest = json.loads((directory / 'manifest.json').read_text(encoding='utf-8-sig'))
    if manifest['debugView'] != view or manifest['cameraBefore'] != manifest['cameraAfter']:
        raise ValueError('Wrong debug view or moved camera')
    debug, _ = read_dds(next(directory.glob('rtxImageDebugView_*.dds')))
    return manifest, debug


def percentiles(values):
    return np.percentile(values, [0, 50, 99, 100]).tolist() if values.size else None


def compare_layers(final, first, remix_only=None, next_hit=None):
    final_report, first_report = compare(final), compare(first)
    if final_report['pair']['pid'] != first_report['pair']['pid']:
        raise ValueError('Cross-launch pose comparisons are unsupported')
    manifest, hit = load(final, 817)
    first_manifest, first_hit = load(first, 817)
    if manifest['cameraBefore'] != first_manifest['cameraBefore']:
        raise ValueError('First/final camera mismatch')
    native = json.loads(next(final.glob('native-*.json')).read_text())
    nd, _ = read_dds(next(final.glob('*-depth.dds')))
    first_nd, _ = read_dds(next(first.glob('*-depth.dds')))
    projection = np.asarray(native['shadowProjection'], dtype=np.float32).astype(np.float64)
    native_z = projection[14] / (nd[..., 0] - projection[10])
    mask = np.zeros(native_z.shape, dtype=bool)
    mask[310:560, 670:810] = True
    foreground = mask & (native_z < 100)
    mask = foreground & (hit[..., 1] - native_z > 5)
    raw_depth, _ = read_dds(next(final.glob('gbufferLinearZ_*.dds')))
    raw_delta = raw_depth[..., 0] - native_z
    if not np.array_equal(nd[..., 0][mask], first_nd[..., 0][mask]):
        raise ValueError('Native pose/coverage changed at failed samples')
    result = {
        'scope': 'Manual face ROI [670,310,810,560], native Z<100, final Z error>5; no parity gate. '
                 'First/final different frames: camera and native depth at selected pixels must match. '
                 'Debug is float16; alpha is not preserved and large IDs are quantized.',
        'final': str(final.resolve()), 'first': str(first.resolve()),
        'pid': final_report['pair']['pid'], 'failedSamples': int(mask.sum()),
        'hasFailureSamplesForCrossFrameComparison': bool(mask.any()),
        'rawDepthDiagnostics': {
            'scope': 'Same manual native-foreground ROI, INCLUDING RTX misses. Debug817 alone cannot count misses.',
            'samples': int(foreground.sum()),
            'depthErrorOver5': int(np.sum(foreground & (raw_delta > 5))),
            'absoluteDepthErrorOver1': int(np.sum(foreground & (np.abs(raw_delta) > 1))),
            'minMedianP99Max': percentiles(raw_delta[foreground]),
        },
        'minMedianP99Max': {
            'finalPositionError3D': percentiles(hit[..., 0][mask]),
            'finalDepthMinusNative': percentiles((hit[..., 1] - native_z)[mask]),
            'firstDepthMinusNative': percentiles((first_hit[..., 1] - native_z)[mask]),
            'firstDepthMinusFinal': percentiles((first_hit[..., 1] - hit[..., 1])[mask]),
        },
    }
    if remix_only:
        other_manifest, other_hit = load(remix_only, 817)
        if (manifest['cameraBefore'] != other_manifest['cameraBefore'] or
                manifest['state']['pid'] != other_manifest['state']['pid']):
            raise ValueError('Remix-only camera mismatch')
        result['remixOnly'] = str(remix_only.resolve())
        result['equalRemixOnlyRGBAtFailedSamples'] = bool(np.array_equal(hit[mask, :3], other_hit[mask, :3]))
    if next_hit:
        other_manifest, other_hit = load(next_hit, 819)
        next_native = json.loads(next(next_hit.glob('native-*.json')).read_text())
        next_target = json.loads(next(next_hit.glob('rtx-*.json')).read_text())
        if not validate_capture_pair(next_native, next_target):
            raise ValueError('Next-hit probe requires a registered native pair')
        if (manifest['cameraBefore'] != other_manifest['cameraBefore'] or
                final_report['pair']['pid'] != next_native['pid']):
            raise ValueError('Next-hit camera mismatch')
        next_nd, _ = read_dds(next(next_hit.glob('*-depth.dds')))
        if not np.array_equal(nd[..., 0][mask], next_nd[..., 0][mask]):
            raise ValueError('Native pose/coverage changed in next-hit probe')
        result['nextHit'] = str(next_hit.resolve())
        valid = mask & (other_hit[..., 2] >= 0)
        result['nextHitRepeatedPrimitiveSamples'] = int(np.sum(mask & (other_hit[..., 2] == -2)))
        result['nextHitMissSamples'] = int(np.sum(mask & (other_hit[..., 2] == -1)))
        result['nextHitValidSamples'] = int(valid.sum())
        if valid.any():
            result['nextHitMinMedianP99Max'] = {
                'depthMinusNative': percentiles((other_hit[..., 0] - native_z)[valid]),
                'distanceFromFirstHit': percentiles(other_hit[..., 1][valid]),
            }
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('final', type=Path)
    parser.add_argument('first', type=Path)
    parser.add_argument('--remix-only', type=Path)
    parser.add_argument('--next-hit', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    report = json.dumps(compare_layers(args.final, args.first, args.remix_only, args.next_hit),
                        indent=2, allow_nan=False)
    if args.output:
        args.output.write_text(report + '\n', encoding='utf-8')
    print(report)
