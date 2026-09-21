"""Validate consecutive rendered-frame captures; report differences, not a jitter verdict."""
import argparse
import hashlib
import json
from pathlib import Path


def validate_records(rows):
    if not rows or not 2 <= rows[0]['count'] <= 64:
        raise ValueError('Expected 2-64 frames')
    count = rows[0]['count']
    if len(rows) != count or [r['index'] for r in rows] != list(range(count)):
        raise ValueError('Missing, duplicate or unordered sequence indices')
    for row in rows:
        if row['saved'] is not True or row['count'] != count:
            raise ValueError('Save failed or count changed')
        for key in ('pid', 'width', 'height', 'format', 'source'):
            if row[key] != rows[0][key]:
                raise ValueError(f'{key} changed during sequence')
        if row['width'] <= 0 or row['height'] <= 0:
            raise ValueError('Invalid dimensions')
    for before, after in zip(rows, rows[1:]):
        if after['frame'] != before['frame'] + 1:
            raise ValueError('Rendered frames are not consecutive')
        if after['captureTimeUs'] <= before['captureTimeUs']:
            raise ValueError('Capture timestamps did not advance')


def analyze(directory, output, crop=None):
    import numpy as np
    from PIL import Image, ImageDraw
    paths = sorted(directory.glob('*.png.json'))
    rows = [json.loads(p.read_text()) for p in paths]
    validate_records(rows)
    if len(list(directory.glob('*.png'))) != len(paths):
        raise ValueError('Image/metadata count mismatch')
    sheet = Image.new('RGB', (8 * 240, ((len(rows) + 7) // 8) * 160))
    draw = ImageDraw.Draw(sheet)
    measurements, previous = [], None
    for index, (row, path) in enumerate(zip(rows, paths)):
        with Image.open(path.with_suffix('')) as source:
            source.load()
            if source.size != (row['width'], row['height']):
                raise ValueError('Image dimensions disagree with metadata')
            picture = source.convert('RGB')
        if crop:
            x, y, w, h = crop
            if min(x, y) < 0 or min(w, h) <= 0 or x + w > picture.width or y + h > picture.height:
                raise ValueError('Crop out of bounds')
            picture = picture.crop((x, y, x + w, y + h))
        pixels = np.asarray(picture)
        brightness = pixels @ np.array([0.2126, 0.7152, 0.0722])
        difference = None if previous is None else float(np.abs(pixels.astype(np.float32) - previous).mean())
        measurements.append({'index': index, 'frame': row['frame'], 'captureTimeUs': row['captureTimeUs'],
                             'pixelSha256': hashlib.sha256(pixels.tobytes()).hexdigest(),
                             'meanLuminance': float(brightness.mean()), 'previousRgbMad': difference})
        previous = pixels.astype(np.float32)
        picture.thumbnail((240, 140))
        origin = ((index % 8) * 240, (index // 8) * 160)
        sheet.paste(picture, origin)
        draw.text((origin[0] + 2, origin[1] + 140), f"{index}: frame {row['frame']}", fill='white')
    report = {'captureIntegrity': True, 'pid': rows[0]['pid'], 'count': len(rows),
              'crop': crop, 'scope': rows[0]['source'] + '; image differences include motion, lighting and sampling noise. '
              'No automatic animation/jitter/pacing acceptance.', 'frames': measurements}
    output.parent.mkdir(parents=True, exist_ok=True)
    output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    sheet.save(output.with_suffix('.png'))
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--crop', type=int, nargs=4, metavar=('X', 'Y', 'W', 'H'))
    args = parser.parse_args()
    report = analyze(args.directory, args.output, args.crop)
    print(json.dumps({k: v for k, v in report.items() if k != 'frames'}, indent=2))
