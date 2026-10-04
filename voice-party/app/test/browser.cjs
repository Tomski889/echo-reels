const {chromium}=require('playwright');const assert=require('node:assert/strict');
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files/Google/Chrome/Application/chrome.exe',headless:true,args:['--use-fake-device-for-media-stream','--use-fake-ui-for-media-stream','--autoplay-policy=no-user-gesture-required']});
 try{
  const contexts=await Promise.all([browser.newContext({permissions:['microphone'],viewport:{width:1024,height:574}}),browser.newContext({permissions:['microphone'],viewport:{width:1024,height:574}})]);
  const pages=await Promise.all(contexts.map(c=>c.newPage()));const errors=[];
  for(const page of pages){page.on('pageerror',e=>errors.push(e.message));await page.addInitScript(()=>{window.testPeers=[];const Original=window.RTCPeerConnection;window.RTCPeerConnection=class extends Original{constructor(...args){super(...args);window.testPeers.push(this);}};});}
  for(let i=0;i<2;i++){await pages[i].goto(process.env.ECHO_PARTY_TEST_URL||'http://127.0.0.1:9630');await pages[i].locator('#name').fill(i?'Bob':'Alice');await pages[i].locator('#login button').click();await pages[i].locator('#app').waitFor({state:'visible'});}
  const [alice,bob]=pages;const ids=await Promise.all(pages.map(p=>p.locator('#copy-id').textContent()));
  await alice.locator('#friend-id').fill(ids[1]);await alice.locator('#add-friend button').click();await bob.locator('#requests').getByRole('button',{name:'Accept',exact:true}).click();
  await alice.locator('#create').click();await alice.getByRole('button',{name:'Friends',exact:true}).click();await alice.locator('#friend-list').getByRole('button',{name:'Invite',exact:true}).click();
  await bob.getByRole('button',{name:'Party',exact:true}).click();await bob.locator('#invites').getByRole('button',{name:'Join',exact:true}).click();
  await alice.getByRole('button',{name:'Party',exact:true}).click();
  for(const page of pages){await page.locator('#voice').click();await page.waitForFunction(()=>document.getElementById('voice').textContent==='Microphone connected');}
  for(const page of pages)await page.waitForFunction(()=>window.testPeers.some(pc=>pc.connectionState==='connected'),{},{timeout:20000});
  for(const page of pages)await page.waitForFunction(async()=>{for(const pc of window.testPeers){for(const stat of (await pc.getStats()).values())if(stat.type==='inbound-rtp'&&stat.kind==='audio'&&stat.bytesReceived>0)return true;}return false;},{},{timeout:20000});
  await alice.getByRole('button',{name:'Friends',exact:true}).click();assert.equal(await alice.evaluate(()=>window.testPeers[0].connectionState),'connected');await alice.getByRole('button',{name:'Party',exact:true}).click();
  await alice.locator('#mute').click();assert.equal(await alice.locator('#mute').textContent(),'Unmute microphone');
  await bob.locator('#deafen').click();assert.equal(await bob.locator('#deafen').textContent(),'Undeafen');
  await alice.screenshot({path:'docs/party-preview.png'});
  await bob.locator('#leave').click();await alice.waitForFunction(()=>document.querySelectorAll('#members .row').length===1);
  assert.deepEqual(errors,[]);console.log('PASS: two browser identities, accepted friends, invite-only party, received fake audio, voice survives tab changes, mute/deafen and leave');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
