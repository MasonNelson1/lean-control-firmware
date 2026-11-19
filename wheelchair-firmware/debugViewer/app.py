import asyncio, csv, json, os, time, argparse, sys, io, zipfile, zlib
from collections import deque
from pathlib import Path
from fastapi import FastAPI, WebSocket, Request, HTTPException
from fastapi.responses import HTMLResponse, FileResponse, Response

try:
    import pty
except ImportError:
    pty = None

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
            row['mode'] = mode if mode in ('LEAN', 'REMOTE', 'DISARM') else 'UNKNOWN'
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
    if pty is None:
        await note('Wired monitoring requires Linux or WSL. Use Bluetooth or start the viewer with --replay.')
        return
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
async def index(): return HTMLResponse((ROOT/'index.html').read_text(encoding='utf-8'))
@app.get('/bluetooth.js')
async def bluetooth_script(): return FileResponse(ROOT/'bluetooth.js', media_type='text/javascript')
@app.get('/config-protocol.js')
async def config_protocol_script(): return FileResponse(ROOT/'config-protocol.js', media_type='text/javascript')
@app.get('/configuration.js')
async def configuration_script(): return FileResponse(ROOT/'configuration.js', media_type='text/javascript')
@app.get('/pid-preview.js')
async def pid_preview_script(): return FileResponse(ROOT/'pid-preview.js', media_type='text/javascript')

# Profile files are handled only by the dashboard; these routes never access a
# serial connection or change the running/saved wheelchair configuration.
PROFILE_LIMIT = 1024 * 1024

def profile_json(data):
    try:
        profile = json.loads(data)
        if not isinstance(profile, dict) or profile.get('format') != 'lean-config' or \
                type(profile.get('version')) is not int or profile['version'] != 1 or \
                not isinstance(profile.get('fields'), list):
            raise ValueError()
        result = json.dumps(profile, ensure_ascii=False, allow_nan=False, indent=2).encode('utf-8')
        if len(result) > PROFILE_LIMIT:
            raise ValueError()
        return result
    except (ValueError, TypeError, UnicodeError, RecursionError):
        raise HTTPException(400, 'Invalid or unsupported Lean configuration JSON.')

async def profile_body(request):
    data = bytearray()
    async for chunk in request.stream():
        data.extend(chunk)
        if len(data) > PROFILE_LIMIT:
            raise HTTPException(413, 'Profile files must be at most 1 MiB.')
    return bytes(data)

@app.post('/profiles/export')
async def export_profile(request: Request):
    data = profile_json(await profile_body(request))
    archive = io.BytesIO()
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED) as profile:
        profile.writestr('profile.json', data)
    return Response(archive.getvalue(), media_type='application/zip',
                    headers={'Content-Disposition': 'attachment; filename="configuration.lean"'})

@app.post('/profiles/import')
async def import_profile(request: Request):
    data = await profile_body(request)
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            entries = archive.infolist()
            if len(entries) != 1 or entries[0].filename != 'profile.json' or \
                    entries[0].file_size > PROFILE_LIMIT or entries[0].flag_bits & 1 or \
                    entries[0].compress_type != zipfile.ZIP_DEFLATED:
                raise ValueError()
            # Bound actual decompression too, rather than trusting ZIP metadata.
            with archive.open(entries[0]) as source:
                content = source.read(PROFILE_LIMIT + 1)
            if len(content) > PROFILE_LIMIT:
                raise ValueError()
    except (ValueError, OSError, EOFError, RuntimeError, NotImplementedError, zipfile.BadZipFile, zlib.error):
        raise HTTPException(400, 'Invalid .lean file. Expected a ZIP containing compressed profile.json.')
    return Response(profile_json(content), media_type='application/json')

# Fixed receiver only: never accept a command or address from the browser.
@app.post('/bluetooth/disconnect')
async def release_bluetooth(request: Request):
    if request.client.host not in ('127.0.0.1', '::1') or request.headers.get('x-bluetooth-cleanup') != '1':
        raise HTTPException(403, 'Bluetooth cleanup requires a local dashboard connection.')
    if sys.platform != 'linux':
        return {'message': 'Linux Bluetooth cleanup skipped on this platform.'}
    try:
        cleanup = await asyncio.create_subprocess_exec(
            'bluetoothctl', 'disconnect', '98:DA:60:0A:41:3C',
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
        try:
            stdout, stderr = await asyncio.wait_for(cleanup.communicate(), 8)
        except asyncio.TimeoutError:
            cleanup.kill()
            await cleanup.communicate()
            raise HTTPException(504, 'Bluetooth disconnect timed out.')
    except FileNotFoundError:
        raise HTTPException(503, 'bluetoothctl is not installed on the dashboard server.')
    output = (stdout + stderr).decode(errors='replace').strip()
    if cleanup.returncode and 'org.bluez.Error.NotConnected' not in output:
        raise HTTPException(503, output or 'Linux Bluetooth disconnect failed.')
    return {'message': 'Linux receiver connection released.'}

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
