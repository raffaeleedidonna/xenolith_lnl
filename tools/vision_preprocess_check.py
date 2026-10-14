#!/usr/bin/env python3
"""Compare native PNG/EXIF/alpha pixels with Pillow and pinned HF resize math."""
import argparse
import ast
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
from PIL import Image, ImageOps

root = Path(__file__).resolve().parent.parent
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--prepare', type=Path, default=root / 'tools/vision_prepare')
p.add_argument('--output', type=Path, default=root / '.vision-cache/preprocess-check')
a = p.parse_args(); a.output.mkdir(parents=True, exist_ok=True)
source = root / '.vision-cache/reference/image_processing_gemma4.py'
tree = ast.parse(source.read_text())
fn = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'get_aspect_ratio_preserving_size')
scope = {'math': math}; exec(compile(ast.Module(body=[fn], type_ignores=[]), str(source), 'exec'), scope)
resize = scope[fn.name]
im = Image.new('RGB', (43, 29))
im.putdata([((x*17+y*5)%256, (x*3+y*23)%256, (x*13+y*7)%256) for y in range(29) for x in range(43)])
cases = []
for orientation in range(1, 9):
    exif = Image.Exif(); exif[274] = orientation
    path = a.output / f'exif-{orientation}.png'; im.save(path, exif=exif)
    cases.append((path, 70, 0))
exif = Image.Exif(); exif[274] = 6
path = a.output / 'oriented.jpg'; im.save(path, exif=exif, quality=95, subsampling=0)
cases.append((path, 70, 3))
alpha = Image.new('RGBA', (43, 29))
alpha.putdata([(x*5%256, y*7%256, (x+y)*9%256, (x*7+y*11)%256) for y in range(29) for x in range(43)])
path = a.output / 'alpha.png'; alpha.save(path); cases.append((path, 70, 0))
for name in ['quadrants', 'wide', 'portrait']:
    for budget in [70, 140, 280, 560, 1120]:
        cases.append((root / 'vision/fixtures' / (name+'.png'), budget, 0))
report = []
for path, budget, tolerance in cases:
    reference = ImageOps.exif_transpose(Image.open(path)).convert('RGBA')
    background = Image.new('RGBA', reference.size, 'white')
    reference = Image.alpha_composite(background, reference).convert('RGB')
    w, h = reference.size; th, tw = resize(h, w, 16, budget*9, 3)
    expected = reference.resize((tw, th), Image.Resampling.BICUBIC).tobytes()
    output = a.output / f'{path.stem}-{budget}.rgb'
    subprocess.run([str(a.prepare), str(path), str(budget), str(output)], check=True, stdout=subprocess.DEVNULL)
    actual = output.read_bytes(); assert struct.unpack('<II', actual[:8]) == (tw, th)
    assert len(actual[8:]) == len(expected)
    maximum = max(abs(x-y) for x, y in zip(actual[8:], expected))
    report.append({'image': path.name, 'budget': budget, 'max_pixel_difference': maximum,
                   'tolerance': tolerance, 'pass': maximum <= tolerance,
                   'input_sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
    print(path.name, budget, maximum, 'PASS' if maximum <= tolerance else 'FAIL')
# Truncation and malformed orientation fail without returning an image.
bad = a.output / 'truncated.png'; bad.write_bytes((root/'vision/fixtures/quadrants.png').read_bytes()[:64])
assert subprocess.run([str(a.prepare), str(bad), '70', str(a.output/'bad.rgb')], capture_output=True).returncode != 0
(a.output / 'report.json').write_text(json.dumps(report, indent=2)+'\n')
raise SystemExit(0 if all(row['pass'] for row in report) else 1)
