"""Validate bounded GPU readbacks, not animation/presentation smoothness."""
import argparse
import json
import math
import re
from pathlib import Path


def validate(text, face=False, vertices=898):
    if not 1 <= vertices <= 4096 or (not face and vertices != 898):
        raise ValueError('Vertex selection requires face mode and a count from 1 to 4096')
    rows = []
    for line in text.splitlines():
        if '[CSRemix.skinProbe]' not in line:
            continue
        fields = dict(re.findall(r'(\w+)=([^\s]+)', line))
        ints = ('sample', 'frame', 'key', 'boneHash', 'vertices', 'nonFinite')
        floats = ('maxPositionError', 'maxNormalError', 'maxMovement')
        try:
            row = {k: int(fields[k]) for k in ints}
            row.update({k: float(fields[k]) for k in floats})
            if 'geometryKey' in fields:
                row['geometryKey'] = int(fields['geometryKey'])
        except (KeyError, ValueError) as error:
            raise ValueError('Malformed skin probe record') from error
        if fields.get('faceProbe', '0') != ('1' if face else '0'):
            raise ValueError('Wrong probe mode')
        if not all(math.isfinite(row[k]) and row[k] >= 0 for k in floats):
            raise ValueError('Invalid probe metric')
        rows.append(row)
    count = 64 if face else 12
    if len(rows) != count or [r['sample'] for r in rows] != list(range(count)):
        raise ValueError(f'Expected exactly {count} ordered samples from one process log')
    if any(b['frame'] <= a['frame'] for a, b in zip(rows, rows[1:])):
        raise ValueError('Frames did not advance')
    if len({r['key'] for r in rows}) != 1 or not rows[0]['key']:
        raise ValueError('Probe changed geometry identity')
    if any(r['vertices'] != (vertices if face else 32) or r['nonFinite'] or
           r['maxPositionError'] > 0.01 or r['maxNormalError'] > 0.0001 for r in rows):
        raise ValueError('GPU skinning differs from the submitted pose')
    if len({r['boneHash'] for r in rows}) < 2 or max(r['maxMovement'] for r in rows[1:]) <= 0.00001:
        raise ValueError('No animated pose observed')
    return {'passed': True, 'samples': rows,
            'scope': (f'{vertices} vertices of one vertex-count/material class (not unique actor or mesh). ' if face else
                      '32 vertices of one 24-bone MSN geometry. ') + 'After skinning and before tracing. '
                     'GPU output versus CPU evaluation of captured inputs; not native-pose timing, '
                     'all-frame continuity, BLAS contents, first-person correctness or presentation pacing.'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--face', action='store_true')
    parser.add_argument('--vertices', type=int, default=898)
    args = parser.parse_args()
    result = validate(args.log.read_text(encoding='utf-8', errors='replace'), face=args.face, vertices=args.vertices)
    encoded = json.dumps(result, indent=2)
    if args.report:
        args.report.write_text(encoded + '\n', encoding='utf-8')
    print(encoded)
