#!/usr/bin/env python3
"""Run the existing Xenolith text benchmark matrix for vision regression checks.

Run on the target Linux machine, with no other inference workloads active.
Baseline and candidate paths must refer to separate, already built executables.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--model', required=True, type=Path)
p.add_argument('--candidate', default=Path('./xenolith'), type=Path)
p.add_argument('--baseline', type=Path)
p.add_argument('--output', required=True, type=Path)
p.add_argument('--repetitions', type=int, default=3)
p.add_argument('--paired', action='store_true', help='ABBA baseline/candidate runs of pp2048 and tg128, at text prefill 512')
a = p.parse_args()
if a.repetitions < 1:
    p.error('repetitions must be positive')
if a.paired and not a.baseline:
    p.error('--paired requires --baseline')
a.output.mkdir(parents=True, exist_ok=True)
executables = [('candidate', a.candidate.resolve(), [512, 1024])]
if a.baseline:
    executables.insert(0, ('baseline', a.baseline.resolve(), [512]))
record = {'model': str(a.model.resolve()), 'executables': {}, 'repetitions': a.repetitions,
          'contexts': [65536, 131072], 'warmup': True, 'delay_seconds': 1}
record['mode']='paired ABBA pp2048/tg128 text512' if a.paired else 'full matrix'
for variant, executable, batches in executables:
    record['executables'][variant] = {'path': str(executable), 'sha256': hashlib.sha256(executable.read_bytes()).hexdigest()}
(a.output / 'provenance.json').write_text(json.dumps(record, indent=2)+'\n')
if a.paired:
    for context in record['contexts']:
        for index,variant in enumerate(('baseline','candidate','candidate','baseline'),1):
            executable=a.baseline.resolve() if variant=='baseline' else a.candidate.resolve()
            name=f'paired-ctx{context}-{index}-{variant}'
            cmd=[str(executable),'bench',str(a.model.resolve()),'--ctx',str(context),
                 '-p','2048','-n','128','-r',str(a.repetitions),'--delay','1','-o','jsonl']
            with (a.output/(name+'.jsonl')).open('w') as out,(a.output/(name+'.log')).open('w') as err:
                subprocess.run(cmd,stdout=out,stderr=err,check=True)
            print(name,'complete',flush=True)
    raise SystemExit(0)
for variant, executable, batches in executables:
    for context in [65536, 131072]:
        for batch in batches:
            for prompt in [512, 2048, 8192]:
                name = f'{variant}-ctx{context}-b{batch}-pp{prompt}'
                cmd = [str(executable), 'bench', str(a.model.resolve()), '--ctx', str(context),
                       '-p', str(prompt), '-n', '0', '-r', str(a.repetitions), '--delay', '1', '-o', 'jsonl']
                if variant == 'candidate':
                    cmd += ['--prefill', str(batch)]
                with (a.output / (name+'.jsonl')).open('w') as out, (a.output / (name+'.log')).open('w') as err:
                    subprocess.run(cmd, stdout=out, stderr=err, check=True)
                print(name, 'complete', flush=True)
        name = f'{variant}-ctx{context}-tg128'
        cmd = [str(executable), 'bench', str(a.model.resolve()), '--ctx', str(context),
               '-p', '0', '-n', '128', '-r', str(a.repetitions), '--delay', '1', '-o', 'jsonl']
        with (a.output / (name+'.jsonl')).open('w') as out, (a.output / (name+'.log')).open('w') as err:
            subprocess.run(cmd, stdout=out, stderr=err, check=True)
        print(name, 'complete', flush=True)
