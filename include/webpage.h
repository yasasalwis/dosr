#pragma once
#include <Arduino.h>

// Single-page control panel. Designed for a phone held in a grow room:
// dark ground, tabular-figure readouts, targets big enough to hit with
// wet hands. The setpoint band under each reading is the one bit of
// invention - it shows where the reading sits inside its deadband.
static const char PAGE_INDEX[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Mini Feeder</title>
<style>
:root{--bg:#12140f;--panel:#1c1f18;--line:#2e3327;--ink:#e8e6de;--dim:#8c9182;
--ok:#8fbf4a;--act:#e0a82e;--bad:#c4553b;--r:10px}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);
font:15px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif;padding:14px 12px 40px;max-width:560px;margin:0 auto}
h1{font-size:15px;font-weight:500;letter-spacing:.14em;text-transform:uppercase;color:var(--dim);margin:0 0 14px}
.card{background:var(--panel);border:1px solid var(--line);border-radius:var(--r);padding:14px;margin-bottom:12px}
.rd{display:flex;align-items:baseline;justify-content:space-between}
.rd b{font:500 40px/1 ui-monospace,SFMono-Regular,Menlo,monospace;font-variant-numeric:tabular-nums}
.rd span{color:var(--dim);font-size:13px;letter-spacing:.1em;text-transform:uppercase}
.band{position:relative;height:6px;background:var(--line);border-radius:3px;margin-top:12px}
.band i{position:absolute;top:0;bottom:0;background:#3d4633;border-radius:3px}
.band u{position:absolute;top:-3px;width:2px;height:12px;background:var(--ok);border-radius:1px}
.band u.out{background:var(--bad)}
.meta{display:flex;justify-content:space-between;color:var(--dim);font-size:12px;margin-top:6px}
.pump{border-top:1px solid var(--line);padding:12px 0}
.pump:first-of-type{border-top:0;padding-top:0}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.row .grow{flex:1;min-width:120px}
label{display:block;color:var(--dim);font-size:12px;margin:10px 0 4px}
input,select{width:100%;background:#0d0f0b;color:var(--ink);border:1px solid var(--line);
border-radius:8px;padding:9px 10px;font:14px ui-monospace,Menlo,monospace}
input:focus,select:focus{outline:2px solid var(--ok);outline-offset:1px}
button{background:transparent;color:var(--ink);border:1px solid var(--line);border-radius:8px;
padding:10px 14px;font:500 14px system-ui;cursor:pointer;min-height:42px}
button:hover{border-color:var(--dim)}
button.go{border-color:var(--ok);color:var(--ok)}
button.stop{border-color:var(--bad);color:var(--bad)}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.tag{font-size:12px;color:var(--dim);font-variant-numeric:tabular-nums}
.st{color:var(--act)}
@media(prefers-reduced-motion:no-preference){.band u{transition:left .4s ease}}
</style></head><body>
<h1>Mini Feeder</h1>

<div class="card">
  <div class="rd"><b id="ph">--</b><span>pH</span></div>
  <div class="band"><i id="phb"></i><u id="phm"></u></div>
  <div class="meta"><span id="phmeta">no reading</span><span id="phv"></span></div>
  <div style="height:18px"></div>
  <div class="rd"><b id="ec">--</b><span>uS/cm</span></div>
  <div class="band"><i id="ecb"></i><u id="ecm"></u></div>
  <div class="meta"><span id="ecmeta">no reading</span><span id="ecv"></span></div>
</div>

<div class="card">
  <div class="row"><div class="grow st" id="status">Loading</div>
  <button id="pause" onclick="togglePause()">Pause</button>
  <button class="stop" onclick="post('/api/stop')">Stop all</button></div>
</div>

<div class="card" id="pumps"></div>

<div class="card">
  <label>pH setpoint / deadband</label>
  <div class="grid"><input id="phsp" type="number" step="0.05"><input id="phdb" type="number" step="0.05"></div>
  <label>EC setpoint / deadband (uS/cm)</label>
  <div class="grid"><input id="ecsp" type="number" step="25"><input id="ecdb" type="number" step="10"></div>
  <label>Mix wait (s) / settle after pump (s)</label>
  <div class="grid"><input id="mix" type="number"><input id="settle" type="number"></div>
  <label>Solution temperature (C) &mdash; fallback when no probe is fitted</label>
  <input id="temp" type="number" step="0.5">
</div>

<div class="card">
  <label>Calibration</label>
  <div class="grid" style="margin-bottom:8px">
    <button onclick="cal('ph',4)">Probe in pH 4.00</button>
    <button onclick="cal('ph',7)">Probe in pH 7.00</button>
  </div>
  <div class="row"><input class="grow" id="ecstd" type="number" value="1413">
  <button onclick="cal('ec',+document.getElementById('ecstd').value)">Set EC standard</button></div>
</div>

<div class="card">
  <label>WiFi network / password</label>
  <div class="grid"><input id="ssid"><input id="pass" type="password" placeholder="unchanged"></div>
  <label>MQTT broker / port</label>
  <div class="grid"><input id="mqh"><input id="mqp" type="number"></div>
  <label>MQTT user / password</label>
  <div class="grid"><input id="mqu"><input id="mqw" type="password" placeholder="unchanged"></div>
</div>

<div class="row"><button class="go grow" onclick="save()">Save settings</button>
<button onclick="post('/api/reboot')">Reboot</button></div>
<p class="tag" id="msg"></p>

<script>
let S=null,C=null;
const $=i=>document.getElementById(i);
const j=async(u,o)=>(await fetch(u,o)).json();

function band(pre,val,sp,db,lo,hi){
  const b=$(pre+'b'),m=$(pre+'m'),meta=$(pre+'meta');
  if(val==null||isNaN(val)){m.style.left='-9px';meta.textContent='no reading';return}
  const w=(2*db)/(hi-lo)*100, l=(sp-db-lo)/(hi-lo)*100;
  b.style.left=Math.max(0,l)+'%';b.style.width=Math.min(100,w)+'%';
  const p=Math.min(100,Math.max(0,(val-lo)/(hi-lo)*100));
  m.style.left='calc('+p+'% - 1px)';
  const out=Math.abs(val-sp)>db;
  m.classList.toggle('out',out);
  meta.textContent=out?('target '+sp+', off by '+Math.abs(val-sp).toFixed(pre=='ph'?2:0)):'in range';
}

async function poll(){
  try{S=await j('/api/state')}catch(e){return}
  $('ph').textContent=S.ph==null?'--':S.ph.toFixed(2);
  $('ec').textContent=S.ec==null?'--':Math.round(S.ec);
  $('phv').textContent=S.phVolt==null?'':S.phVolt.toFixed(3)+' V';
  $('ecv').textContent=(S.ecVolt==null?'':S.ecVolt.toFixed(3)+' V')
    +(S.tempC==null?'':'  '+S.tempC.toFixed(1)+'°C'+(S.tempAuto?'':' set'));
  $('status').textContent=S.status+(S.valid?'':'  (settling)');
  $('pause').textContent=S.paused?'Resume':'Pause';
  band('ph',S.ph,S.phSetpoint,S.phDeadband,4,8);
  band('ec',S.ec,S.ecSetpoint,S.ecDeadband,0,3000);
  if(C)S.pumps.forEach((p,i)=>{const e=$('pr'+i);if(e)e.textContent=
    (S.running==i?'running '+Math.ceil(S.remainMs/1000)+'s':p.queued?'queued':'')});
}

function pumpCard(p,i){return `<div class="pump">
<div class="row"><input class="grow" id="pn${i}" value="${p.name}">
<span class="tag" id="pr${i}"></span></div>
<div class="grid">
<div><label>Mode</label><select id="pm${i}">
${['Off','Manual','Scheduled','Auto'].map((n,k)=>`<option value="${k}"${p.mode==k?' selected':''}>${n}</option>`).join('')}
</select></div>
<div><label>Dose (ml)</label><input id="pd${i}" type="number" step="0.5" value="${p.doseMl}"></div>
<div><label>Every (min)</label><input id="pi${i}" type="number" value="${p.intervalMin}"></div>
<div><label>Corrects</label><select id="pt${i}">
${['pH down','pH up','EC up'].map((n,k)=>`<option value="${k}"${p.target==k?' selected':''}>${n}</option>`).join('')}
</select></div>
<div><label>Lockout (min)</label><input id="pl${i}" type="number" value="${p.lockoutMin}"></div>
<div><label>Max doses/hour</label><input id="px${i}" type="number" value="${p.maxDosesPerHour}"></div>
<div><label>Calibration (ml/s)</label><input id="pc${i}" type="number" step="0.01" value="${p.mlPerSec}"></div>
<div><label>&nbsp;</label><button onclick="post('/api/dose?pump=${i}')">Dose now</button></div>
</div></div>`}

async function load(){
  C=await j('/api/settings');
  $('pumps').innerHTML=C.pumps.map(pumpCard).join('');
  $('phsp').value=C.phSetpoint;$('phdb').value=C.phDeadband;
  $('ecsp').value=C.ecSetpoint;$('ecdb').value=C.ecDeadband;
  $('mix').value=C.mixWaitSec;$('settle').value=C.settleSec;$('temp').value=C.tempC;
  $('ssid').value=C.wifiSsid;$('mqh').value=C.mqttHost;$('mqp').value=C.mqttPort;$('mqu').value=C.mqttUser;
}

async function save(){
  const b={pumps:C.pumps.map((_,i)=>({name:$('pn'+i).value,mode:+$('pm'+i).value,
    doseMl:+$('pd'+i).value,intervalMin:+$('pi'+i).value,target:+$('pt'+i).value,
    lockoutMin:+$('pl'+i).value,maxDosesPerHour:+$('px'+i).value,mlPerSec:+$('pc'+i).value})),
    phSetpoint:+$('phsp').value,phDeadband:+$('phdb').value,
    ecSetpoint:+$('ecsp').value,ecDeadband:+$('ecdb').value,
    mixWaitSec:+$('mix').value,settleSec:+$('settle').value,tempC:+$('temp').value,
    wifiSsid:$('ssid').value,mqttHost:$('mqh').value,mqttPort:+$('mqp').value,mqttUser:$('mqu').value};
  if($('pass').value)b.wifiPass=$('pass').value;
  if($('mqw').value)b.mqttPass=$('mqw').value;
  const r=await j('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});
  say(r.ok?'Saved. WiFi and MQTT changes need a reboot.':'Save failed');
  load();
}

async function post(u){const r=await j(u,{method:'POST'});say(r.msg||(r.ok?'Done':'Rejected'))}
async function cal(k,v){const r=await j('/api/cal/'+k+'?value='+v,{method:'POST'});say(r.msg||'')}
async function togglePause(){await post('/api/pause?on='+(S&&S.paused?0:1))}
function say(m){$('msg').textContent=m;setTimeout(()=>{if($('msg').textContent==m)$('msg').textContent=''},4000)}

load();poll();setInterval(poll,1500);
</script></body></html>)HTML";
