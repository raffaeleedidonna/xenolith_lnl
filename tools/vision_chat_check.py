#!/usr/bin/env python3
"""Check interactive image chat and a text-only chat on the target machine."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
for name in ('engine', 'model', 'mmproj', 'image', 'output'):
    p.add_argument('--' + name, type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
report = {'context': 65536, 'image_sha256': hashlib.sha256(a.image.read_bytes()).hexdigest(),
          'executable_sha256': hashlib.sha256(a.engine.read_bytes()).hexdigest(), 'cases': []}
for visual in (True, False):
    name = 'image' if visual else 'text'
    cmd = [str(a.engine.resolve()), 'chat', str(a.model.resolve()), '--ctx', '65536',
           '--top-k', '1', '-n', '64']
    if visual:
        cmd += ['--mmproj', str(a.mmproj.resolve()), '--image', str(a.image.resolve())]
        prompts = ['What animal is shown? Reply with just the animal name.',
                   'Describe its fur color briefly, based on the image.']
    else:
        prompts = ['Reply with only the word HELLO.', 'Reply with only the word WORLD.']
    started = time.monotonic()
    with (a.output / (name + '.stderr')).open('w') as err:
        result = subprocess.run(cmd, input='\n'.join(prompts + ['/exit']) + '\n',
                                text=True, stdout=subprocess.PIPE, stderr=err,
                                check=True, timeout=600)
    (a.output / (name + '.stdout')).write_text(result.stdout)
    answers = [re.split(r'\n\[\d+ tokens', s)[0].strip()
               for s in re.findall(r'assistant> (.*?)(?=you> |\Z)', result.stdout, re.S)]
    assert len(answers) == 2 and all(answers), answers
    if visual:
        assert re.search(r'\b(cat|kitten)\b', answers[0], re.I), answers
        assert result.stdout.count('Image ready.') == 1, result.stdout
        assert (a.output / (name + '.stderr')).read_text().count('encoder_ms=') == 1
    else:
        assert re.search(r'\bhello\b', answers[0], re.I), answers
        assert re.search(r'\bworld\b', answers[1], re.I), answers
    report['cases'].append({'kind': name, 'command': cmd, 'prompts': prompts,
                            'answers': answers, 'seconds': time.monotonic() - started,
                            'status': 'PASS'})
    print(name + ' chat: two turns PASS', flush=True)
report['status'] = 'PASS'
(a.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
