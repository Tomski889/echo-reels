// Touch keyboard for the tablet: no system clipboard or desktop keyboard needed.
let target=null;
const keyboard=document.createElement('aside');keyboard.className='touch-keyboard';keyboard.hidden=true;keyboard.setAttribute('aria-label','Tablet keyboard');
const keys=document.createElement('div');keys.className='touch-keys';keyboard.append(keys);
for(const char of '1234567890qwertyuiopasdfghjklzxcvbnm'){
  const key=document.createElement('button');key.type='button';key.textContent=char;key.onpointerdown=e=>e.preventDefault();
  key.onclick=()=>{if(!target)return;if(target.value.length<(target.maxLength>0?target.maxLength:24)){target.value+=char;target.dispatchEvent(new Event('input',{bubbles:true}));}};keys.append(key);
}
const actions=document.createElement('div');actions.className='touch-key-actions';keyboard.append(actions);
for(const [label,act] of [['Space',()=>{if(target?.id==='name'&&target.value.length<24)target.value+=' ';}],['Delete',()=>{if(target)target.value=target.value.slice(0,-1);}],['Done',()=>{keyboard.hidden=true;document.body.classList.remove('keyboard-open');target?.blur();target=null;}]]){
  const b=document.createElement('button');b.type='button';b.textContent=label;b.onpointerdown=e=>e.preventDefault();b.onclick=act;actions.append(b);
}
document.body.append(keyboard);
document.addEventListener('focusin',event=>{if(event.target.matches('input:not([type=range])')){target=event.target;keyboard.hidden=false;document.body.classList.add('keyboard-open');target.scrollIntoView({block:'start'});}});
const scroll=document.createElement('div');scroll.className='tablet-scroll';
for(const [label,amount] of [['↑',-180],['↓',180]]){const b=document.createElement('button');b.type='button';b.textContent=label;b.setAttribute('aria-label',amount<0?'Scroll up':'Scroll down');b.onclick=()=>document.querySelector('.tablet').scrollBy({top:amount,behavior:'smooth'});scroll.append(b);}
document.body.append(scroll);
