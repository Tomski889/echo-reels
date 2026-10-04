const $=id=>document.getElementById(id),encoder=new TextEncoder();
let socket,profile,state,stream,muted=true,deafened=false,volume=1;const peers=new Map();
const notice=text=>{$('notice').textContent=text;};
const hex=bytes=>Array.from(new Uint8Array(bytes),b=>b.toString(16).padStart(2,'0')).join('');
const send=msg=>{if(socket?.readyState===WebSocket.OPEN)socket.send(JSON.stringify(msg));else notice('Connect first.');};
async function identity(){
  const db=await new Promise((resolve,reject)=>{const r=indexedDB.open('echo-party',1);r.onupgradeneeded=()=>r.result.createObjectStore('profile');r.onsuccess=()=>resolve(r.result);r.onerror=()=>reject(r.error);});
  const saved=await new Promise((resolve,reject)=>{const r=db.transaction('profile').objectStore('profile').get('key');r.onsuccess=()=>resolve(r.result);r.onerror=()=>reject(r.error);});
  if(saved){db.close();return saved;}
  const key=await crypto.subtle.generateKey({name:'ECDSA',namedCurve:'P-256'},false,['sign','verify']);
  await new Promise((resolve,reject)=>{const t=db.transaction('profile','readwrite');t.objectStore('profile').put(key,'key');t.oncomplete=resolve;t.onerror=()=>reject(t.error);});db.close();return key;
}
function tab(id){for(const page of document.querySelectorAll('.page'))page.hidden=page.id!==id;for(const b of document.querySelectorAll('.tab'))b.classList.toggle('selected',b.dataset.tab===id);}
for(const b of document.querySelectorAll('.tab'))b.onclick=()=>tab(b.dataset.tab);
$('name').value=localStorage.getItem('echo-party-name')||'';
$('login').onsubmit=async event=>{event.preventDefault();try{
  profile=await identity();localStorage.setItem('echo-party-name',$('name').value.trim());
  if(socket)socket.close();socket=new WebSocket(`${location.protocol==='https:'?'wss':'ws'}://${location.host}/connect`);
  $('status').textContent='Connecting';
  socket.onmessage=async event=>{try{
    const msg=JSON.parse(event.data);
    if(msg.type==='challenge'){
      const publicKey=await crypto.subtle.exportKey('jwk',profile.publicKey);
      const signature=hex(await crypto.subtle.sign({name:'ECDSA',hash:'SHA-256'},profile.privateKey,encoder.encode(msg.nonce)));
      send({type:'auth',publicKey,signature,name:$('name').value.trim()});
    }else if(msg.type==='state'){const oldParty=state?.party?.id;state=msg;if(oldParty&&oldParty!==state.party?.id)stopVoice();render();reconcile();}
    else if(msg.type==='signal')await signal(msg.from,msg.data);
    else if(msg.type==='error')notice(msg.message);
  }catch(error){notice(error.message);}};
  socket.onclose=()=>{stopVoice();state=null;$('status').textContent='Offline';$('signin').hidden=false;$('app').hidden=true;notice('Disconnected. Connect again to rejoin.');};
  socket.onerror=()=>notice('Connection unavailable. Check that the server is running.');
}catch(error){notice(error.message);}};
$('copy-id').onclick=async()=>{try{await navigator.clipboard.writeText(state.me.id);notice('Friend ID copied.');}catch{notice('Your friend ID: '+state.me.id);}};
$('add-friend').onsubmit=e=>{e.preventDefault();send({type:'friend.request',target:$('friend-id').value.toLowerCase().trim()});$('friend-id').value='';};
$('create').onclick=()=>{send({type:'party.create'});tab('party');};
$('leave').onclick=()=>{stopVoice();send({type:'party.leave'});};
function button(text,action,style='secondary'){const b=document.createElement('button');b.textContent=text;b.className=style;b.onclick=action;return b;}
function row(person,details,buttons=[]){const r=document.createElement('div');r.className='row';const info=document.createElement('div'),name=document.createElement('div'),meta=document.createElement('div');
  name.className='name';name.textContent=person.name;meta.className='meta';meta.textContent=details;meta.dataset.peer=person.id;info.append(name,meta);const actions=document.createElement('div');actions.className='actions';actions.append(...buttons);r.append(info,actions);return r;
}
function render(){
  $('signin').hidden=true;$('app').hidden=false;$('status').textContent='Connected';$('copy-id').textContent=state.me.id;
  $('friend-count').textContent=`· ${state.friends.length}`;$('friend-list').replaceChildren();
  if(!state.friends.length){const p=document.createElement('p');p.textContent='Share your ID with a friend to get started.';$('friend-list').append(p);}
  for(const f of state.friends){const buttons=[];
    if(state.party?.owner===state.me.id&&!state.party.members.some(m=>m.id===f.id))buttons.push(button('Invite',()=>{send({type:'party.invite',target:f.id});notice('Invite sent.');}));
    buttons.push(button('Remove',()=>{if(confirm(`Remove ${f.name} from your friends?`))send({type:'friend.remove',target:f.id});}));
    $('friend-list').append(row(f,f.online?'Online':'Offline',buttons));
  }
  $('requests').replaceChildren();for(const f of state.requests)$('requests').append(row(f,'Friend request',[button('Accept',()=>send({type:'friend.accept',target:f.id}),''),button('Decline',()=>send({type:'friend.decline',target:f.id}))]));
  $('invites').replaceChildren();for(const inv of state.invites)$('invites').append(row(inv.owner,'Invited you to a private party',[button('Join',()=>{send({type:'party.join',party:inv.id});tab('party');},''),button('Decline',()=>send({type:'party.decline',party:inv.id}))]));
  $('party-empty').hidden=Boolean(state.party);$('leave').hidden=!state.party;$('create').disabled=Boolean(state.party);$('voice').disabled=!state.party;$('members').replaceChildren();
  for(const p of state.party?.members||[]){const buttons=[];if(state.party.owner===state.me.id&&p.id!==state.me.id)buttons.push(button('Remove',()=>send({type:'party.kick',target:p.id}),'danger'));
    $('members').append(row({...p,name:p.name+(p.id===state.me.id?' (you)':'')},p.id===state.me.id?(stream?(muted?'Microphone muted':'Microphone connected'):'Microphone off'):(peers.get(p.id)?.pc.connectionState||'Connecting'),buttons));}
  controls();
}
function controls(){
  $('voice').textContent=stream?'Microphone connected':'Connect microphone';$('mute').disabled=!stream;$('mute').textContent=muted?'Unmute microphone':'Mute microphone';
  $('deafen').disabled=!state?.party;$('deafen').textContent=deafened?'Undeafen':'Deafen';
}
async function devices(){const list=await navigator.mediaDevices.enumerateDevices(),selected=$('microphone').value;
  $('microphone').replaceChildren(new Option('Default microphone',''));for(const d of list.filter(d=>d.kind==='audioinput'))$('microphone').add(new Option(d.label||'Microphone',d.deviceId));$('microphone').value=selected;
}
async function microphone(){
  if(!state?.party)throw Error('Join or create a party first');
  const selected=$('microphone').value;
  const next=await navigator.mediaDevices.getUserMedia({audio:{echoCancellation:true,noiseSuppression:true,autoGainControl:true,...(selected?{deviceId:{exact:selected}}:{})},video:false});
  const old=stream;stream=next;muted=false;const track=stream.getAudioTracks()[0];track.enabled=!deafened;
  for(const entry of peers.values()){
    const sender=entry.pc.getSenders().find(s=>s.track?.kind==='audio');if(sender)await sender.replaceTrack(track);else entry.pc.addTrack(track,stream);
    entry.audio.play().catch(()=>{});
  }
  if(old)old.getTracks().forEach(t=>t.stop());await devices();controls();render();
  notice('Microphone connected. Mute Echo’s game chat separately if you only want your party to hear you.');
}
$('voice').onclick=()=>microphone().catch(e=>notice('Microphone unavailable: '+e.message));
$('refresh-devices').onclick=()=>devices().catch(e=>notice(e.message));
$('microphone').onchange=()=>{if(stream)microphone().catch(e=>notice(e.message));};
$('mute').onclick=()=>{muted=!muted;for(const t of stream?.getAudioTracks()||[])t.enabled=!muted&&!deafened;controls();render();};
$('deafen').onclick=()=>{deafened=!deafened;for(const t of stream?.getAudioTracks()||[])t.enabled=!muted&&!deafened;for(const e of peers.values())e.audio.muted=deafened;controls();};
$('volume').oninput=()=>{volume=Number($('volume').value);$('volume-value').textContent=Math.round(volume*100)+'%';for(const e of peers.values())e.audio.volume=volume;};
function makePeer(id){
  if(peers.has(id))return peers.get(id);
  const pc=new RTCPeerConnection({iceServers:[{urls:'stun:stun.cloudflare.com:3478'}]}),audio=new Audio();audio.autoplay=true;audio.volume=volume;audio.muted=deafened;
  const entry={pc,audio,makingOffer:false,ignoreOffer:false,answerPending:false,candidates:[],polite:state.me.id>id};peers.set(id,entry);
  if(stream)for(const t of stream.getTracks())pc.addTrack(t,stream);else pc.addTransceiver('audio',{direction:'recvonly'});
  pc.onicecandidate=event=>{if(event.candidate)send({type:'signal',target:id,data:{kind:'candidate',candidate:event.candidate.toJSON()}});};
  pc.ontrack=event=>{audio.srcObject=event.streams[0]||new MediaStream([event.track]);audio.play().catch(()=>notice('Click Connect microphone to allow party audio playback.'));};
  pc.onnegotiationneeded=async()=>{try{entry.makingOffer=true;await pc.setLocalDescription();send({type:'signal',target:id,data:{kind:pc.localDescription.type,description:pc.localDescription.toJSON()}});}catch(e){if(pc.connectionState!=='closed')notice('Voice negotiation: '+e.message);}finally{entry.makingOffer=false;}};
  pc.onconnectionstatechange=()=>{
    for(const label of document.querySelectorAll('[data-peer]'))if(label.dataset.peer===id)label.textContent=pc.connectionState;
    if(pc.connectionState==='failed')notice('Direct voice could not connect on this network. This free build has no paid relay.');
  };
  return entry;
}
async function signal(id,data){
  if(!state?.party?.members.some(m=>m.id===id))return;
  const e=makePeer(id),pc=e.pc;
  if(data.kind==='candidate'){
    if(e.ignoreOffer)return;
    if(pc.remoteDescription)await pc.addIceCandidate(data.candidate);else e.candidates.push(data.candidate);return;
  }
  const description=data.description;if(!description||description.type!==data.kind)throw Error('Invalid voice description');
  const collision=description.type==='offer'&&!(!e.makingOffer&&(pc.signalingState==='stable'||e.answerPending));
  e.ignoreOffer=!e.polite&&collision;if(e.ignoreOffer)return;
  e.answerPending=description.type==='answer';try{await pc.setRemoteDescription(description);}finally{e.answerPending=false;}
  for(const candidate of e.candidates.splice(0))await pc.addIceCandidate(candidate);
  if(description.type==='offer'){await pc.setLocalDescription();send({type:'signal',target:id,data:{kind:'answer',description:pc.localDescription.toJSON()}});}
}
function removePeer(id){const p=peers.get(id);if(!p)return;p.pc.close();p.audio.pause();p.audio.srcObject=null;peers.delete(id);}
function reconcile(){const members=new Set((state?.party?.members||[]).filter(m=>m.id!==state.me.id).map(m=>m.id));for(const id of peers.keys())if(!members.has(id))removePeer(id);for(const id of members)makePeer(id);}
function stopVoice(){for(const id of [...peers.keys()])removePeer(id);for(const t of stream?.getTracks()||[])t.stop();stream=null;muted=true;deafened=false;controls();}
window.addEventListener('pagehide',()=>{stopVoice();socket?.close();});
