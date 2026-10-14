#!/usr/bin/env python3
"""Model-backed migration, predecessor restore and disabled-cache checks.

Run with a pre-integration engine and the current engine. State/cache directories
are isolated under --output; no existing user session is modified.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import selectors
import struct
import subprocess
import time

p=argparse.ArgumentParser(description=__doc__)
for name in ('engine','previous-engine','model','mmproj','image','output'):
    p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--ctx',type=int,choices=(65536,131072),default=65536)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
cache=out/'cache';state=out/'state';report={'context':a.ctx,'checks':[],'generations':[]}
proc=None;log=None;buffer=b'';starts=0

def record(name):
    report['checks'].append(name)
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(name,'PASS',flush=True)

def start(engine,budget=None):
    global proc,log,buffer,starts
    buffer=b'';log=(out/f'server-{starts}.log').open('wb');starts+=1
    command=[str(engine.resolve()),'wire',str(a.model.resolve()),'--ctx',str(a.ctx),
             '--mmproj',str(a.mmproj.resolve()),'--state',str(state),'--cache',str(cache)]
    if budget is not None:command+=['--cache-budget',budget]
    proc=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=log)

def stop():
    global proc
    proc.stdin.close();assert proc.wait(timeout=120)==0;log.close();proc=None

def send(request):
    proc.stdin.write(json.dumps(request).encode()+b'\n');proc.stdin.flush()

def read():
    global buffer
    with selectors.DefaultSelector() as selector:
        selector.register(proc.stdout,selectors.EVENT_READ)
        while b'\n' not in buffer:
            assert selector.select(600),'timeout'
            part=os.read(proc.stdout.fileno(),65536);assert part,'engine closed';buffer+=part
    line,buffer=buffer.split(b'\n',1);return json.loads(line)

def call(request):
    send(request);reply=read();assert reply.get('ok'),(request,reply);return reply

def generate():
    send({'op':'generate','temperature':1,'top_k':1,'max_tokens':12,'reasoning':False})
    text=''
    while True:
        event=read();assert event.get('ok') is not False and event.get('event')!='error',event
        if event.get('event')=='text_delta':text+=event['text']
        if event.get('event')=='done':break
    assert text.strip().lower()=='red',text
    report['generations'].append({'text':text,'done':event})
    return event

def checkpoint():
    started=time.monotonic()
    result=call({'op':'checkpoint'})
    assert result.get('saved') or result.get('reason')=='nothing_new',result
    report.setdefault('checkpoints',[]).append({'reply':result,'elapsed_seconds':time.monotonic()-started})

def snapshots():
    result=[]
    for file in cache.glob('*.xkv'):
        with file.open('rb') as f:
            first=f.read(96)
            offset=96+40*struct.unpack_from('<Q',first,48)[0] if first[:8]==b'XEKVSEG\n' else 0
            f.seek(offset);header=f.read(384)
        result.append((struct.unpack_from('<Q',header,40)[0],file,offset,header))
    return sorted(result)

def inventory():
    return {str(f.relative_to(cache)):hashlib.sha256(f.read_bytes()).hexdigest()
            for f in cache.rglob('*') if f.is_file()}

try:
    start(a.previous_engine)
    sid=call({'op':'create','system':'Answer concisely.'})['session']
    call({'op':'append','role':'user','text':'What color is the upper-left quadrant? One word.',
          'image':{'data':base64.b64encode(a.image.read_bytes()).decode(),'budget':70}})
    generate();checkpoint();history=call({'op':'history'});media=history['entries'][1]['image']
    old=snapshots();assert old and all(struct.unpack_from('<H',h,8)[0]==2 for _,_,_,h in old)
    stop();record('pre-integration visual-v2 session created')

    start(a.engine);call({'op':'open','session':sid})
    assert call({'op':'history'})==history
    fetched=call({'op':'media','image_id':media['id']})
    assert base64.b64decode(fetched['data'])==a.image.read_bytes()
    call({'op':'append','role':'user','text':'Repeat the color from the image. One word.'})
    done=generate();assert not done.get('resume',{}).get('loaded'),done
    checkpoint();assert snapshots() and all(struct.unpack_from('<H',h,8)[0]==3 for _,_,_,h in snapshots())
    assert list(cache.glob('*.xkc'))
    report['storage']={'standalone_snapshot_bytes':sum(struct.unpack_from('<Q',h,32)[0] for _,_,_,h in snapshots()),
                       'checkpoint_disk_bytes':sum(f.stat().st_size for f in cache.iterdir() if f.is_file())}
    stop();record('legacy visual-v2 replays retained media and writes shared visual-v3')

    start(a.engine);call({'op':'open','session':sid})
    call({'op':'append','role':'user','text':'Repeat that color. One word.'})
    done=generate();assert done.get('resume',{}).get('loaded'),done
    checkpoint();stop();record('shared visual-v3 restores after process restart')

    points=snapshots();assert len(points)>=2,points
    newest,path,offset,header=points[-1]
    with path.open('r+b') as f:
        f.seek(offset+376);byte=f.read(1);f.seek(offset+376);f.write(bytes([byte[0]^1]));f.flush();os.fsync(f.fileno())
    start(a.engine);call({'op':'open','session':sid})
    call({'op':'append','role':'user','text':'Repeat that color again. One word.'})
    done=generate();assert done.get('resume',{}).get('loaded') and done['resume']['tokens']<newest,done
    checkpoint();stop();record('corrupt newest checkpoint falls back to compatible visual predecessor')

    before=inventory();start(a.engine,'0');call({'op':'open','session':sid})
    call({'op':'append','role':'user','text':'Repeat the color. One word.'})
    done=generate();assert not done.get('resume',{}).get('loaded'),done
    ckpt=call({'op':'checkpoint'});assert not ckpt['saved'] and ckpt['reason']=='no_kvstore',ckpt
    stop();assert inventory()==before
    record('cache budget zero replays media without disk cache reads/writes')

    start(a.engine,'1MiB');call({'op':'open','session':sid})
    assert not list(cache.glob('*.xkv')) and not list(cache.glob('*.xkc'))
    call({'op':'append','role':'user','text':'Repeat the color. One word.'})
    done=generate();assert not done.get('resume',{}).get('loaded'),done
    ckpt=call({'op':'checkpoint'});assert not ckpt['saved'] and ckpt['reason']=='budget',ckpt
    assert call({'op':'history'})['entries'][1]['image']==media
    stop();record('budget eviction removes checkpoints/chunks and preserves source media')
    report['status']='PASS';(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
except BaseException as exc:
    report['status']='FAIL';report['error']=repr(exc)
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    if proc:proc.kill();proc.wait();log.close()
    raise
