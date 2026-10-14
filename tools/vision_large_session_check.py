#!/usr/bin/env python3
from pathlib import Path
import subprocess,json,base64,selectors,os,time,argparse
parser=argparse.ArgumentParser(description='Verify a synthetic 1120-row image in a durable 128k session without tracing.')
for name in ('engine','model','mmproj','fixtures','output'):parser.add_argument('--'+name,type=Path,required=True)
a=parser.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
command=[str(a.engine.resolve()),'wire',str(a.model.resolve()),'--ctx','131072','--mmproj',str(a.mmproj.resolve()),'--state',str(out/'state'),'--cache',str(out/'cache')]
log=(out/'server.log').open('wb');p=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=log)
buffer=b'';events=[]
def send(d):p.stdin.write(json.dumps(d).encode()+b'\n');p.stdin.flush()
def read():
 global buffer
 with selectors.DefaultSelector() as s:
  s.register(p.stdout,selectors.EVENT_READ)
  while b'\n' not in buffer:
   assert s.select(600),'timeout'
   data=os.read(p.stdout.fileno(),65536);assert data,'server closed';buffer+=data
 line,buffer=buffer.split(b'\n',1);return json.loads(line)
try:
 send({'op':'create','system':'Answer concisely.'});reply=read();assert reply['ok'];sid=reply['session']
 raw=(a.fixtures/'quadrants.png').read_bytes()
 send({'op':'append','role':'user','text':'What color is the upper-left quadrant? Reply with just the color name.','image':{'data':base64.b64encode(raw).decode(),'budget':1120}});reply=read();assert reply['ok'],reply
 send({'op':'generate','top_k':1,'temperature':1,'max_tokens':12,'reasoning':False});text=''
 while True:
  e=read();events.append(e);assert e.get('ok') is not False and e.get('event')!='error',e
  if e.get('event')=='text_delta':text+=e['text']
  if e.get('event')=='done':break
 assert text.strip().lower()=='red',text
 send({'op':'checkpoint'});ckpt=read();assert ckpt['ok'] and (ckpt['saved'] or ckpt.get('reason')=='nothing_new'),ckpt
 send({'op':'history'});h=read();assert h['entries'][1]['image']['rows']==1120,h
 fields={k:v.strip() for line in Path(f'/proc/{p.pid}/status').read_text().splitlines() if ':' in line for k,v in [line.split(':',1)]}
 p.stdin.close();assert p.wait(timeout=120)==0;log.close()
 buffer=b'';log=(out/'restart.log').open('wb');p=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=log)
 send({'op':'open','session':sid});opened=read();assert opened['ok'] and opened['resume']=='snapshot',opened
 send({'op':'append','role':'user','text':'Repeat the color from that image. One word.'});assert read()['ok']
 send({'op':'generate','top_k':1,'temperature':1,'max_tokens':12,'reasoning':False});continued='';resume_events=[]
 while True:
  e=read();resume_events.append(e);assert e.get('ok') is not False and e.get('event')!='error',e
  if e.get('event')=='text_delta':continued+=e['text']
  if e.get('event')=='done':break
 assert continued.strip().lower()=='red' and e.get('resume',{}).get('loaded'),(continued,e)
 p.stdin.close();assert p.wait(timeout=120)==0;log.close()
 report={'status':'PASS','context':131072,'budget':1120,'response':text,'checkpoint':ckpt,'image':h['entries'][1]['image'],'events':events,'memory':{k:fields[k] for k in ('VmHWM','VmRSS','VmSwap')},'restart':{'response':continued,'events':resume_events}}
 (out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print('runtime 1120 no tracing: response and checkpoint PASS',flush=True)
except BaseException:
 p.kill();p.wait();log.close();raise
