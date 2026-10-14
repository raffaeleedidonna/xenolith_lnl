#!/usr/bin/env python3
"""Check persisted visual sessions across full-SHA and metadata verification."""
import argparse,base64,json,os,selectors,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
for n in ('engine','model','mmproj','image','output'):p.add_argument('--'+n,type=Path,required=True)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
command=[str(a.engine.resolve()),'wire',str(a.model.resolve()),'--ctx','65536','--mmproj',str(a.mmproj.resolve()),'--state',str(out/'state'),'--cache',str(out/'cache')]
report={'checks':[]};sid=None
for index,mode in enumerate(('full','metadata','metadata','full')):
 env=os.environ.copy();env.pop('XENOLITH_WEIGHTS_VERIFY',None)
 if mode=='metadata':env['XENOLITH_WEIGHTS_VERIFY']='metadata'
 log=(out/f'{index}-{mode}.log').open('wb');proc=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=log,env=env);buffer=b''
 def send(d):proc.stdin.write(json.dumps(d).encode()+b'\n');proc.stdin.flush()
 def read():
  global buffer
  with selectors.DefaultSelector() as s:
   s.register(proc.stdout,selectors.EVENT_READ)
   while b'\n' not in buffer:
    assert s.select(300),'timeout';part=os.read(proc.stdout.fileno(),65536);assert part,'server closed';buffer+=part
  line,buffer=buffer.split(b'\n',1);return json.loads(line)
 try:
  if sid is None:
   send({'op':'create','system':'Answer concisely.'});r=read();assert r['ok'];sid=r['session']
   send({'op':'append','role':'user','text':'What color is the upper-left quadrant? Reply with one word.','image':{'data':base64.b64encode(a.image.read_bytes()).decode(),'budget':70}})
  else:
   send({'op':'open','session':sid});assert read()['ok']
   send({'op':'append','role':'user','text':'Repeat the color from the image. One word.'})
  assert read()['ok'];send({'op':'generate','top_k':1,'temperature':1,'max_tokens':12,'reasoning':False});text='';events=[]
  while True:
   e=read();events.append(e);assert e.get('ok') is not False and e.get('event')!='error',e
   if e.get('event')=='text_delta':text+=e['text']
   if e.get('event')=='done':break
  assert text.strip().lower()=='red',text
  if index:assert e.get('resume',{}).get('loaded')==(index==2),e
  send({'op':'checkpoint'});r=read();assert r['ok'] and (r['saved'] or r.get('reason')=='nothing_new'),r
  proc.stdin.close();assert proc.wait(timeout=60)==0;log.close()
  report['checks'].append({'mode':mode,'response':text,'resume':e.get('resume'),'events':events})
  print(mode+' verification: response and snapshot policy PASS',flush=True)
 except BaseException:
  proc.kill();proc.wait();log.close();raise
report['status']='PASS';(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
