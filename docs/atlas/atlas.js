(()=>{'use strict';
const nav=document.getElementById('site-nav'),toggle=document.querySelector('.nav-toggle'),close=document.querySelector('.nav-close'),backdrop=document.querySelector('.nav-backdrop'),main=document.querySelector('main');
const mobile=matchMedia('(max-width:800px)');
let opened=false,previousFocus=null;
function drawer(open,restore=true){
  opened=open;document.body.classList.toggle('nav-open',open);toggle.setAttribute('aria-expanded',String(open));backdrop.hidden=!open;main.inert=open;nav.inert=mobile.matches&&!open;
  if(open){previousFocus=document.activeElement;close.focus();}else if(restore&&previousFocus)previousFocus.focus();
}
nav.inert=mobile.matches;
toggle.addEventListener('click',()=>drawer(!opened));close.addEventListener('click',()=>drawer(false));backdrop.addEventListener('click',()=>drawer(false));
mobile.addEventListener('change',()=>{drawer(false,false);nav.inert=mobile.matches;});
document.addEventListener('keydown',event=>{
  if(!opened)return;if(event.key==='Escape'){event.preventDefault();drawer(false);return;}if(event.key!=='Tab')return;
  const items=[...nav.querySelectorAll('a,button')].filter(x=>x.getClientRects().length&&!x.disabled),first=items[0],last=items.at(-1);
  if(event.shiftKey&&document.activeElement===first){event.preventDefault();last.focus();}else if(!event.shiftKey&&document.activeElement===last){event.preventDefault();first.focus();}
});
nav.querySelectorAll('a').forEach(a=>a.addEventListener('click',()=>{if(opened)drawer(false);}));
const search=document.getElementById('site-search'),clear=document.getElementById('clear-search');
const rows=[...document.querySelectorAll('[data-search]')];
const groups=[...document.querySelectorAll('[data-search-group]')],status=document.getElementById('search-status'),empty=document.getElementById('search-empty');
let savedDetails=null;
function expose(node){for(let parent=node.parentElement;parent;parent=parent.parentElement)if(parent.tagName==='DETAILS')parent.open=true;}
function filter(){
  const q=search.value.trim().toLocaleLowerCase();if(q&&!savedDetails)savedDetails=new Map([...document.querySelectorAll('details')].map(d=>[d,d.open]));
  let count=0;
  for(const row of rows){const match=!q||((row.dataset.search||'')+' '+row.textContent).toLocaleLowerCase().includes(q);row.hidden=!match;if(match){count++;if(q)expose(row);}}
  for(const group of groups)group.hidden=!!q&&![...group.querySelectorAll('[data-search]')].some(r=>!r.hidden);
  if(!q&&savedDetails){for(const [d,open] of savedDetails)d.open=open;savedDetails=null;}
  empty.hidden=!q||count>0;status.textContent=q?count+' matching records on this page.':'Search the records on this page.';
}
search.addEventListener('input',filter);clear.addEventListener('click',()=>{search.value='';filter();search.focus();});filter();
const technical=document.getElementById('technical-toggle'),technicalDetails=[...document.querySelectorAll('[data-technical]')];
technical.addEventListener('click',()=>{const open=technical.getAttribute('aria-expanded')!=='true';technicalDetails.forEach(d=>{d.open=open;if(savedDetails)savedDetails.set(d,open);});technical.setAttribute('aria-expanded',String(open));technical.textContent=open?'Hide technical detail':'Show technical detail';});
function revealHash(){if(!location.hash)return;let id;try{id=decodeURIComponent(location.hash.slice(1));}catch{return;}const target=document.getElementById(id);if(!target)return;if(search.value){search.value='';filter();}expose(target);requestAnimationFrame(()=>target.scrollIntoView({block:'start',behavior:'instant'}));}
addEventListener('hashchange',revealHash);revealHash();
const currentLinks=[...nav.querySelectorAll('a[href^="#"]')],sections=currentLinks.map(a=>document.getElementById(a.hash.slice(1))).filter(Boolean);
if(sections.length&&'IntersectionObserver' in window){
  const intersections=new Map();
  const observer=new IntersectionObserver(entries=>{
    for(const entry of entries){if(entry.isIntersecting)intersections.set(entry.target,entry);else intersections.delete(entry.target);}
    const visible=[...intersections.values()].sort((a,b)=>a.boundingClientRect.top-b.boundingClientRect.top);
    currentLinks.forEach(a=>{if(visible.length&&a.hash==='#'+visible[0].target.id)a.setAttribute('aria-current','location');else a.removeAttribute('aria-current');});
  },{rootMargin:'-10% 0px -65% 0px',threshold:0});
  sections.forEach(s=>observer.observe(s));
}
const copy=document.getElementById('copy-handoff');
if(copy)copy.addEventListener('click',async()=>{const output=document.getElementById('copy-status');try{await navigator.clipboard.writeText(document.getElementById('handoff-text').textContent);output.textContent='Handoff copied.';}catch{output.textContent='Copy unavailable. Select the handoff text below.';}});
})();
