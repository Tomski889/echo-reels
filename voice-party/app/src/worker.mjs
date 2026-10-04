import {PartyHub} from './hub.mjs';
const headers={
  'Content-Security-Policy':"default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self' wss:; media-src 'self' blob:; img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'; form-action 'self'",
  'X-Content-Type-Options':'nosniff','Referrer-Policy':'no-referrer','Permissions-Policy':'microphone=(self), camera=()'
};
export default {async fetch(request,env){
  const url=new URL(request.url);
  if(url.pathname==='/connect'){
    if(request.headers.get('Origin')!==url.origin)return new Response('Origin rejected',{status:403});
    return env.PARTY.get(env.PARTY.idFromName('community-v1')).fetch(request);
  }
  const response=await env.ASSETS.fetch(request);const h=new Headers(response.headers);for(const [k,v] of Object.entries(headers))h.set(k,v);
  return new Response(response.body,{status:response.status,headers:h});
}};
export class PartyDirectory{
  constructor(ctx){this.ctx=ctx;this.hub=new PartyHub(ctx.storage);for(const ws of ctx.getWebSockets())this.hub.restore(ws,ws.deserializeAttachment());}
  async fetch(request){
    if(request.headers.get('Upgrade')?.toLowerCase()!=='websocket')return new Response('WebSocket required',{status:426});
    const pair=new WebSocketPair(),client=pair[0],server=pair[1];this.ctx.acceptWebSocket(server);
    this.hub.connect(server);server.serializeAttachment(server.meta);
    return new Response(null,{status:101,webSocket:client});
  }
  async webSocketMessage(ws,message){await this.hub.receive(ws,message);ws.serializeAttachment(ws.meta);}
  async webSocketClose(ws){await this.hub.disconnect(ws);}
  async webSocketError(ws){await this.hub.disconnect(ws);}
}
