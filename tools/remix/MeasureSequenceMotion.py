"""Fixed-patch phase correlation for frame sequences; diagnostic, not camera parity."""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image
from AnalyzeFrameSequence import validate_records


def shift(a, b):
    if a.shape != b.shape or a.ndim != 2:
        raise ValueError('Expected equal 2D patches')
    window = np.outer(np.hanning(a.shape[0]), np.hanning(a.shape[1]))
    fa = np.fft.rfft2((a - a.mean()) * window)
    fb = np.fft.rfft2((b - b.mean()) * window)
    cross = fb * np.conj(fa)
    correlation = np.fft.irfft2(cross / np.maximum(np.abs(cross), 1e-12), s=a.shape)
    y, x = np.unravel_index(np.argmax(correlation), correlation.shape)
    return [int(x if x < a.shape[1] // 2 else x - a.shape[1]),
            int(y if y < a.shape[0] // 2 else y - a.shape[0]), float(correlation[y, x])]


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--patch', nargs=4, type=int, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    paths = sorted(args.directory.glob('*.png.json'))
    metadata = [json.loads(p.read_text()) for p in paths]
    validate_records(metadata)
    x, y, w, h = args.patch
    if min(x, y) < 0 or min(w, h) < 4 or x+w > metadata[0]['width'] or y+h > metadata[0]['height']:
        raise ValueError('Invalid patch')
    previous = None
    rows = []
    for record, path in zip(metadata, paths):
        with Image.open(path.with_suffix('')) as source:
            pixels = np.asarray(source.convert('L').crop((x, y, x+w, y+h)), dtype=float)
        if previous is not None:
            dx, dy, peak = shift(previous, pixels)
            rows.append(dict(frame=record['frame'], dx=dx, dy=dy, peak=peak))
        previous = pixels
    report = dict(patch=args.patch, rows=rows, scope='Integer phase-correlation displacement in a manually selected image patch. '
                  'Parallax, rotation, deformation, occlusion and noise may invalidate a translation estimate; no automatic pass.')
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))
