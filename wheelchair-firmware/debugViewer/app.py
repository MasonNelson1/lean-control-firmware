import asyncio, csv, json, os, pty, time, argparse
from collections import deque
from pathlib import Path
from fastapi import FastAPI, WebSocket
from fastapi.responses import HTMLResponse, FileResponse

ROOT = Path(__file__).parent
FIELDS = 'time_ms F B L R total userWeight speed turn leftTargetRPM rightTargetRPM leftSmoothedRPM rightSmoothedRPM stopped'.split()
def parse(line):
    parts = [p.strip() for p in next(csv.reader([line]))]
    if not parts: return 'text', line
    if parts[0] == 'time_ms': return 'header', None
    if len(parts) in (12, 14, 15):
        try:
            mode = parts[-1].upper() if len(parts) == 15 else 'UNKNOWN'
            numeric = parts[:-1] if len(parts) == 15 else parts
            values = [float(p) for p in numeric]
            import math
            if not all(math.isfinite(v) for v in values): raise ValueError()
            names = FIELDS if len(values) == 14 else FIELDS[:11]+['stopped']
            row = dict(zip(names, values))
            row['mode'] = mode if mode in ('LEAN', 'REMOTE') else 'UNKNOWN'
            return 'data', row
        except ValueError: pass
    return 'text', line

app=FastAPI()
history=deque(maxlen=13000); console=deque(maxlen=400); clients=set()
connected=True; proc=None; args=None
async def emit(event):
    for ws in list(clients):
        try: await asyncio.wait_for(ws.send_json(event), .3)
        except Exception: clients.discard(ws)
async def line_received(line):
    kind,value=parse(line)
    if kind=='data':
        if history and value['time_ms'] < history[-1]['time_ms']: history.clear()
        history.append(value)
        last_change[0]=time.monotonic()
        while history and value['time_ms']-history[0]['time_ms']>60000: history.popleft()
    elif kind=='text' and line.strip():
        console.append(line); await emit({'console':line})
async def note(s):
    await line_received(s)
async def acquire():
    global proc
    if args.replay:
        while True:
            previous=None
            for line in Path(args.replay).read_text().splitlines():
                while not connected: await asyncio.sleep(.1)
                kind,row=parse(line)
                if kind=='data':
                    if previous is not None: await asyncio.sleep(max(0,min(1,(row['time_ms']-previous)/1000)))
                    previous=row['time_ms']
                await line_received(line)
            await asyncio.sleep(2)
    while True:
        if not connected: await asyncio.sleep(.3); continue
        master,slave=pty.openpty()
        try:
            cmd=['pio','device','monitor','-d',str(ROOT.parent),'-e','teensy41','-b',str(args.baud)]
            if args.port: cmd+=['-p',args.port]
            proc=await asyncio.create_subprocess_exec(*cmd,stdin=slave,stdout=slave,stderr=slave,cwd=ROOT,start_new_session=True)
            os.close(slave); slave=None
            os.set_blocking(master,False); buffer=''
            while connected and proc.returncode is None:
                try:
                    data=os.read(master,65536)
                    buffer+=data.decode(errors='replace')
                    while '\n' in buffer:
                        line,buffer=buffer.split('\n',1); await line_received(line.rstrip('\r'))
                except BlockingIOError: pass
                except OSError: break
                await asyncio.sleep(.01)
        except Exception as exc: await note(str(exc))
        finally:
            if slave is not None: os.close(slave)
            os.close(master)
            if proc and proc.returncode is None: proc.terminate(); await proc.wait()
            proc=None
        if connected:
            await note('Lost connection — reconnecting'); await asyncio.sleep(2)
async def broadcast():
    while True:
        await emit({'history':list(history),'stale':not history or time.monotonic()-last_change[0]>2})
        await asyncio.sleep(.05)
last_change=[0]
@app.on_event('startup')
async def start():
    app.state.tasks=[asyncio.create_task(acquire()),asyncio.create_task(broadcast())]
@app.on_event('shutdown')
async def stop():
    for t in app.state.tasks: t.cancel()
    if proc and proc.returncode is None: proc.terminate()
@app.get('/')
async def index(): return HTMLResponse((ROOT/'index.html').read_text())
@app.get('/bluetooth.js')
async def bluetooth_script(): return FileResponse(ROOT/'bluetooth.js', media_type='text/javascript')
@app.websocket('/ws')
async def socket(ws:WebSocket):
    global connected
    await ws.accept(); clients.add(ws)
    await ws.send_json({'console':'\n'.join(console)})
    try:
        while True:
            command=await ws.receive_text()
            if command=='disconnect':
                connected=False
                if proc and proc.returncode is None:
                    try: proc.terminate()
                    except ProcessLookupError: pass
            elif command=='connect': connected=True
    finally: clients.discard(ws)
if __name__=='__main__':
    p=argparse.ArgumentParser(); p.add_argument('--replay'); p.add_argument('--port'); p.add_argument('--baud',type=int,default=115200); p.add_argument('--host',default='0.0.0.0'); p.add_argument('--http-port',type=int,default=8000); args=p.parse_args()
    import uvicorn
    uvicorn.run(app,host=args.host,port=args.http_port)
