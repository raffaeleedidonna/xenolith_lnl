#!/usr/bin/env python3
"""Model-backed protocol v2 checks, in an isolated state/cache directory."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import selectors
import subprocess
import time

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--engine',type=Path,required=True)
p.add_argument('--model',type=Path,required=True)
p.add_argument('--mmproj',type=Path,required=True)
p.add_argument('--fixtures',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--ctx',type=int,choices=(65536,131072),default=65536)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
state=a.output/'state';cache=a.output/'cache'
report={'context':a.ctx,'checks':[],'generations':[]}
proc=None;log=None;pending=b''

def record(name):
    report['checks'].append(name)
    (a.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(name,'PASS',flush=True)

def start(vision=True):
    global proc,log,pending
    pending=b''
    log=(a.output/f"server-{len(report['checks'])}.log").open('wb')
    cmd=[str(a.engine.resolve()),'wire',str(a.model.resolve()),'--ctx',str(a.ctx),
         '--state',str(state.resolve()),'--cache',str(cache.resolve())]
    if vision:cmd+=['--mmproj',str(a.mmproj.resolve())]
    proc=subprocess.Popen(cmd,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=log)

def stop():
    global proc
    if proc is None:return
    status=Path(f'/proc/{proc.pid}/status')
    if status.exists():
        fields={line.split(':',1)[0]:line.split(':',1)[1].strip()
                for line in status.read_text().splitlines() if ':' in line}
        report.setdefault('server_memory',[]).append({key:fields[key]
            for key in ('VmPeak','VmHWM','VmRSS','VmSwap') if key in fields})
    proc.stdin.close()
    try:code=proc.wait(timeout=120)
    except subprocess.TimeoutExpired:
        proc.terminate();code=proc.wait(timeout=30)
    log.close();proc=None
    assert code==0,code

def send(req):
    proc.stdin.write(json.dumps(req,separators=(',',':')).encode()+b'\n');proc.stdin.flush()

def read():
    global pending
    deadline=time.monotonic()+600
    with selectors.DefaultSelector() as sel:
        sel.register(proc.stdout,selectors.EVENT_READ)
        while b'\n' not in pending:
            assert sel.select(max(0,deadline-time.monotonic())), 'response timeout'
            data=os.read(proc.stdout.fileno(),1048576)
            assert data,'engine closed output'
            pending+=data
    line,pending=pending.split(b'\n',1)
    return json.loads(line)

def call(req,ok=True):
    send(req);reply=read();assert reply.get('ok') is ok,(req,reply)
    return reply

def generate(req,expected=None):
    send(dict(temperature=1,top_k=1,max_tokens=24,reasoning=False,**req))
    text='';events=[]
    while True:
        e=read();events.append(e)
        assert e.get('ok') is not False,e
        assert e.get('event')!='error',e
        if e.get('event')=='text_delta':text+=e['text']
        if e.get('event')=='done':break
    report['generations'].append({'request':req['op'],'text':text,'done':e})
    if expected:assert expected.lower() in text.lower(),(expected,text)
    return e,text

raw=(a.fixtures/'quadrants.png').read_bytes()
other=(a.fixtures/'quadrants-rotated.png').read_bytes()
image=lambda data,budget=70:{'data':base64.b64encode(data).decode(),'budget':budget}
prompt='What color is the upper-left quadrant? Reply with just the color name.'
try:
    start()
    d=call({'op':'describe'});assert d['protocol']==2 and d['vision']['max_bytes']==48*1024*1024
    record('vision capabilities')
    sid=call({'op':'create','system':'Answer concisely.'})['session']
    for bad in ({'data':'YQ=='},{'data':'!!!!!!!!!!!!'},{'data':'AAAAAAAAAAA=' ,'budget':71},
                {'data':'AAAAAAAAAAA=','id':'0'*64},{'id':'0'*64}, {'data':'AAAAAAAAAA=='}):
        call({'op':'append','role':'user','text':prompt,'image':bad},False)
    assert len(call({'op':'history'})['entries'])==1
    record('invalid images leave history unchanged')
    marker=call({'op':'append','role':'user','text':prompt,'image':image(raw)})['marker']
    entries=call({'op':'history'})['entries'];meta=entries[-1]['image']
    assert meta['rows']==70 and meta['id']==hashlib.sha256(raw).hexdigest()
    media=call({'op':'media','image_id':meta['id']})
    assert base64.b64decode(media['data'])==raw
    call({'op':'media','image_id':meta['id'],'offset':len(raw)+1},False)
    call({'op':'media','image_id':meta['id'],'limit':0},False)
    record('image history and source retrieval')
    done,_=generate({'op':'generate'},'red')
    ckpt=call({'op':'checkpoint'});assert ckpt['saved'] is True,ckpt
    record('durable response and checkpoint')
    call({'op':'append','role':'user','text':prompt,'image':image(other)})
    generate({'op':'generate'},'yellow')
    record('second image changes answer with existing KV')
    call({'op':'rewind','marker':marker})
    generate({'op':'generate'},'red')
    ckpt=call({'op':'checkpoint'});assert ckpt['saved'] is True,ckpt
    record('rewind across second image')
    stop();start()
    opened=call({'op':'open','session':sid});assert opened['resume']=='snapshot',opened
    assert call({'op':'history'})['entries'][1]['image']==meta
    call({'op':'append','role':'user','text':'Repeat the color from that image, one word.'})
    done,_=generate({'op':'generate'},'red')
    assert done.get('resume',{}).get('loaded') is True,done
    record('restart resumes visual snapshot')
    call({'op':'rewind','marker':marker});stop()
    for f in (cache/'vision-v1').glob('*.xve'):f.unlink()
    start();call({'op':'open','session':sid})
    generate({'op':'generate'},'red')
    assert list((cache/'vision-v1').glob('*.xve'))
    record('evicted embeddings rebuilt from persisted media')
    before=call({'op':'history'})
    generate({'op':'ephemeral','messages':[{'role':'user','text':prompt,'image':image(other)}]},'yellow')
    assert call({'op':'history'})==before
    record('ephemeral image preserves durable history')
    call({'op':'rebuild','system':'Answer concisely.','messages':[{'role':'user','text':prompt,'image':{'id':meta['id'],'budget':140}}]})
    assert call({'op':'history'})['entries'][-1]['image']['rows']==126
    generate({'op':'generate'},'red')
    record('rebuild by media ID with variable visual length')
    tools=[{'name':'camera','description':'Read a camera image.','parameters':{'type':'object','properties':{}}}]
    call({'op':'rebuild','system':'Answer concisely.','tools':tools,'messages':[
        {'role':'user','text':'Read the camera and identify the upper-left color.'},
        {'role':'assistant','text':'','calls':[{'name':'camera','arguments':{}}]},
        {'role':'tool','tool':'camera','text':prompt,'image':image(other)}]})
    generate({'op':'generate'},'yellow')
    record('tool-result image')
    before=call({'op':'history'})
    call({'op':'rebuild','messages':[
        {'role':'user','text':prompt,'image':image(raw)},
        {'role':'user','image':image(b'broken-image')}]},False)
    assert call({'op':'history'})==before
    record('failed visual rebuild rolls back the transcript')
    send({'op':'generate','temperature':1,'top_k':1,'max_tokens':128,
          'reasoning':{'effort':'high','budget_tokens':128,'history':'discard'}})
    assert read()['event']=='start'
    send({'op':'cancel'})
    cancelled=False;terminal=None
    while terminal is None:
        e=read()
        if e.get('ok') is True:cancelled=True
        assert e.get('event')!='error',e
        if e.get('event')=='done':terminal=e
    assert cancelled and terminal['stop']=='aborted',terminal
    assert any(e.get('image') for e in call({'op':'history'})['entries'])
    generate({'op':'generate'})
    record('reasoning cancellation and projection replay retain the image')
    code=(a.fixtures/'code.png').read_bytes()
    generate({'op':'ephemeral','messages':[{'role':'user',
        'text':'Read the four-digit code in the image. Reply with just the digits.',
        'image':image(code,140)}]},'7318')
    record('synthetic OCR response')
    stop();start(False)
    assert call({'op':'describe'})['protocol']==1
    call({'op':'open','session':sid},False)
    call({'op':'ephemeral','messages':[{'role':'user','image':image(raw)}]},False)
    record('text service rejects visual sessions and requests')
    stop();report['status']='PASS'
    (a.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
except BaseException as e:
    report['status']='FAIL';report['error']=repr(e)
    (a.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    if proc:proc.kill();proc.wait();log.close()
    raise
