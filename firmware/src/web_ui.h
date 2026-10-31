// Weboberfläche der Bridge (eine Seite, fragt /api/state ab, sendet an /api/set).
#pragma once

#include <Arduino.h>

static const char WEB_UI[] PROGMEM = R"HTML(<!doctype html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Alde Heizung</title>
<style>
:root{--bg:#f4f5f7;--card:#fff;--fg:#1d2329;--mut:#69727d;--line:#dde1e6;--acc:#d9480f;--ok:#2b8a3e;--bad:#c92a2a}
@media (prefers-color-scheme:dark){:root{--bg:#15181c;--card:#1f2328;--fg:#e8eaed;--mut:#9aa3ad;--line:#343a40;--acc:#ff8a4c;--ok:#51cf66;--bad:#ff6b6b}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.4 system-ui,-apple-system,sans-serif}
main{max-width:480px;margin:0 auto;padding:16px}
header{display:flex;justify-content:space-between;align-items:center;margin-bottom:12px}
h1{font-size:20px;margin:0}
.badge{font-size:13px;padding:3px 10px;border-radius:99px;background:var(--line)}
.badge.ok{background:var(--ok);color:#fff}.badge.bad{background:var(--bad);color:#fff}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px;margin-bottom:12px}
.temps{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.lbl{font-size:13px;color:var(--mut)}
.big{font-size:32px;font-weight:600;font-variant-numeric:tabular-nums}
.row{display:flex;justify-content:space-between;align-items:center;gap:12px;padding:8px 0}
.row+.row{border-top:1px solid var(--line)}
.sp{display:flex;align-items:center;gap:10px}
.sp span{font-size:22px;font-weight:600;min-width:72px;text-align:center;font-variant-numeric:tabular-nums}
button{font:inherit;color:var(--fg);background:var(--bg);border:1px solid var(--line);border-radius:8px;padding:8px 12px;cursor:pointer;min-width:44px}
button.on{background:var(--acc);border-color:var(--acc);color:#fff}
button:disabled{opacity:.4;cursor:default}
.seg{display:flex}.seg button{border-radius:0;margin-left:-1px}
.seg button:first-child{border-radius:8px 0 0 8px;margin:0}.seg button:last-child{border-radius:0 8px 8px 0}
.flags{display:flex;flex-wrap:wrap;gap:6px}
.flag{font-size:13px;padding:2px 8px;border-radius:6px;border:1px solid var(--line);color:var(--mut)}
.flag.on{color:var(--fg);border-color:var(--acc)}.flag.err{color:#fff;background:var(--bad);border-color:var(--bad)}
#msg{min-height:20px;font-size:14px;color:var(--bad)}
#pend{font-size:13px;color:var(--mut)}
</style></head><body><main>
<header><h1>Alde Heizung</h1><span id="conn" class="badge">…</span></header>
<div class="card temps">
 <div><div class="lbl">Innen</div><div class="big" id="z1">–</div></div>
 <div><div class="lbl">Außen</div><div class="big" id="out">–</div></div>
</div>
<div class="card">
 <div class="row"><span>Raumtemperatur</span>
  <div class="sp"><button data-d="-0.5">−</button><span id="sp">–</span><button data-d="0.5">+</button></div></div>
 <div class="row"><span>Gas</span><button id="gas" data-k="gas">Aus</button></div>
 <div class="row"><span>Elektro</span><div class="seg" id="el">
  <button data-k="el" data-v="0">Aus</button><button data-k="el" data-v="1">1 kW</button>
  <button data-k="el" data-v="2">2 kW</button><button data-k="el" data-v="3">3 kW</button></div></div>
 <div class="row"><span>Warmwasser</span><div class="seg" id="ww">
  <button data-k="ww" data-v="0">Aus</button><button data-k="ww" data-v="1">An</button>
  <button data-k="ww" data-v="2">Boost</button></div></div>
 <div id="pend"></div>
</div>
<div class="card"><div class="flags" id="flags"></div></div>
<div id="msg"></div>
<div class="lbl" id="rssi"></div>
</main><script>
const $=id=>document.getElementById(id);
let st=null,spLocal=null,spSent=null,spTimer=null;
const t=v=>v==null?'–':v.toFixed(1)+' °C';
function render(j){
 st=j.state;
 const c=$('conn');
 c.textContent=st?'Verbunden':'Keine Verbindung zum Panel';c.className='badge '+(st?'ok':'bad');
 document.querySelectorAll('button').forEach(b=>b.disabled=!st);
 $('pend').textContent=j.pending?'Wird an das Panel übertragen …':'';
 if(j.error)$('msg').textContent=j.error;
 $('rssi').textContent='WLAN '+j.rssi+' dBm'+(j.rssi<-75?' (schwach)':'');
 if(!st)return;
 $('z1').textContent=t(st.z1);$('out').textContent=t(st.out);
 if(!j.pending)spSent=null;
 // Bis das Panel bestätigt, den gesendeten statt des alten Sollwerts zeigen.
 if(spLocal==null)$('sp').textContent=(spSent??st.sp).toFixed(1)+' °C';
 const g=$('gas');g.textContent=st.gas?'An':'Aus';g.classList.toggle('on',!!st.gas);
 for(const k of['el','ww'])$(k).querySelectorAll('button').forEach(b=>b.classList.toggle('on',+b.dataset.v===st[k]));
 const f=[['Panel',st.on],['Pumpe',st.pump],['Gasventil',st.valve],['230 V',st.ac],['Beschäftigt',st.busy]];
 $('flags').innerHTML=f.map(([n,v])=>`<span class="flag${v?' on':''}">${n}</span>`).join('')+
  (st.err?'<span class="flag err">Fehler</span>':'');
}
async function poll(){
 try{render(await (await fetch('/api/state')).json())}
 catch(e){const c=$('conn');c.textContent='Bridge nicht erreichbar';c.className='badge bad'}
}
async function send(p){
 $('msg').textContent='';
 try{
  const r=await fetch('/api/set',{method:'POST',body:new URLSearchParams(p)});
  const j=await r.json();if(!j.ok)$('msg').textContent=j.error;
 }catch(e){$('msg').textContent='Senden fehlgeschlagen'}
 poll();
}
document.querySelectorAll('[data-d]').forEach(b=>b.onclick=()=>{
 if(!st)return;
 spLocal=Math.min(30,Math.max(5,(spLocal??st.sp)+ +b.dataset.d));
 $('sp').textContent=spLocal.toFixed(1)+' °C';
 clearTimeout(spTimer);
 spTimer=setTimeout(()=>{spSent=spLocal;spLocal=null;send({sp:spSent})},800);
});
$('gas').onclick=()=>st&&send({gas:st.gas?0:1});
document.querySelectorAll('.seg button').forEach(b=>b.onclick=()=>send({[b.dataset.k]:b.dataset.v}));
poll();setInterval(poll,2000);
</script></body></html>
)HTML";
