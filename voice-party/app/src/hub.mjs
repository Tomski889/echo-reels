const enc=new TextEncoder();
export const limits={accounts:2000,friends:50,party:4,connections:100,messagesPerSecond:40};
export const hex=bytes=>Array.from(new Uint8Array(bytes),b=>b.toString(16).padStart(2,'0')).join('');
export const unhex=s=>{if(typeof s!=='string'||!/^([0-9a-f]{2})+$/.test(s))throw Error('Invalid signature');return Uint8Array.from(s.match(/../g),b=>parseInt(b,16));};
export async function identity(publicKey){
  if(publicKey?.kty!=='EC'||publicKey.crv!=='P-256'||typeof publicKey.x!=='string'||typeof publicKey.y!=='string'||publicKey.d)throw Error('Invalid public key');
  return hex(await crypto.subtle.digest('SHA-256',enc.encode(JSON.stringify({crv:'P-256',x:publicKey.x,y:publicKey.y})))).slice(0,16);
}
export class PartyHub{
  constructor(storage){this.storage=storage;this.users={};this.parties={};this.sockets=new Set();this.tail=Promise.resolve();this.ready=this.load();}
  async load(){const s=await this.storage.get('directory');if(s){this.users=s.users||{};this.parties=s.parties||{};}
    for(const p of Object.values(this.parties)){p.members=p.members.filter(id=>this.online(id));if(!p.members.length)delete this.parties[p.id];else if(!p.members.includes(p.owner)){p.owner=p.members[0];p.invites={};}}
  }
  async save(){await this.storage.put('directory',{users:this.users,parties:this.parties});}
  connect(socket){
    if(this.sockets.size>=limits.connections){socket.close(1013,'Community full; try again later');return;}
    this.sockets.add(socket);socket.meta={nonce:hex(crypto.getRandomValues(new Uint8Array(32))),issued:Date.now(),rateAt:Date.now(),rate:0};
    socket.send(JSON.stringify({type:'challenge',nonce:socket.meta.nonce}));
  }
  restore(socket,meta){socket.meta=meta;this.sockets.add(socket);}
  receive(socket,raw){const run=async()=>{await this.ready;let before,oldId=socket.meta?.id;try{
    if(typeof raw!=='string'||raw.length>24000)throw Error('Message too large');
    const now=Date.now(),m=socket.meta;if(!m)throw Error('Connection is closed');
    if(now-m.rateAt>=1000){m.rateAt=now;m.rate=0;}if(++m.rate>limits.messagesPerSecond)throw Error('Too many messages; wait a moment');
    const msg=JSON.parse(raw);if(!msg||typeof msg!=='object')throw Error('Invalid message');
    if(msg.type!=='signal')before=structuredClone({users:this.users,parties:this.parties});
    if(!m.id){if(msg.type!=='auth')throw Error('Sign in first');await this.auth(socket,msg);return;}
    await this.action(socket,msg);
  }catch(error){if(before){this.users=before.users;this.parties=before.parties;socket.meta.id=oldId;}this.send(socket,{type:'error',message:error.message||'Request failed'});}};
    const task=this.tail.then(run);this.tail=task.catch(()=>{});return task;
  }
  send(socket,data){try{socket.send(JSON.stringify(data));}catch{}}
  online(id){return [...this.sockets].some(s=>s.meta?.id===id);}
  to(id,data){for(const s of this.sockets)if(s.meta?.id===id)this.send(s,data);}
  partyOf(id){return Object.values(this.parties).find(p=>p.members.includes(id));}
  async auth(socket,msg){
    if(Date.now()-socket.meta.issued>60000)throw Error('Sign-in expired; reconnect');
    const id=await identity(msg.publicKey);
    const key=await crypto.subtle.importKey('jwk',msg.publicKey,{name:'ECDSA',namedCurve:'P-256'},false,['verify']);
    if(!await crypto.subtle.verify({name:'ECDSA',hash:'SHA-256'},key,unhex(msg.signature),enc.encode(socket.meta.nonce)))throw Error('Sign-in failed');
    if(this.online(id))throw Error('This profile is already connected in another window');
    const name=String(msg.name||'Player').normalize('NFKC').trim().slice(0,24)||'Player';
    if(!this.users[id]){if(Object.keys(this.users).length>=limits.accounts)throw Error('Community profile limit reached');this.users[id]={id,name,friends:[],incoming:[]};}
    this.users[id].name=name;socket.meta.id=id;delete socket.meta.nonce;await this.save();this.broadcast();
  }
  snapshot(id){
    const me=this.users[id],p=this.partyOf(id),person=i=>({id:i,name:this.users[i]?.name||'Player',online:this.online(i)});
    return {type:'state',me:{id,name:me.name},friends:me.friends.map(person),requests:me.incoming.map(person),
      sent:Object.values(this.users).filter(u=>u.incoming.includes(id)).map(u=>u.id),
      invites:Object.values(this.parties).filter(p=>p.invites[id]>Date.now()).map(p=>({id:p.id,owner:person(p.owner),members:p.members.length})),
      party:p?{id:p.id,owner:p.owner,members:p.members.map(person)}:null};
  }
  broadcast(){for(const s of this.sockets)if(s.meta?.id)this.send(s,this.snapshot(s.meta.id));}
  leave(id){const p=this.partyOf(id);if(!p)return;
    p.members=p.members.filter(i=>i!==id);if(!p.members.length){delete this.parties[p.id];return;}
    if(p.owner===id){p.owner=p.members[0];p.invites={};}
  }
  async action(socket,msg){
    const id=socket.meta.id,u=this.users[id],target=msg.target,other=this.users[target];let dirty=true;
    switch(msg.type){
      case 'friend.request':
        if(!other||target===id)throw Error('That friend ID was not found');
        if(u.friends.includes(target)||other.incoming.includes(id))throw Error('Already added or requested');
        if(u.friends.length>=limits.friends||other.friends.length>=limits.friends||other.incoming.length>=limits.friends)throw Error('Friend list is full');
        other.incoming.push(id);break;
      case 'friend.accept':
        if(!other||!u.incoming.includes(target))throw Error('No friend request from that player');
        if(u.friends.length>=limits.friends||other.friends.length>=limits.friends)throw Error('Friend list is full');
        u.incoming=u.incoming.filter(i=>i!==target);other.incoming=other.incoming.filter(i=>i!==id);
        if(!u.friends.includes(target))u.friends.push(target);if(!other.friends.includes(id))other.friends.push(id);break;
      case 'friend.decline':u.incoming=u.incoming.filter(i=>i!==target);break;
      case 'friend.remove':
        u.friends=u.friends.filter(i=>i!==target);if(other)other.friends=other.friends.filter(i=>i!==id);
        for(const p of Object.values(this.parties))if(p.owner===id)delete p.invites[target];else if(p.owner===target)delete p.invites[id];break;
      case 'party.create':
        if(this.partyOf(id))throw Error('Leave your current party first');
        const partyId=hex(crypto.getRandomValues(new Uint8Array(16)));
        this.parties[partyId]={id:partyId,owner:id,members:[id],invites:{}};break;
      case 'party.invite':{
        const p=this.partyOf(id);if(!p||p.owner!==id)throw Error('Only the party owner can invite');
        if(!u.friends.includes(target)||!other?.friends.includes(id))throw Error('Add and accept this friend first');
        if(p.members.length>=limits.party)throw Error('Party is full');
        p.invites[target]=Date.now()+15*60*1000;break;}
      case 'party.join':{
        const p=this.parties[msg.party];if(!p||!p.invites[id]||p.invites[id]<=Date.now())throw Error('Invite expired or unavailable');
        if(!u.friends.includes(p.owner)||!this.users[p.owner]?.friends.includes(id))throw Error('Party owner must still be your friend');
        if(this.partyOf(id))throw Error('Leave your current party first');if(p.members.length>=limits.party)throw Error('Party is full');
        p.members.push(id);delete p.invites[id];break;}
      case 'party.decline':{const p=this.parties[msg.party];if(p)delete p.invites[id];break;}
      case 'party.leave':this.leave(id);break;
      case 'party.kick':{
        const p=this.partyOf(id);if(!p||p.owner!==id||target===id||!p.members.includes(target))throw Error('Cannot remove that player');this.leave(target);break;}
      case 'signal':{
        dirty=false;const p=this.partyOf(id);
        if(!p||!p.members.includes(target)||target===id||!this.online(target))throw Error('Voice peer is outside your party');
        if(!msg.data||!['offer','answer','candidate'].includes(msg.data.kind))throw Error('Invalid voice signal');
        this.to(target,{type:'signal',from:id,data:msg.data});break;}
      default:throw Error('Unknown action');
    }
    if(dirty){await this.save();this.broadcast();}
  }
  disconnect(socket){const run=async()=>{await this.ready;this.sockets.delete(socket);const id=socket.meta?.id;
    if(id){this.leave(id);await this.save();this.broadcast();}};const task=this.tail.then(run);this.tail=task.catch(()=>{});return task;}
}
