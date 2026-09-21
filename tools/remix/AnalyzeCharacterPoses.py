"""Join rendered-frame sidecars to host pose inputs; never claims GPU pose parity."""
import argparse
import json
from pathlib import Path


def join_frames(sidecars, audit):
    records = audit['frames']
    by_frame = {row['frame']: row for row in records}
    if len(by_frame) != len(records):
        raise ValueError('Duplicate pose frame')
    if not sidecars or any(row['frame'] not in by_frame for row in sidecars):
        raise ValueError('Pose records do not cover every captured frame')
    result = []
    previous = {}
    for sidecar in sidecars:
        row = by_frame[sidecar['frame']]
        if not row['ready'] or not row['poses']:
            raise ValueError('Scene not ready or no player facial poses')
        current = {str(pose['geometry']): pose for pose in row['poses']}
        if len(current) != len(row['poses']):
            raise ValueError('Duplicate geometry identity')
        changes = []
        for key in sorted(current.keys() | previous.keys()):
            pose, old = current.get(key), previous.get(key)
            if not pose or not old:
                if previous:
                    changes.append({'geometry': key, 'membership': 'added' if pose else 'removed'})
                continue
            fields = [field for field in ('submitted', 'mesh', 'retained', 'morphHash', 'world', 'rotation', 'bones')
                      if pose.get(field) != old.get(field)]
            if fields:
                changes.append({'geometry': key, 'name': pose['name'], 'fields': fields})
        result.append({'frame': row['frame'], 'changes': changes,
                       'unsubmitted': [p['name'] for p in current.values() if not p['submitted']]})
        previous = current
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    sidecars = [json.loads(path.read_text()) for path in sorted(args.directory.glob('*.png.json'))]
    audit = json.loads((args.directory / 'pose-audit.json').read_text())
    joined = join_frames(sidecars, audit)
    report = {'scope': audit['scope'], 'coveredFrames': len(joined), 'frames': joined}
    (args.directory / 'pose-analysis.json').write_text(json.dumps(report, indent=2))
    print(json.dumps({'coveredFrames': len(joined), 'changedFrames': sum(bool(r['changes']) for r in joined),
                      'unsubmittedFrames': sum(bool(r['unsubmitted']) for r in joined)}, indent=2))
