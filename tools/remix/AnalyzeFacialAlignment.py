"""Compare this Riverwood save's matching head/beard bind rotations.

Not a general skeleton validator: different bind rotations require a different
comparison. Host data only, not proof of GPU/native animation parity.
"""
import argparse
import json
from pathlib import Path


def rotation_delta(head, beard):
    if len(head['bones']) != len(beard['bones']) or not head['bones']:
        raise ValueError('Expected matching nonempty head/beard bone lists')

    def world_rotation(pose, bone):
        return [pose['world'][3] * sum(pose['rotation'][row * 3 + k] * bone[k * 4 + col]
                                      for k in range(3))
                for row in range(3) for col in range(3)]

    return max(abs(a - b) for h, b in zip(head['bones'], beard['bones'])
               for a, b in zip(world_rotation(head, h), world_rotation(beard, b)))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    captured = {json.loads(p.read_text())['frame'] for p in args.directory.glob('*.png.json')}
    audit = json.loads((args.directory / 'pose-audit.json').read_text())
    rows = []
    for row in audit['frames']:
        if row['frame'] not in captured:
            continue
        poses = {p['name']: p for p in row['poses']}
        rows.append({'frame': row['frame'], 'rotationDelta': rotation_delta(
            poses['MaleHeadNord'], poses['HumanBeard02'])})
    report = {'scope': __doc__, 'frames': rows,
              'mismatchedFrames': [r for r in rows if r['rotationDelta'] > 1e-5]}
    (args.directory / 'alignment-analysis.json').write_text(json.dumps(report, indent=2))
    print(json.dumps({'coveredFrames': len(rows), 'mismatchedFrames': report['mismatchedFrames']}))
