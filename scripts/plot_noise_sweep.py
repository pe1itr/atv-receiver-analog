#!/usr/bin/env python3
"""Optional matplotlib report for noise_sweep.py output."""
import argparse
import json
import os
from pathlib import Path
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('directory', type=Path)
args = parser.parse_args()
os.environ.setdefault('MPLCONFIGDIR', str(Path(tempfile.gettempdir()) / 'atv-matplotlib'))
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

rows = json.loads((args.directory / 'results.json').read_text())
widths = sorted({r['if_mhz'] for r in rows}, reverse=True)
fig, axes = plt.subplots(1, 2, figsize=(11, 4), constrained_layout=True)
for width in widths:
    for axis, mode, metric in [(axes[0], 'auto', 'field_fraction'), (axes[1], 'free', 'psnr_db')]:
        selected = sorted((r for r in rows if r['cnr_db'] != 'clean' and r['if_mhz'] == width and r['mode'] == mode), key=lambda r: r['cnr_db'])
        axis.plot([r['cnr_db'] for r in selected], [r[metric] for r in selected], 'o-', label=f'IF {width:g} MHz')
        axis.set_xlabel('C/N over full IQ bandwidth (dB)')
        axis.grid(alpha=.25)
        axis.legend()
axes[0].set(title='Automatic sync: produced / expected fields', ylabel='Field fraction', ylim=(-.05, 1.05))
axes[1].set(title='Free sync: image comparison', ylabel='PSNR versus clean free-sync field (dB)')
fig.suptitle('Synthetic FM-ATV + AWGN; one seed, no RF hardware')
fig.savefig(args.directory / 'metrics.png', dpi=160)
plt.close(fig)
# Compare wide-IF clean reference with the middle IF at several available C/Ns.
width = widths[len(widths)//2]
available = sorted({r['cnr_db'] for r in rows if r['cnr_db'] != 'clean'}, reverse=True)
targets = [min(available, key=lambda v: abs(v-target)) for target in (8, 4, 0, -4, -8)]
images = [('Clean reference (auto sync)', 'reference-auto.pgm')]
for cnr in dict.fromkeys(targets):
    row = next(r for r in rows if r['cnr_db'] == cnr and r['mode'] == 'free' and r['if_mhz'] == width)
    if row['image']:
        images.append((f'C/N {cnr:g} dB / IF {width:g} MHz / free', row['image']))
fig, axes = plt.subplots(2, 3, figsize=(12, 6.5), constrained_layout=True)
for ax in axes.flat:
    ax.axis('off')
for ax, (title, filename) in zip(axes.flat, images):
    ax.imshow(plt.imread(args.directory / filename), cmap='gray', vmin=0, vmax=255, extent=(0, 4, 0, 3), interpolation='nearest')
    ax.set_title(title, fontsize=10)
fig.savefig(args.directory / 'images.png', dpi=160)
plt.close(fig)
print(args.directory / 'metrics.png')
print(args.directory / 'images.png')
