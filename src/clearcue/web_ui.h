// Single-page settings UI, served from flash at "/". Same page in captive-portal
// (setup AP) and LAN mode. Talks to the /api/* JSON endpoints in main.cpp.
#pragma once
#include <Arduino.h>

static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Button setup</title>
<style>
:root{--bg:#f6f6f4;--card:#fff;--fg:#1d1d1b;--mute:#6b6b66;--line:#e2e1dc;--acc:#2f6f4f;--red:#b23a2e;--grn:#2f7d4f}
@media (prefers-color-scheme:dark){:root{--bg:#161615;--card:#1f1f1d;--fg:#ecebe6;--mute:#9a9992;--line:#33322f;--acc:#5fb98a;--red:#e0685b;--grn:#5fb98a}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,-apple-system,Segoe UI,sans-serif}
main{max-width:560px;margin:0 auto;padding:16px}
h1{font-size:20px;margin:8px 0 2px}.sub{color:var(--mute);font-size:13px;margin-bottom:14px}
section{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px;margin:12px 0}
h2{font-size:14px;text-transform:uppercase;letter-spacing:.04em;color:var(--mute);margin:0 0 10px}
label{display:block;font-size:13px;color:var(--mute);margin:10px 0 4px}
input,select,textarea{width:100%;padding:9px 10px;border:1px solid var(--line);border-radius:7px;background:var(--bg);color:var(--fg);font:inherit}
textarea{font:12px/1.3 ui-monospace,monospace;min-height:90px}
.hint{font-size:12px;color:var(--mute);margin-top:4px}
button{font:inherit;padding:9px 14px;border-radius:7px;border:1px solid var(--line);background:var(--card);color:var(--fg);cursor:pointer}
button.pri{background:var(--acc);border-color:var(--acc);color:#fff}button.bad{color:var(--red)}
.row{display:flex;gap:8px;flex-wrap:wrap;margin-top:12px}
.dot{display:inline-block;width:12px;height:12px;border-radius:50%;vertical-align:-1px;margin-right:6px;background:var(--mute)}
.dot.r{background:var(--red)}.dot.g{background:var(--grn)}
pre{white-space:pre-wrap;word-break:break-all;background:var(--bg);border:1px solid var(--line);border-radius:7px;padding:8px;font-size:12px;margin:8px 0 0}
#msg{position:sticky;top:0;z-index:2;display:none;padding:10px 12px;border-radius:8px;background:var(--acc);color:#fff;margin-bottom:8px}
details summary{cursor:pointer;color:var(--mute);font-size:13px;margin-top:10px}
.big{font-size:18px;font-weight:600;word-break:break-all}.big a{color:var(--acc)}
.step{font-size:12px;font-weight:600;color:var(--acc);text-transform:uppercase;letter-spacing:.05em;margin-bottom:4px}
.err{color:var(--red);font-size:13px;margin-top:8px}
dl{display:grid;grid-template-columns:auto 1fr;gap:4px 12px;margin:0;font-size:13px}dt{color:var(--mute)}dd{margin:0;word-break:break-all}
</style></head><body><main>
<div id="msg"></div>
<h1 id="title">Button setup</h1><div class="sub" id="subtitle"></div>

<section id="setup" hidden>
<div id="wForm">
<div class="step">Step 1 of 2</div>
<h2 style="text-transform:none;letter-spacing:0;color:var(--fg);font-size:17px">Connect the button to your Wi-Fi</h2>
<label for="wNets">Network</label>
<select id="wNets"><option value="">Scanning…</option></select>
<input id="wSsid" placeholder="Network name" autocomplete="off" autocapitalize="off" spellcheck="false" hidden style="margin-top:8px">
<label for="wPass">Password</label>
<input id="wPass" type="password" autocomplete="off">
<div class="hint">2.4 GHz networks only.</div>
<div class="err" id="wErr" hidden></div>
<div class="row"><button class="pri" id="wGo">Connect</button><button id="wRescan">Scan again</button></div>
<div class="hint" style="margin-top:12px">Next you'll finish setting it up from a computer, at <b id="wHost1"></b>.</div>
</div>
<div id="wBusy" hidden>
<div class="step">Step 1 of 2</div>
<p>Connecting to <b id="wName"></b>…</p>
<div class="hint">Your phone may drop off <b class="apn"></b> for a moment while the button joins. If this page stops updating, reconnect your phone to <b class="apn"></b>.</div>
</div>
<div id="wDone" hidden>
<div class="step">Step 2 of 2</div>
<p style="margin-top:0">✓ Connected to <b id="wName2"></b>.</p>
<p>On a computer connected to that network, open:</p>
<div class="big"><a id="wUrl1" target="_blank"></a></div>
<div class="hint" id="wUrl2Row">If that doesn't open, use <b><a id="wUrl2" target="_blank" style="color:var(--acc)"></a></b></div>
<p class="hint">There you'll enter the printer server details. Until then the light blinks blue slowly.</p>
<div class="row"><button class="pri" id="wFinish">Done, turn off setup network</button></div>
</div>
<div id="wLost" hidden>
<p style="margin-top:0">Lost contact with the button.</p>
<p class="hint">If its light is <b>blinking blue slowly</b>, it joined your Wi-Fi: on a computer, open <b id="wHost2"></b>.<br>If it's <b>solid blue</b>, reconnect your phone to <b class="apn"></b> and try again.</p>
<div class="row"><button id="wRetry">Check again</button></div>
</div>
<p class="hint" style="margin-top:14px"><a href="#" id="showAll" style="color:var(--mute)">No computer? Set up everything on this phone instead</a></p>
</section>

<div id="step2" hidden><section style="border-color:var(--acc)"><div class="step">Step 2 of 2</div>Fill in the <b>Printer server</b> section below, then press <b>Save</b>.</section></div>

<div id="full">
<section><h2>Status</h2>
<div id="state"><span class="dot"></span>Loading…</div>
<dl id="info" style="margin-top:10px"></dl>
<div class="row"><button id="btnTest">Test connection</button></div>
<pre id="testOut" hidden></pre>
</section>

<form id="f">
<section><h2>Wi-Fi</h2>
<label for="ssid">Network name</label>
<input id="ssid" autocomplete="off" autocapitalize="off" spellcheck="false" required>
<div class="row" style="margin-top:6px"><button type="button" id="btnScan">Scan for networks</button></div>
<select id="nets" hidden style="margin-top:8px"></select>
<label for="pass">Password</label>
<input id="pass" type="password" autocomplete="new-password" placeholder="">
<div class="hint">2.4 GHz networks only. Leave blank to keep the saved password.</div>
</section>

<section><h2>Printer server</h2>
<label for="backend">Server type</label>
<select id="backend"><option value="watchtower">Watchtower</option><option value="bambuddy">Bambuddy</option></select>
<label for="apiBase">API base URL</label>
<input id="apiBase" inputmode="url" autocomplete="off" required>
<div class="hint" id="baseHint"></div>
<label for="apiKey">API key</label>
<input id="apiKey" type="password" autocomplete="off">
<div class="hint" id="keyHint"></div>
<label for="printer" id="printerLbl">Printer</label>
<input id="printer" autocomplete="off" required>
<div class="hint" id="printerHint"></div>
<details id="tlsBox"><summary>HTTPS certificate check</summary>
<div class="hint" id="caState"></div>
<div class="row"><button type="button" id="btnTrust">Trust this server</button><button type="button" id="btnUntrust" class="bad">Stop checking</button></div>
<div id="trustOut" hidden style="margin-top:10px"></div>
<label for="caCert">Trusted CA certificate (PEM). Empty = encrypt without checking who the server is.</label>
<textarea id="caCert" spellcheck="false" placeholder="-----BEGIN CERTIFICATE-----"></textarea>
</details>
</section>

<section><h2>Device</h2>
<label for="hostname">Device name (network hostname)</label>
<input id="hostname" pattern="[a-z0-9-]{1,32}" autocomplete="off">
<div class="hint">Lowercase letters, digits, hyphens. Reach this page at http://<b id="hn"></b>.local</div>
<label for="pollSec">Check every (seconds)</label>
<input id="pollSec" type="number" min="5" max="300">
<label for="adminPass">Settings password</label>
<input id="adminPass" type="password" autocomplete="new-password">
<div class="hint" id="adminHint">Protects this page (user name <b>admin</b>). Forgot it? Hold the button 10 s to factory reset.</div>
<label style="display:flex;gap:8px;align-items:center;color:var(--fg);margin-top:14px" id="dbgRow"><input type="checkbox" id="debug" style="width:auto"> Serial debug logging</label>
<div class="hint">Detailed log on the USB serial port (115200 baud): checks, server replies, button presses. Leave off unless troubleshooting. Takes effect when you press Save.</div>
<div class="row"><button class="pri" type="submit">Save</button></div>
</section>
</form>

<section><h2>Firmware</h2>
<dl><dt>Installed</dt><dd id="ver"></dd><dt>Latest</dt><dd id="latest">–</dd></dl>
<div class="row"><button id="btnCheck">Check for update</button><button id="btnInstall" class="pri" hidden>Install update</button></div>
<label for="bin">Or upload a firmware .bin</label>
<input id="bin" type="file" accept=".bin">
<div class="row"><button id="btnUpload">Upload</button></div>
<div class="hint" style="margin-top:12px">Free software under the GNU GPL v3, with wolfSSL. <span id="src"></span></div>
</section>

<section><h2>Device actions</h2>
<div class="row"><button id="btnReboot">Restart</button><button id="btnReset" class="bad">Factory reset</button></div>
</section>
</div>
<footer id="foot" class="hint" style="text-align:center;margin:20px 0 8px"></footer>
</main>
<script>
const $=id=>document.getElementById(id);
const B={bambuddy:{base:"https://bambuddy.example.com/api/v1",baseHint:"Ends in /api/v1.",key:"Needs read access plus the clear-plate permission.",lbl:"Printer ID",ph:"1",ph2:"The number Bambuddy shows for this printer."},
watchtower:{base:"http://192.168.1.50/api/public/v1",baseHint:"Ends in /api/public/v1. https:// is supported; use Trust this server below so the button checks it's really your Watchtower.",key:"Starts with wt_. Needs the control scope and set_ready permission.",lbl:"Printer name",ph:"MyPrinter",ph2:"Exactly as shown in Watchtower (case-sensitive)."}};
let cfg={};
function msg(t,ms){const m=$("msg");m.textContent=t;m.style.display="block";if(ms)setTimeout(()=>m.style.display="none",ms)}
async function api(p,o){const r=await fetch(p,o);if(!r.ok&&r.status!==200)throw new Error(p+" → HTTP "+r.status);return r.json()}
function backendUi(){const b=B[$("backend").value];$("apiBase").placeholder=b.base;$("baseHint").textContent=b.baseHint;$("keyHint").textContent=(cfg.hasKey?"A key is saved; leave blank to keep it (changing the URL or server type clears it). ":"")+b.key;$("printerLbl").textContent=b.lbl;$("printer").placeholder=b.ph;$("printerHint").textContent=b.ph2}
function dot(s){if(!s.configured)return'<span class="dot"></span>Not set up yet';if(!s.wifi)return'<span class="dot"></span>Not connected to Wi-Fi';if(!s.poll||!s.poll.ok)return'<span class="dot r"></span>Can\'t reach the printer server';return s.poll.red?'<span class="dot r"></span>Red: printer busy or plate needs clearing':'<span class="dot g"></span>Green: ready'}
async function refresh(){try{const s=await api("/api/info");$("step2").hidden=!(s.wifi&&!s.configured&&!setupMode);$("title").textContent=s.product+" setup";$("subtitle").textContent=s.ap?("Setup network: "+s.apSsid):("http://"+s.host+".local · "+s.ip);$("state").innerHTML=dot(s);
$("info").innerHTML=[["Wi-Fi",s.wifi?(s.ssid+" ("+s.rssi+" dBm)"):"—"],["IP",s.ip||"—"],["MAC",s.mac],["Last check",s.poll&&s.poll.age>=0?(s.poll.age+" s ago · "+(s.poll.summary||s.poll.hint)):"—"]].map(r=>"<dt>"+r[0]+"</dt><dd>"+esc(r[1])+"</dd>").join("");$("ver").textContent=s.version;$("foot").textContent=s.product+" "+s.version+" · "+s.host+" · "+s.mac;$("src").innerHTML=s.source?'Source code and licenses: <a href="'+esc(s.source)+'" style="color:var(--acc)">'+esc(s.source)+"</a>":""}catch(e){}}
function esc(x){return String(x??"").replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]))}
async function load(){cfg=await api("/api/config");for(const k of["ssid","backend","apiBase","printer","caCert","hostname","pollSec"])$(k).value=cfg[k]??"";$("hn").textContent=cfg.hostname;$("pass").placeholder=cfg.hasPass?"•••••• (saved)":"";$("apiKey").placeholder=cfg.hasKey?"•••••• (saved)":"";$("debug").checked=!!cfg.debug;$("dbgRow").style.display=cfg.debugAvailable===false?"none":"flex";$("adminPass").placeholder=cfg.hasAdmin?"•••••• (set, type to change)":"Optional";backendUi();caState();if(cfg.caCert)$("tlsBox").open=true;refresh()}
$("backend").onchange=backendUi;$("hostname").oninput=e=>$("hn").textContent=e.target.value;
function bars(r){return r>=-60?"▂▄▆█":r>=-70?"▂▄▆":r>=-80?"▂▄":"▂"}
$("btnScan").onclick=async()=>{const b=$("btnScan");b.disabled=true;b.textContent="Scanning…";
try{const n=(await api("/api/scan")).sort((a,c)=>c.rssi-a.rssi),sel=$("nets");
if(!n.length){sel.hidden=true;msg("No 2.4 GHz networks found. You can still type the name.",4000);return}
const cur=$("ssid").value;
sel.innerHTML='<option value="">Choose a network ('+n.length+' found)…</option>'+n.map(x=>'<option value="'+esc(x.ssid)+'"'+(x.ssid===cur?" selected":"")+'>'+esc(x.ssid)+"  "+bars(x.rssi)+(x.secure?"  🔒":"")+"</option>").join("");
sel.hidden=false;sel.focus()}
catch(e){msg("Scan failed: "+e.message,4000)}finally{b.disabled=false;b.textContent="Scan again"}};
$("nets").onchange=e=>{if(!e.target.value)return;$("ssid").value=e.target.value;const p=$("pass");p.value="";p.focus()};
$("f").onsubmit=async e=>{e.preventDefault();const b={};for(const k of["ssid","pass","backend","apiBase","apiKey","printer","caCert","hostname","adminPass"])b[k]=$(k).value.trim();b.pollSec=+$("pollSec").value||10;b.debug=$("debug").checked;
try{const r=await api("/api/config",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(b)});if(!r.ok){msg(r.error||"Save failed");return}
if(r.reboot){msg("Saved. Restarting and joining "+b.ssid+". Reconnect your phone/PC to that network, then open http://"+b.hostname+".local")}else{msg("Saved.",2500);$("pass").value="";$("apiKey").value="";$("adminPass").value="";load();setTimeout(()=>$("btnTest").click(),300)}}catch(e){msg(e.message)}};
$("btnTest").onclick=async()=>{const o=$("testOut");o.hidden=false;o.textContent="Testing…";try{const r=await api("/api/test",{method:"POST"});o.textContent=(r.ok?"✓ ":"✗ ")+r.hint+"\n\nURL:  "+r.url+"\nHTTP: "+r.http+(r.transport?"\nError: "+r.transport:"")+(r.tls?"\nTLS: "+r.tls:"")+(r.summary?"\nRead: "+r.summary:"")+(r.body?"\nBody: "+r.body:"");refresh()}catch(e){o.textContent=e.message}};
$("btnCheck").onclick=async()=>{msg("Checking…");try{const r=await api("/api/ota",{method:"POST",headers:{"Content-Type":"application/json"},body:"{}"});if(!r.ok){msg(r.error,4000);return}$("latest").textContent=r.latest;$("btnInstall").hidden=!r.newer;msg(r.newer?"Update available.":"Up to date.",3000)}catch(e){msg(e.message,4000)}};
$("btnInstall").onclick=async()=>{if(!confirm("Install the update? The button restarts when done."))return;await api("/api/ota",{method:"POST",headers:{"Content-Type":"application/json"},body:'{"install":true}'});msg("Updating… the button will restart in about a minute.")};
$("btnUpload").onclick=async()=>{const f=$("bin").files[0];if(!f){msg("Choose a .bin first",3000);return}if(/factory/i.test(f.name)){msg("That's the full factory image. Upload the -app.bin file instead.",5000);return}const fd=new FormData();fd.append("firmware",f,f.name);msg("Uploading…");try{const r=await fetch("/update",{method:"POST",body:fd});const t=await r.text();msg(r.ok?"Update installed. Restarting…":("Upload failed: "+t))}catch(e){msg("Upload failed: "+e.message)}};
$("btnReboot").onclick=async()=>{await api("/api/reboot",{method:"POST"});msg("Restarting…")};
$("btnReset").onclick=async()=>{if(!confirm("Erase all settings and return to setup mode?"))return;await api("/api/reset",{method:"POST"});msg("Reset. Join the setup Wi-Fi network to configure again.")};
function caState(){const v=$("caCert").value.trim();$("caState").textContent=v?"Checking the server's certificate against the trusted CA below.":"Not checking the server's certificate (traffic is still encrypted).";$("btnUntrust").hidden=!v}
$("caCert").oninput=caState;
$("btnTrust").onclick=async()=>{const o=$("trustOut");o.hidden=false;o.innerHTML="Connecting…";
try{const r=await api("/api/trust",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({apiBase:$("apiBase").value.trim()})});
if(!r.ok){o.innerHTML='<div class="hint" style="color:var(--red)">'+esc(r.error)+'</div>';return}
o.innerHTML='<dl><dt>Certificate</dt><dd>'+esc(r.subject)+'</dd><dt>Issued by</dt><dd>'+esc(r.issuer)+'</dd><dt>SHA-256</dt><dd style="font:12px ui-monospace,monospace">'+esc(r.sha256)+'</dd></dl>'+
(r.selfSigned?'':'<div class="hint" style="color:var(--red)">This is an intermediate CA, not a root. It may be replaced when the server renews its certificate, which would break the check.</div>')+
'<div class="hint">Compare the fingerprint with the CA certificate on your server before trusting it.</div><div class="row"><button type="button" class="pri" id="btnTrustYes">Trust and save</button><button type="button" id="btnTrustNo">Cancel</button></div>';
$("btnTrustYes").onclick=()=>{$("caCert").value=r.pem;caState();o.hidden=true;$("f").requestSubmit()};
$("btnTrustNo").onclick=()=>{o.hidden=true}}catch(e){o.textContent=e.message}};
$("btnUntrust").onclick=()=>{if(!confirm("Stop checking the server's certificate?"))return;$("caCert").value="";caState();$("f").requestSubmit()};
// ---- phone setup: Wi-Fi only, then finish on a PC ----
let setupMode=false,wInfo={};
const show=id=>{for(const k of["wForm","wBusy","wDone","wLost"])$(k).hidden=k!==id};
function wHost(){return "http://"+(wInfo.host||"")+".local"}
function wFill(){$("wHost1").textContent=wHost();$("wHost2").textContent=wHost();document.querySelectorAll(".apn").forEach(e=>e.textContent=wInfo.apSsid||"the setup network")}
async function wScan(){const sel=$("wNets"),b=$("wRescan");b.disabled=true;sel.innerHTML='<option value="">Scanning…</option>';
try{const n=(await api("/api/scan")).sort((a,c)=>c.rssi-a.rssi);
sel.innerHTML='<option value="">'+(n.length?"Choose your network":"No networks found")+'</option>'+n.map(x=>'<option value="'+esc(x.ssid)+'">'+esc(x.ssid)+"  "+bars(x.rssi)+(x.secure?"  🔒":"")+"</option>").join("")+'<option value="__other">Other (type the name)…</option>';
if(cfg.ssid&&n.some(x=>x.ssid===cfg.ssid))sel.value=cfg.ssid}
catch(e){sel.innerHTML='<option value="">Scan failed</option><option value="__other">Other (type the name)…</option>'}
finally{b.disabled=false;wOther()}}
function wOther(){const o=$("wNets").value==="__other";$("wSsid").hidden=!o;if(o)$("wSsid").focus()}
$("wNets").onchange=()=>{wOther();if($("wNets").value&&$("wNets").value!=="__other")$("wPass").focus()};
$("wRescan").onclick=wScan;
$("wGo").onclick=async()=>{const v=$("wNets").value,ssid=(v==="__other"?$("wSsid").value:v).trim(),e=$("wErr");e.hidden=true;
if(!ssid){e.textContent="Choose or type your network name.";e.hidden=false;return}
try{const r=await api("/api/wifi",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({ssid,pass:$("wPass").value})});
if(!r.ok){e.textContent=r.error;e.hidden=false;return}
$("wName").textContent=ssid;show("wBusy");wPoll()}catch(x){e.textContent=x.message;e.hidden=false}};
async function wPoll(){const t0=Date.now();
while(Date.now()-t0<60000){await new Promise(r=>setTimeout(r,1000));
try{const r=await api("/api/wifi");wInfo=Object.assign(wInfo,r);wFill();
if(r.state==="ok"||(r.state==="idle"&&r.wifi))return wDone(r);
if(r.state==="fail"){$("wErr").textContent=r.error;$("wErr").hidden=false;$("wPass").value="";show("wForm");return}}catch(e){}}
show("wLost")}
function wDone(r){$("wName2").textContent=r.ssid;const u1=wHost(),u2="http://"+r.ip;
$("wUrl1").textContent=u1;$("wUrl1").href=u1;$("wUrl2").textContent=u2;$("wUrl2").href=u2;$("wUrl2Row").hidden=!r.ip;show("wDone")}
$("wRetry").onclick=()=>{show("wBusy");wPoll()};
$("wFinish").onclick=async()=>{try{const r=await api("/api/ap/close",{method:"POST"});if(!r.ok){msg(r.error,4000);return}
$("setup").innerHTML='<p style="margin:0">Setup network turned off. Reconnect your phone to your usual Wi-Fi.</p><p class="hint">Finish setup on your computer at <b>'+esc(wHost())+'</b>.</p>'}catch(e){msg(e.message,4000)}};
$("showAll").onclick=e=>{e.preventDefault();$("full").hidden=false;$("setup").hidden=true;setupMode=false;$("full").scrollIntoView()};
async function boot(){let s=null;try{s=await api("/api/info")}catch(e){}
try{await load()}catch(e){}
if(s&&s.ap&&!s.wifi){setupMode=true;wInfo={host:s.host,apSsid:s.apSsid};wFill();$("setup").hidden=false;$("full").hidden=true;
let w=null;try{w=await api("/api/wifi")}catch(e){}
if(w&&w.state==="connecting"){$("wName").textContent=w.ssid;show("wBusy");wPoll()}else{show("wForm");wScan()}}
else if(s&&s.ap&&s.wifi){let w=null;try{w=await api("/api/wifi")}catch(e){}   // joined already; phone came back to read the result
if(w&&w.state==="ok"){setupMode=true;wInfo=Object.assign({},w);wFill();$("setup").hidden=false;$("full").hidden=true;wDone(w)}}
refresh();setInterval(refresh,10000)}
boot();
</script></body></html>)HTML";
