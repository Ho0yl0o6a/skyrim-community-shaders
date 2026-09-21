"""Measure composed host bone motion, not GPU vertices or display pacing."""
import argparse
import json
from pathlib import Path

import numpy as np


def world_bone_origins(pose):
    rotation = np.asarray(pose['rotation']).reshape(3, 3)
    if not pose['bones']:
        return np.asarray([pose['world'][:3]])
    bones = np.asarray(pose['bones']).reshape(-1, 3, 4)
    return np.asarray(pose['world'][:3]) + pose['world'][3] * (bones[:, :, 3] @ rotation.T)


def world_bone_rotations(pose):
    rotation = np.asarray(pose['rotation']).reshape(3, 3)
    if not pose['bones']:
        return np.asarray([pose['world'][3] * rotation])
    bones = np.asarray(pose['bones']).reshape(-1, 3, 4)
    return pose['world'][3] * (rotation @ bones[:, :, :3])


def measure(directory):
    sidecars = [json.loads(p.read_text()) for p in sorted(directory.glob('*.png.json'))]
    audit = json.loads((directory / 'pose-audit.json').read_text())
    rows = {row['frame']: row for row in audit['frames']}
    if not sidecars or any(s['frame'] not in rows for s in sidecars):
        raise ValueError('Incomplete pose coverage')
    changes = []
    for prev, curr in zip(sidecars, sidecars[1:]):
        if curr['frame'] != prev['frame'] + 1:
            raise ValueError('Nonconsecutive captured frames')
        dt = (curr['captureTimeUs'] - prev['captureTimeUs']) / 1e6
        if dt <= 0:
            raise ValueError('Non-increasing capture timestamps')
        old = {p['geometry']: p for p in rows[prev['frame']]['poses']}
        for p in rows[curr['frame']]['poses']:
            q = old.get(p['geometry'])
            if not q or not p['submitted'] or not q['submitted'] or len(p['bones']) != len(q['bones']):
                continue
            delta = np.linalg.norm(world_bone_origins(p) - world_bone_origins(q), axis=1)
            record = dict(frame=curr['frame'], name=p['name'], geometry=p['geometry'],
                          viewModel=p.get('viewModel', False), rigid=not bool(p['bones']),
                          dtMs=dt * 1000, maxBoneOriginStep=float(delta.max()),
                          maxBoneOriginSpeed=float(delta.max() / dt),
                          rootStep=float(np.linalg.norm(np.asarray(p['world'][:3]) - q['world'][:3])))
            record['maxBoneRotationDelta'] = float(np.abs(world_bone_rotations(p) - world_bone_rotations(q)).max())
            record['bonePhases'] = rows[curr['frame']].get('bonePhases')
            native = {n['geometry']: n for n in rows[curr['frame']].get('nativeViewModelPoses', [])}.get(p['geometry'])
            native_old = {n['geometry']: n for n in rows[prev['frame']].get('nativeViewModelPoses', [])}.get(p['geometry'])
            if native and native_old and len(native['rotations']) == len(native_old['rotations']):
                # Native values omit the constant skin-to-bone bind transform.
                # Compare consecutive native samples, not native versus palette.
                a, b = np.asarray(native['rotations']), np.asarray(native_old['rotations'])
                if a.size and a.shape == b.shape:
                    record['nativeCameraBoneRotationDelta'] = float(np.abs(a - b).max())
            eye_key = 'viewModelEyeWorld' if p.get('viewModel') else 'eyeWorld'
            valid_key = 'viewModelCameraValid' if p.get('viewModel') else 'cameraValid'
            if rows[curr['frame']].get(valid_key) and rows[prev['frame']].get(valid_key):
                camera_step = np.asarray(rows[curr['frame']][eye_key]) - rows[prev['frame']][eye_key]
                relative = world_bone_origins(p) - world_bone_origins(q) - camera_step
                record['maxCameraRelativeBoneStep'] = float(np.linalg.norm(relative, axis=1).max())
            if 'entryWorld' in p:
                record['entryToSubmittedTranslation'] = float(np.linalg.norm(np.asarray(p['world'][:3]) - p['entryWorld'][:3]))
                record['entryToSubmittedRotation'] = float(np.abs(np.asarray(p['rotation']) - p['entryRotation']).max())
            if record['rigid'] and 'attachmentWorld' in p:
                record['attachmentToSubmittedTranslation'] = float(np.linalg.norm(np.asarray(p['world'][:3]) - p['attachmentWorld'][:3]))
                record['attachmentToSubmittedRotation'] = float(np.abs(np.asarray(p['rotation']) - p['attachmentRotation']).max())
            changes.append(record)
    return dict(scope=__doc__ + ' Bone origins are bind-transform probes, not animated vertex positions. Timestamps are copy-queue times, not engine update times.',
                coveredFrames=len(sidecars), changes=changes)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    result = measure(args.directory)
    (args.directory / 'motion-analysis.json').write_text(json.dumps(result, indent=2))
    for name, geometry in sorted({(r['name'], r['geometry']) for r in result['changes']}):
        rows = [r for r in result['changes'] if r['geometry'] == geometry]
        worst = max(rows, key=lambda r: r['maxBoneOriginStep'])
        print(json.dumps(dict(name=name, geometry=geometry, transitions=len(rows), worst=worst,
                              repeatedRotations=[r['frame'] for r in rows if r['maxBoneRotationDelta'] < 1e-5])))
