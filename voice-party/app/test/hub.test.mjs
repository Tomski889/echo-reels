import test from 'node:test';import assert from 'node:assert/strict';import {PartyHub,hex,identity} from '../src/hub.mjs';
const encode=new TextEncoder();
function setup(){const data=new Map(),store={get:async k=>structuredClone(data.get(k)),put:async(k,v)=>data.set(k,structuredClone(v))};return {hub:new PartyHub(store),store};}
async function user(hub,name='Player',key){key=key||await crypto.subtle.generateKey({name:'ECDSA',namedCurve:'P-256'},true,['sign','verify']);
  const ws={messages:[],send(s){this.messages.push(JSON.parse(s));},close(){this.closed=true;}};hub.connect(ws);
  const pub=await crypto.subtle.exportKey('jwk',key.publicKey),signature=hex(await crypto.subtle.sign({name:'ECDSA',hash:'SHA-256'},key.privateKey,encode.encode(ws.meta.nonce)));
  await hub.receive(ws,JSON.stringify({type:'auth',name,publicKey:pub,signature}));return {ws,key,id:await identity(pub)};
}
async function act(hub,user,type,body={}){const n=user.ws.messages.length;await hub.receive(user.ws,JSON.stringify({type,...body}));return user.ws.messages.slice(n);}
async function friend(hub,a,b){await act(hub,a,'friend.request',{target:b.id});await act(hub,b,'friend.accept',{target:a.id});}
test('signed profile rejects forged and replayed challenges',async()=>{
  const {hub}=setup(),a=await user(hub),ws={messages:[],send(s){this.messages.push(JSON.parse(s));},close(){}};hub.connect(ws);
  const pub=await crypto.subtle.exportKey('jwk',a.key.publicKey);
  await hub.receive(ws,JSON.stringify({type:'auth',publicKey:pub,signature:'00'.repeat(64)}));assert.equal(ws.messages.at(-1).type,'error');assert.equal(ws.meta.id,undefined);
  await hub.receive(ws,JSON.stringify({type:'party.create'}));assert.equal(ws.messages.at(-1).type,'error');
});
test('friends require acceptance; closed party blocks outsiders and signals',async()=>{
  const {hub}=setup(),a=await user(hub,'Alice'),b=await user(hub,'Bob'),c=await user(hub,'Outside');
  await act(hub,a,'party.create');const party=hub.partyOf(a.id).id;
  assert.equal((await act(hub,a,'party.invite',{target:b.id})).at(-1).type,'error');
  await friend(hub,a,b);await act(hub,a,'party.invite',{target:b.id});
  assert.equal((await act(hub,c,'party.join',{party})).at(-1).type,'error');
  await act(hub,b,'party.join',{party});assert.equal(hub.partyOf(b.id).id,party);
  const before=c.ws.messages.length;
  assert.equal((await act(hub,a,'signal',{target:c.id,data:{kind:'offer',description:{}}})).at(-1).type,'error');assert.equal(c.ws.messages.length,before);
  await act(hub,a,'signal',{target:b.id,data:{kind:'offer',description:{type:'offer',sdp:'test'}}});assert.equal(b.ws.messages.at(-1).type,'signal');
  await act(hub,a,'party.kick',{target:b.id});assert.equal(hub.partyOf(b.id),undefined);
  assert.equal((await act(hub,b,'signal',{target:a.id,data:{kind:'candidate'}})).at(-1).type,'error');
});
test('party capacity, expiry and owner transfer',async()=>{
  const {hub}=setup(),owner=await user(hub),members=[];await act(hub,owner,'party.create');const p=hub.partyOf(owner.id);
  for(let i=0;i<4;i++){const u=await user(hub);members.push(u);await friend(hub,owner,u);await act(hub,owner,'party.invite',{target:u.id});}
  p.invites[members[0].id]=Date.now()-1;assert.equal((await act(hub,members[0],'party.join',{party:p.id})).at(-1).type,'error');
  await act(hub,owner,'party.invite',{target:members[0].id});for(const u of members.slice(0,3))await act(hub,u,'party.join',{party:p.id});
  assert.equal((await act(hub,members[3],'party.join',{party:p.id})).at(-1).type,'error');assert.equal(hub.partyOf(owner.id).members.length,4);
  await hub.disconnect(owner.ws);const remaining=hub.partyOf(members[0].id);assert.equal(remaining.owner,members[0].id);assert.deepEqual(remaining.invites,{});
});
test('friend removal revokes invite; profile persistence and daily-limit rollback',async()=>{
  const {hub,store}=setup(),a=await user(hub),b=await user(hub);await friend(hub,a,b);await act(hub,a,'party.create');const party=hub.partyOf(a.id).id;
  await act(hub,a,'party.invite',{target:b.id});await act(hub,a,'friend.remove',{target:b.id});assert.equal((await act(hub,b,'party.join',{party})).at(-1).type,'error');
  store.put=async()=>{throw Error('Free quota exhausted');};const result=await act(hub,a,'friend.request',{target:b.id});assert.equal(result.at(-1).type,'error');assert.equal(hub.users[b.id].incoming.includes(a.id),false);
  const reload=new PartyHub(store);await reload.ready;assert.ok(reload.users[a.id]);assert.equal(Object.keys(reload.parties).length,0);
});
