// Local demo only. Public deployments use the HTTPS Cloudflare adapter.
import http from 'node:http';import {createHash} from 'node:crypto';
import {readFile,mkdir,writeFile} from 'node:fs/promises';import path from 'node:path';import {fileURLToPath} from 'node:url';
import {PartyHub} from './hub.mjs';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..'),port=Number(process.env.PORT||9630),folder=process.env.ECHO_PARTY_STATE_DIR||path.join(root,'.local');
let stored;try{stored=JSON.parse(await readFile(path.join(folder,'state.json'),'utf8'));}catch{}
const hub=new PartyHub({get:async()=>stored,put:async(k,value)=>{await mkdir(folder,{recursive:true});const json=JSON.stringify(value);await writeFile(path.join(folder,'state.json'),json);stored=JSON.parse(json);}});
const allowed=new Set([`localhost:${port}`,`127.0.0.1:${port}`]);
const server=http.createServer(async(req,res)=>{
  if(!allowed.has(req.headers.host)){res.writeHead(403);res.end('Host rejected');return;}
  try{
    const pathname=decodeURIComponent(new URL(req.url,`http://${req.headers.host}`).pathname);
    const file=path.resolve(root,'public',pathname==='/'?'index.html':'.'+pathname);
    if(!file.startsWith(path.join(root,'public')+path.sep))throw Error('Invalid path');
    const body=await readFile(file),type={'.html':'text/html','.js':'text/javascript','.css':'text/css','.svg':'image/svg+xml'}[path.extname(file)]||'application/octet-stream';
    res.writeHead(200,{'Content-Type':type,'X-Content-Type-Options':'nosniff','Cache-Control':'no-store'});res.end(body);
  }catch{res.writeHead(404);res.end('Not found');}
});
function frame(op,data){data=Buffer.from(data);const h=Buffer.alloc(data.length<126?2:4);h[0]=128|op;
  if(data.length<126)h[1]=data.length;else{h[1]=126;h.writeUInt16BE(data.length,2);}return Buffer.concat([h,data]);}
server.on('upgrade',(req,socket,head)=>{
  if(!allowed.has(req.headers.host)||req.url!=='/connect'||req.headers.origin!==`http://${req.headers.host}`||req.headers['sec-websocket-version']!=='13'||!/^[A-Za-z0-9+/]{22}==$/.test(req.headers['sec-websocket-key']||'')){socket.end('HTTP/1.1 403 Forbidden\r\n\r\n');return;}
  const accept=createHash('sha1').update(req.headers['sec-websocket-key']+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
  socket.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`);
  const ws={send:text=>{if(!socket.destroyed)socket.write(frame(1,text));},close:(code=1000,reason='')=>{const b=Buffer.alloc(2);b.writeUInt16BE(code);socket.end(frame(8,Buffer.concat([b,Buffer.from(reason).subarray(0,120)])));}};
  let buffer=Buffer.alloc(0),ended=false;
  function stop(){if(!ended){ended=true;hub.disconnect(ws).catch(()=>{});}}
  socket.on('close',stop);socket.on('error',stop);
  function consume(chunk){buffer=Buffer.concat([buffer,chunk]);if(buffer.length>100000){ws.close(1009);return;}
    while(buffer.length>=2){const op=buffer[0]&15,masked=buffer[1]&128;let size=buffer[1]&127,index=2;
      if(!(buffer[0]&128)||buffer[0]&112||!masked||size===127){ws.close(1003);return;}
      if(size===126){if(buffer.length<4)return;size=buffer.readUInt16BE(2);index=4;}
      if(size>24000){ws.close(1009);return;}if(buffer.length<index+4+size)return;
      const mask=buffer.subarray(index,index+4),body=Buffer.from(buffer.subarray(index+4,index+4+size));buffer=buffer.subarray(index+4+size);
      for(let i=0;i<body.length;i++)body[i]^=mask[i%4];
      if(op===8){ws.close();return;}if(op===9){socket.write(frame(10,body));continue;}if(op===10)continue;
      if(op!==1){ws.close(1003);return;}hub.receive(ws,body.toString('utf8')).catch(()=>ws.close(1011));
    }
  }
  hub.connect(ws);socket.on('data',consume);if(head.length)consume(head);
});
server.listen(port,'127.0.0.1',()=>console.log(`Echo Party local demo: http://127.0.0.1:${port}\nThis is not a public server. Deploy on Workers Free for friends elsewhere.`));
