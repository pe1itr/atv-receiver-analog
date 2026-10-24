#!/usr/bin/env python3
"""Generate replayable IQ and compare receiver IF/free-sync settings offline."""
import argparse
import csv
import json
import math
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='new directory (must not exist)')
    parser.add_argument('--seconds', type=float, default=0.4)
    parser.add_argument('--rate', type=float, default=20e6)
    parser.add_argument('--deviation', type=float, default=4e6)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--cnr', type=float, nargs='+', default=[20, 12, 8, 4, 0, -4, -8, -12])
    parser.add_argument('--if-mhz', type=float, nargs='+', default=[18, 8, 4])
    args = parser.parse_args()
    if not all(math.isfinite(v) for v in [args.seconds, args.rate, args.deviation, *args.cnr, *args.if_mhz]):
        parser.error('all numeric parameters must be finite')
    if not 0.04 <= args.seconds <= 3600 or not 8e6 <= args.rate <= 64e6 or not 0 < args.deviation < args.rate / 2:
        parser.error('duration >= 0.04 s, rate 8..64 MS/s, deviation < rate/2 required')
    if not 0 <= args.seed < 2**64 or any(not -60 <= v <= 120 for v in args.cnr):
        parser.error('invalid seed or C/N')
    if any(not 0.25 <= v <= args.rate/1e6 for v in args.if_mhz):
        parser.error('IF must be 0.25 MHz .. sample rate')
    if len(set(args.cnr)) != len(args.cnr) or len(set(args.if_mhz)) != len(args.if_mhz):
        parser.error('duplicate C/N or IF values')
    args.output.mkdir(parents=True, exist_ok=False)
    base = ['--rate', str(args.rate), '--deviation', str(args.deviation)]
    modes = [('auto', []), ('free', ['--free-sync'])]
    reference_bw = max(args.if_mhz) * 1e6
    references = {}
    rows = []
    columns = ['cnr_db', 'if_mhz', 'mode', 'fields', 'expected_fields', 'field_fraction', 'psnr_db', 'mae', 'image', 'iq']
    with (args.output / 'commands.log').open('w') as log, (args.output / 'results.csv').open('w', newline='') as csvfile:
        writer = csv.DictWriter(csvfile, fieldnames=columns)
        writer.writeheader()

        def run(cmd):
            log.write(' '.join(map(str, cmd)) + '\n')
            log.flush()
            result = subprocess.run(list(map(str, cmd)), stdout=subprocess.PIPE, stderr=log, text=True, check=True)
            log.write(result.stdout)
            log.flush()
            return result.stdout

        for index, cnr in enumerate([None, *args.cnr]):
            label = 'clean' if cnr is None else f'cnr-{index}-{cnr:g}dB'
            iq = args.output / f'{label}.cf32'
            command = [ROOT / 'build/atv-tx', '--output', iq, '--seconds', str(args.seconds), '--seed', str(args.seed), *base]
            if cnr is not None:
                command += ['--cnr-db', str(cnr)]
            run(command)
            if cnr is None:
                for mode, flags in modes:
                    reference = args.output / f'reference-{mode}.pgm'
                    stats = json.loads(run([ROOT / 'build/atv-eval', '--file', iq, *base, '--if-bandwidth', str(reference_bw), *flags, '--output', reference]))
                    if not stats['fields']:
                        raise RuntimeError('clean reference did not decode')
                    references[mode] = reference
            for if_index, if_mhz in enumerate(args.if_mhz):
                for mode, flags in modes:
                    image = args.output / f'{label}-if{if_index}-{if_mhz:g}-{mode}.pgm'
                    stats = json.loads(run([ROOT / 'build/atv-eval', '--file', iq, *base, '--if-bandwidth', str(if_mhz*1e6), *flags,
                                           '--reference', references[mode], '--output', image]))
                    row = dict(cnr_db='clean' if cnr is None else cnr, if_mhz=if_mhz, mode=mode,
                               **{k: stats[k] for k in ['fields', 'expected_fields', 'field_fraction', 'psnr_db', 'mae']},
                               image=image.name if image.exists() else '', iq=iq.name)
                    rows.append(row)
                    writer.writerow(row)
                    csvfile.flush()
                    quality = 'n/a' if stats['psnr_db'] is None else f"{stats['psnr_db']:.1f} dB"
                    print(f"{label:18s} IF {if_mhz:5g} MHz {mode:4s}: {stats['fields']:3}/{stats['expected_fields']} fields; PSNR {quality}", flush=True)
    (args.output / 'results.json').write_text(json.dumps(rows, indent=2) + '\n')
    print(f"Results: {args.output / 'results.csv'}\nAll IQ files retained for --file ... --loop playback.")


if __name__ == '__main__':
    main()
