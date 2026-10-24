#!/usr/bin/env python3
"""Reproducible FM0/FM1 comparison using on-disk IQ and the receiver DSP.
Synthetic PSNR uses a shared clean FM0 image and a fixed free-running raster.
It includes filtering/detector delay and startup; it is not a subjective score.
Real captures have no clean reference: only sync counts and CPU are reported.
"""
import argparse
import json
import math
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output-dir', type=Path, required=True)
parser.add_argument('--capture', type=Path)
parser.add_argument('--rate', type=float, default=20e6)
parser.add_argument('--if-bandwidth', type=float, default=8e6)
args = parser.parse_args()
args.output_dir.mkdir(parents=True, exist_ok=True)

def run(tool, *options):
    p = subprocess.run([str(ROOT/'build'/tool), *map(str, options)],
                       check=True, capture_output=True, text=True)
    return json.loads(p.stdout) if tool == 'atv-eval' else None

rows = []
common = ['--rate', args.rate, '--if-bandwidth', args.if_bandwidth,
          '--alignment-bandwidth', 3200000]
if args.capture:
    for mode in (0, 1):
        row = run('atv-eval', '--file', args.capture, *common, '--fm-mode', mode)
        rows.append(dict(source=str(args.capture), **row))
else:
    reference = args.output_dir/'reference.pgm'
    for cnr in (None, 12, 6, 0, -6):
        iq = args.output_dir/f'cnr-{cnr}.cf32'
        if not iq.exists():
            run('atv-tx', '--output', iq, '--seconds', '.165', '--rate', args.rate,
                '--seed', 42, *([] if cnr is None else ['--cnr-db', cnr]))
        if cnr is None and not reference.exists():
            run('atv-eval', '--file', iq, *common, '--free-sync', '--output', reference)
        for mode in (0, 1):
            image = args.output_dir/f'cnr-{cnr}-fm{mode}.pgm'
            row = run('atv-eval', '--file', iq, *common, '--fm-mode', mode,
                      '--free-sync', '--reference', reference,
                      *([] if image.exists() else ['--output', image]))
            # Compare the same final field, excluding acquisition/startup fields.
            ref_pixels = reference.read_bytes().split(b'\n', 3)[3]
            pixels = image.read_bytes().split(b'\n', 3)[3]
            mse = sum((a-b)**2 for a,b in zip(pixels, ref_pixels))/len(pixels)
            row['last_field_mse'] = mse
            row['last_field_psnr_db'] = 10*math.log10(255**2/mse) if mse else None
            row['last_field_perfect_match'] = mse == 0
            sync = run('atv-eval', '--file', iq, *common, '--fm-mode', mode)
            rows.append(dict(cnr_db=cnr, sync_fields=sync['fields'],
                             sync_hsync_updates=sync['hsync_updates'], **row))
(args.output_dir/'results.json').write_text(json.dumps(rows, indent=2)+'\n')
print(json.dumps(rows, indent=2))
