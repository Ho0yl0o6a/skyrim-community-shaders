"""Compare same-frame native camera values with host Remix submission, not GPU/display pacing."""
import argparse
import json
from pathlib import Path

import numpy as np


def compare(rows):
    result = []
    previous = None
    for row in rows:
        if not row.get('cameraValid') or not row.get('nativeCameraPositionsValid'):
            raise ValueError('Missing current native/submitted camera')
        submitted = np.asarray(row['eyeWorld'])
        native = np.asarray(row['nativeSceneCameraPosition'])
        player = np.asarray(row['nativePlayerPosition'])
        restored = np.asarray(row['nativeCameraOrigin']) + row['nativeRelativeEye']
        item = dict(frame=row['frame'], submittedEye=submitted.tolist(), nativeSceneEye=native.tolist(),
                    nativePlayer=player.tolist(), restoreError=float(np.linalg.norm(submitted - restored)),
                    nativeSceneError=float(np.linalg.norm(submitted - native)))
        if previous is not None:
            if row['frame'] != previous['frame'] + 1:
                raise ValueError('Nonconsecutive frames')
            for key, value, old in [('submittedStep', submitted, previous['eyeWorld']),
                                    ('nativeSceneStep', native, previous['nativeSceneCameraPosition']),
                                    ('playerStep', player, previous['nativePlayerPosition'])]:
                item[key] = float(np.linalg.norm(value - old))
        result.append(item)
        previous = row
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    sidecars = [json.loads(p.read_text()) for p in sorted(args.directory.glob('*.png.json'))]
    audit = json.loads((args.directory / 'pose-audit.json').read_text())
    frames = {row['frame']: row for row in audit['frames']}
    if not sidecars or any(s['frame'] not in frames for s in sidecars):
        raise ValueError('Incomplete capture/audit coverage')
    rows = compare([frames[s['frame']] for s in sidecars])
    report = dict(scope=__doc__, frames=rows, coveredFrames=len(rows),
                  maxRestoreError=max(r['restoreError'] for r in rows),
                  maxNativeSceneError=max(r['nativeSceneError'] for r in rows),
                  largestSteps=sorted(rows[1:], key=lambda r: r['submittedStep'], reverse=True)[:5])
    (args.directory / 'camera-motion.json').write_text(json.dumps(report, indent=2))
    print(json.dumps({k: v for k, v in report.items() if k != 'frames'}, indent=2))
