"""Echo Party tablet host: isolated Chrome, CDP input and triple-buffered BGRA.
No game injection and no microphone capture until Connect microphone is clicked.
"""
from pathlib import Path
import argparse,base64,ctypes,hashlib,io,json,mmap,os,secrets,socket,struct,subprocess,sys,time,urllib.request
from urllib.parse import urlparse
from PIL import Image

HERE=Path(sys.executable).parent if getattr(sys,'frozen',False) else Path(__file__).resolve().parent
WIDTH,HEIGHT=1024,574
FRAME_BYTES=WIDTH*HEIGHT*4; FRAME_OFFSET=8192; SIZE=FRAME_OFFSET+3*FRAME_BYTES
K=ctypes.WinDLL('kernel32',use_last_error=True)
K.GetTickCount64.restype=ctypes.c_uint64
SYNC=ctypes.CDLL(str(HERE/'BridgeSync.dll'))
SYNC.PublishFrame.argtypes=[ctypes.c_void_p,ctypes.c_int];SYNC.PublishFrame.restype=None

class CDP:
    """Minimal loopback-only WebSocket client, retaining unmatched CDP events."""
    def __init__(self,url):
        u=urlparse(url)
        if u.scheme!='ws' or u.hostname not in ('localhost','127.0.0.1'):raise ValueError('CDP must be loopback')
        self.sock=socket.create_connection((u.hostname,u.port),timeout=10);self.sock.settimeout(15)
        key=base64.b64encode(secrets.token_bytes(16)).decode();self.buf=b'';self.counter=0
        self.sock.sendall((f'GET {u.path} HTTP/1.1\r\nHost: {u.hostname}:{u.port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n').encode())
        while b'\r\n\r\n' not in self.buf:self.buf+=self.sock.recv(4096)
        headers,self.buf=self.buf.split(b'\r\n\r\n',1)
        expected=base64.b64encode(hashlib.sha1((key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
        if b' 101 ' not in headers.split(b'\r\n')[0] or expected not in headers:raise RuntimeError('CDP handshake failed')
    def take(self,n):
        while len(self.buf)<n:
            b=self.sock.recv(max(4096,n-len(self.buf)))
            if not b:raise ConnectionError('Chrome disconnected')
            self.buf+=b
        b,self.buf=self.buf[:n],self.buf[n:];return b
    def send(self,op,data):
        mask=secrets.token_bytes(4);n=len(data);h=bytes([128|op,128|min(n,126)]) if n<65536 else bytes([128|op,255])
        if n>=65536:h+=struct.pack('>Q',n)
        elif n>=126:h+=struct.pack('>H',n)
        self.sock.sendall(h+mask+bytes(b^mask[i%4] for i,b in enumerate(data)))
    def receive(self):
        pieces=[]
        while True:
            a,b=self.take(2);n=b&127
            if n==126:n=struct.unpack('>H',self.take(2))[0]
            elif n==127:n=struct.unpack('>Q',self.take(8))[0]
            if n>16*1024*1024:raise ValueError('Oversized CDP frame')
            mask=self.take(4) if b&128 else None;data=self.take(n)
            if mask:data=bytes(v^mask[i%4] for i,v in enumerate(data))
            op=a&15
            if op==8:raise ConnectionError('Chrome closed CDP')
            if op==9:self.send(10,data);continue
            if op==10:continue
            pieces.append(data)
            if a&128:return json.loads(b''.join(pieces))
    def call(self,method,params=None):
        self.counter+=1;ident=self.counter
        self.send(1,json.dumps(dict(id=ident,method=method,params=params or {})).encode())
        while True:
            message=self.receive()
            if message.get('id')!=ident:continue
            if 'error' in message:raise RuntimeError(f'{method}: {message["error"]}')
            return message.get('result',{})
    def close(self):self.sock.close()

def chrome_path():
    choices=[Path(os.environ.get('PROGRAMFILES','C:/Program Files'))/'Google/Chrome/Application/chrome.exe',Path(os.environ.get('PROGRAMFILES(X86)','C:/Program Files (x86)'))/'Microsoft/Edge/Application/msedge.exe',Path(os.environ.get('PROGRAMFILES','C:/Program Files'))/'Microsoft/Edge/Application/msedge.exe']
    return next(p for p in choices if p.is_file())

class Bridge:
    def __init__(self,name='Local\\EchoParty.Shared.v1'):
        self.mem=mmap.mmap(-1,SIZE,tagname=name)
        if struct.unpack_from('<I',self.mem)[0]!=0x41524344:
            self.mem[:FRAME_OFFSET]=bytes(FRAME_OFFSET)
            struct.pack_into('<8I',self.mem,0,0x41524344,2,WIDTH,HEIGHT,WIDTH*4,32,18,0)
            struct.pack_into('<i',self.mem,36,-1);struct.pack_into('<i',self.mem,168,-1)
        assert struct.unpack_from('<5I',self.mem) == (0x41524344,2,WIDTH,HEIGHT,WIDTH*4)
        self.touch_read=struct.unpack_from('<I',self.mem,184)[0];self.pressed=False;self.point=(0,0)
        struct.pack_into('<i',self.mem,32,os.getpid())
    def heartbeat(self):struct.pack_into('<Q',self.mem,48,K.GetTickCount64())
    def publish(self,pixels):
        assert len(pixels)==FRAME_BYTES
        latest,reading=struct.unpack_from('<i',self.mem,36)[0],struct.unpack_from('<i',self.mem,168)[0]
        index=next(i for i in range(3) if i!=latest and i!=reading)
        self.mem[FRAME_OFFSET+index*FRAME_BYTES:FRAME_OFFSET+(index+1)*FRAME_BYTES]=pixels
        address=ctypes.c_char.from_buffer(self.mem)
        SYNC.PublishFrame(ctypes.addressof(address),index)
        del address
    def input(self,cdp):
        write=struct.unpack_from('<I',self.mem,184)[0]
        if write-self.touch_read>256:self.touch_read=write-256
        while self.touch_read<write:
            seq,cell,kind,xy=struct.unpack_from('<4I',self.mem,296+(self.touch_read%256)*16)
            if seq!=self.touch_read+1:break
            self.touch_read+=1
            if cell>=576 or kind not in (0,1):continue
            x=(cell%32+.5)*WIDTH/32;y=(cell//32+.5)*HEIGHT/18
            if kind==1:
                if self.pressed:cdp.call('Input.dispatchMouseEvent',dict(type='mouseMoved',x=x,y=y,buttons=1))
                else:cdp.call('Input.dispatchMouseEvent',dict(type='mousePressed',x=x,y=y,button='left',buttons=1,clickCount=1));self.pressed=True
                self.point=(x,y)
            elif self.pressed:
                cdp.call('Input.dispatchMouseEvent',dict(type='mouseReleased',x=self.point[0],y=self.point[1],button='left',buttons=0,clickCount=1));self.pressed=False
    def close(self):
        struct.pack_into('<Q',self.mem,48,0);struct.pack_into('<i',self.mem,32,0);self.mem.close()

def main():
    args=argparse.ArgumentParser();args.add_argument('--standalone',action='store_true');args.add_argument('--fake-media-test',action='store_true');args.add_argument('--seconds',type=int,default=0);args.add_argument('--mapping',default='Local\\EchoParty.Shared.v1');args=args.parse_args()
    state=Path(os.environ.get('LOCALAPPDATA',str(HERE)))/('EchoPartyTest' if args.fake_media_test else 'EchoParty');state.mkdir(parents=True,exist_ok=True)
    log=open(state/'host.log','a',encoding='utf-8',buffering=1)
    def note(message):log.write(f'{time.time():.0f} {message}\n')
    lock=state/'host.lock';lock_handle=None;server=browser=cdp=bridge=None
    try:
        import msvcrt
        lock_handle=open(lock,'a+b');lock_handle.seek(0);lock_handle.write(b'0');lock_handle.flush();lock_handle.seek(0)
        try:msvcrt.locking(lock_handle.fileno(),msvcrt.LK_NBLCK,1)
        except OSError:return
        config=json.loads((HERE/'party.json').read_text()) if (HERE/'party.json').exists() else {}
        url=config.get('url','').strip()
        if not url:
            port=int(config.get('local_port',9632));url=f'http://127.0.0.1:{port}/'
            node=HERE/'runtime/node.exe'
            if not node.is_file():raise RuntimeError('Bundled Node runtime missing')
            env=os.environ.copy();env['PORT']=str(port);env['ECHO_PARTY_STATE_DIR']=str(state/'directory')
            server=subprocess.Popen([str(node),str(HERE/'app/src/local.mjs')],cwd=HERE/'app',env=env,stdout=log,stderr=log,creationflags=subprocess.CREATE_NO_WINDOW)
            for _ in range(100):
                if server.poll() is not None:raise RuntimeError('Local Party server failed (port may be busy)')
                try:urllib.request.urlopen(url,timeout=.2).close();break
                except OSError:time.sleep(.1)
            else:raise RuntimeError('Local Party server timed out')
        parsed=urlparse(url)
        if parsed.scheme!='https' and not (parsed.scheme=='http' and parsed.hostname in ('127.0.0.1','localhost')):raise ValueError('Party URL must be HTTPS or local loopback')
        profile=state/'browser';profile.mkdir(exist_ok=True);active=profile/'DevToolsActivePort'
        if active.exists():active.unlink()
        command=[str(chrome_path()),'--user-data-dir='+str(profile),'--remote-debugging-port=0','--remote-debugging-address=127.0.0.1','--no-first-run','--no-default-browser-check','--disable-background-timer-throttling','--disable-backgrounding-occluded-windows','--disable-renderer-backgrounding','--autoplay-policy=no-user-gesture-required','--window-size=1024,574','--app='+url]
        if args.fake_media_test:command+=['--headless=new','--use-fake-device-for-media-stream','--use-fake-ui-for-media-stream']
        si=subprocess.STARTUPINFO();si.dwFlags=subprocess.STARTF_USESHOWWINDOW;si.wShowWindow=7
        browser=subprocess.Popen(command,startupinfo=si,stdout=log,stderr=log)
        for _ in range(150):
            if active.exists():break
            if browser.poll() is not None:raise RuntimeError('Chrome exited before debugging started')
            time.sleep(.1)
        else:raise RuntimeError('Chrome startup timed out')
        port=int(active.read_text().splitlines()[0]);pages=[]
        for _ in range(100):
            pages=json.load(urllib.request.urlopen(f'http://127.0.0.1:{port}/json/list',timeout=2));pages=[p for p in pages if p['type']=='page' and p['url'].startswith(url.rstrip('/'))]
            if pages:break
            time.sleep(.1)
        if not pages:raise RuntimeError('Party page missing')
        cdp=CDP(pages[0]['webSocketDebuggerUrl']);cdp.call('Page.enable');cdp.call('Emulation.setDeviceMetricsOverride',dict(width=WIDTH,height=HEIGHT,deviceScaleFactor=1,mobile=False))
        cdp.call('Browser.grantPermissions',dict(permissions=['audioCapture'],origin=f'{parsed.scheme}://{parsed.netloc}'))
        bridge=Bridge(args.mapping);start=time.monotonic();note('Party browser ready; capture begins. Microphone requires Connect microphone.')
        while not args.seconds or time.monotonic()-start<args.seconds:
            bridge.heartbeat();tick=struct.unpack_from('<Q',bridge.mem,176)[0]
            if not args.standalone and K.GetTickCount64()-tick>10000:break
            bridge.input(cdp)
            if args.standalone or struct.unpack_from('<i',bridge.mem,164)[0]:
                image=cdp.call('Page.captureScreenshot',dict(format='png',captureBeyondViewport=False,fromSurface=True))
                pixels=Image.open(io.BytesIO(base64.b64decode(image['data']))).convert('RGBA')
                if pixels.size!=(WIDTH,HEIGHT):pixels=pixels.resize((WIDTH,HEIGHT))
                bridge.publish(pixels.tobytes('raw','BGRA'))
            time.sleep(.08)
    except Exception as error:
        note(f'ERROR: {type(error).__name__}: {error}')
        return 1
    finally:
        if cdp:
            try:cdp.call('Browser.close')
            except Exception:pass
            cdp.close()
        if browser:
            try:browser.wait(timeout=5)
            except subprocess.TimeoutExpired:browser.terminate()
        if server:
            server.terminate()
            try:server.wait(timeout=5)
            except subprocess.TimeoutExpired:server.kill()
        if bridge:bridge.close()
        if lock_handle:lock_handle.close()
        note('Party host stopped');log.close()
    return 0
if __name__=='__main__':sys.exit(main())
