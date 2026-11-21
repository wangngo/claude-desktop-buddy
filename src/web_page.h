#pragma once
// Single-page control UI served by the stick (src/web_ui.h). No CDN/assets:
// works on the SoftAP with no internet. Every POST carries X-Buddy: 1.
static const char WEB_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="vi"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Claude Buddy</title>
<style>
:root{--bg:#f6f5f2;--card:#fff;--fg:#1d1d1b;--dim:#6b6b66;--line:#e3e1db;--acc:#d77757;--ok:#2e8b57;--bad:#c0392b}
@media (prefers-color-scheme:dark){:root{--bg:#161615;--card:#21211f;--fg:#ecebe6;--dim:#9b9a94;--line:#34332f}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.4 system-ui,-apple-system,sans-serif}
main{max-width:560px;margin:0 auto;padding:16px}
h1{font-size:20px;margin:4px 0 12px}h2{font-size:15px;margin:0 0 10px;color:var(--dim);text-transform:uppercase;letter-spacing:.04em}
section{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px;margin-bottom:12px}
.row{display:flex;align-items:center;gap:10px;margin:8px 0;flex-wrap:wrap}.row label{min-width:110px;color:var(--dim)}
input[type=range]{flex:1;min-width:140px}input[type=text],input[type=password],select{flex:1;min-width:140px;padding:8px;border:1px solid var(--line);border-radius:8px;background:var(--bg);color:var(--fg);font:inherit}
button{padding:8px 14px;border-radius:8px;border:1px solid var(--line);background:var(--bg);color:var(--fg);font:inherit;cursor:pointer}
button.p{background:var(--acc);border-color:var(--acc);color:#fff}button.big{font-size:22px;min-width:56px}
.temp{font-size:40px;font-weight:700;min-width:96px;text-align:center}
.pill{padding:2px 8px;border-radius:99px;border:1px solid var(--line);font-size:13px}
.on{color:var(--ok)}.off{color:var(--dim)}.err{color:var(--bad)}
#nets button{display:block;width:100%;text-align:left;margin:4px 0}
canvas{width:100%;height:180px;display:block}
small{color:var(--dim)}#msg{position:fixed;left:50%;bottom:16px;transform:translateX(-50%);background:var(--fg);color:var(--bg);padding:8px 14px;border-radius:8px;display:none}
</style></head><body><main>
<h1>Claude Buddy <small id="host"></small></h1>

<section><h2>Máy lạnh</h2>
<div class="row"><button id="acPw" class="big" onclick="ac({power:!S.ac.power})">⏻</button>
<button class="big" onclick="ac({delta:-1})">−</button><div class="temp" id="acT">--</div>
<button class="big" onclick="ac({delta:1})">+</button></div>
<div class="row"><label>Chế độ</label><select id="acMode" onchange="ac({mode:this.value})"></select></div>
<div class="row"><label>Quạt</label><select id="acFan" onchange="ac({fan:this.value})"></select></div>
<div class="row"><label>Giao thức</label><select id="acProto"></select><button onclick="ac({proto:$('acProto').value})">Test</button></div>
<small>Chưa biết model: chọn từng giao thức → Test; máy kêu "bíp" thì giữ. <span id="acInfo"></span></small>
</section>

<section><h2>Thiết bị</h2>
<div class="row"><label>Độ sáng</label><input type="range" id="bri" min="0" max="4" onchange="set({bright:+this.value})"><span id="briV"></span></div>
<div class="row"><label>Âm lượng</label><input type="range" id="vol" min="0" max="255" onchange="set({vol:+this.value})"></div>
<div class="row"><label>Âm thanh</label><button id="snd" onclick="set({sound:!S.dev.sound})"></button></div>
<div class="row"><label>Tắt màn sau</label><select id="off" onchange="set({offSec:+this.value})">
<option value="0">Không tắt</option><option value="15">15 s</option><option value="30">30 s</option><option value="60">1 phút</option><option value="300">5 phút</option></select></div>
<div class="row"><label>Tắt cả khi cắm USB</label><button id="offu" onclick="set({offUsb:!S.dev.offUsb})"></button></div>
<div class="row"><label>Ngủ sâu khi mất kết nối (pin)</label><select id="slp" onchange="set({sleepMin:+this.value})">
<option value="0">Không</option><option value="1">1 phút</option><option value="2">2 phút</option><option value="5">5 phút</option><option value="10">10 phút</option><option value="30">30 phút</option></select></div>
<div class="row"><small id="devInfo"></small></div>
<div class="row"><button onclick="if(confirm('Khởi động lại stick?'))post('/api/reboot',{})">Khởi động lại</button></div>
</section>

<section><h2>Wi-Fi</h2>
<div class="row"><label>Trạng thái</label><span id="wst" class="pill"></span><button id="wtog"></button></div>
<div class="row"><label>SSID</label><input type="text" id="ssid" autocomplete="off"></div>
<div class="row"><label>Mật khẩu</label><input type="password" id="psk"></div>
<div class="row"><button class="p" onclick="wifiSave()">Lưu &amp; kết nối</button><button onclick="scan()">Quét mạng</button></div>
<div id="nets"></div><small>Stick chỉ bắt được 2.4 GHz.</small>
</section>

<section><h2>Pin &amp; nhiệt độ (2 giờ)</h2>
<canvas id="ch" width="520" height="180"></canvas>
<small><span style="color:#d77757">■</span> nhiệt độ chip °C &nbsp; <span style="color:#3a7bd5">■</span> pin % &nbsp; <span id="pwNow"></span></small>
</section>
<div id="msg"></div>
</main><script>
const $=id=>document.getElementById(id);let S={};
function toast(t){const m=$('msg');m.textContent=t;m.style.display='block';clearTimeout(m._t);m._t=setTimeout(()=>m.style.display='none',2500)}
async function post(u,b){const r=await fetch(u,{method:'POST',headers:{'X-Buddy':'1','Content-Type':'application/json'},body:JSON.stringify(b)});
 const j=await r.json().catch(()=>({}));if(!r.ok||j.ok===false)toast('Lỗi: '+(j.error||r.status));return j}
async function set(b){await post('/api/set',b);load()}
async function ac(b){const j=await post('/api/ac',b);toast(j.ok?'Đã gửi IR':'Gửi IR lỗi');load()}
async function wifiSave(){const s=$('ssid').value.trim();if(!s)return toast('Nhập SSID');
 const j=await post('/api/wifi',{ssid:s,psk:$('psk').value});if(j.ok)toast('Đã lưu, stick đang kết nối… trang này có thể mất kết nối')}
async function scan(){$('nets').textContent='Đang quét…';await post('/api/scan',{});
 for(let i=0;i<20;i++){await new Promise(r=>setTimeout(r,700));const j=await (await fetch('/api/scan')).json();
  if(j.running)continue;$('nets').innerHTML='';(j.nets||[]).forEach(n=>{const b=document.createElement('button');
  b.textContent=n[0]+'  ('+n[1]+' dBm, kênh '+n[2]+')';b.onclick=()=>{$('ssid').value=n[0];$('psk').focus()};$('nets').appendChild(b)});
  if(!(j.nets||[]).length)$('nets').textContent='Không thấy mạng nào';return}$('nets').textContent='Quét quá lâu'}
function opts(sel,list,cur){if(sel.options.length!==list.length){sel.innerHTML='';list.forEach(v=>sel.add(new Option(v,v)))}sel.value=cur}
async function load(){const r=await fetch('/api/state');if(!r.ok)return;S=await r.json();
 const a=S.ac||{};$('acT').textContent=(a.temp??'--')+'°';$('acPw').className='big '+(a.power?'p':'');
 if(a.modes){opts($('acMode'),a.modes,a.mode);opts($('acFan'),a.fans,a.fan);opts($('acProto'),a.protos,a.proto)}
 $('acInfo').textContent=a.sentAgo>=0?('Lần gửi cuối '+a.sentAgo+' s trước'+(a.lastOk?'':' (LỖI)')):'';
 const d=S.dev;$('bri').value=d.bright;$('briV').textContent=(20+d.bright*20)+'%';$('vol').value=d.vol;
 $('snd').textContent=d.sound?'Bật':'Tắt';$('snd').className=d.sound?'on':'off';$('off').value=d.offSec;
 $('offu').textContent=d.offUsb?'Có':'Không';$('slp').value=d.sleepMin;
 $('devInfo').textContent='Uptime '+Math.floor(d.up/3600)+'h'+String(Math.floor(d.up/60)%60).padStart(2,'0')+' · RAM nội '+Math.round(d.heapInt/1024)+' KB · PSRAM '+Math.round(d.psram/1024)+' KB';
 const w=S.wifi;$('host').textContent=w.host;$('wst').textContent=w.state+(w.ip?' · '+w.ip:'')+(w.rssi?' · '+w.rssi+' dBm':'');
 if(document.activeElement!==$('ssid')&&!$('ssid').value)$('ssid').value=w.ssid||'';
 $('wtog').textContent=w.enabled?'Tắt Wi-Fi':'Bật Wi-Fi';$('wtog').onclick=()=>{if(!w.enabled||confirm('Tắt Wi-Fi? Trang này sẽ mất kết nối.'))post('/api/wifi',{enabled:!w.enabled}).then(load)};
 const p=S.pwr;$('pwNow').textContent='· hiện tại '+p.pct+'% · '+(p.mV/1000).toFixed(2)+' V · '+p.tC+' °C'+(p.chg?' · đang sạc':'')}
async function chart(){const r=await fetch('/api/power');if(!r.ok)return;const h=await r.json();const c=$('ch'),g=c.getContext('2d');
 const W=c.width,H=c.height,P=28;g.clearRect(0,0,W,H);const dim=getComputedStyle(document.body).color;g.font='11px system-ui';g.fillStyle=dim;g.globalAlpha=.6;
 if(h.length<2){g.fillText('Chưa đủ dữ liệu (1 điểm / 30 s)',P,H/2);return}
 const t0=h[0][0],t1=h[h.length-1][0]||1,x=t=>P+(t-t0)/Math.max(1,t1-t0)*(W-2*P);
 const temps=h.map(q=>q[4]/10),tl=Math.floor(Math.min(...temps)-2),th=Math.ceil(Math.max(...temps)+2);
 for(let i=0;i<=4;i++){const y=8+i*(H-24)/4;g.fillRect(P,y,W-2*P,1);g.fillText(Math.round(th-(th-tl)*i/4)+'°',2,y+4);g.fillText(100-25*i+'%',W-P+3,y+4)}
 g.globalAlpha=1;const line=(col,f)=>{g.strokeStyle=col;g.lineWidth=2;g.beginPath();h.forEach((q,i)=>{const X=x(q[0]),Y=f(q);i?g.lineTo(X,Y):g.moveTo(X,Y)});g.stroke()};
 line('#d77757',q=>8+(th-q[4]/10)/(th-tl)*(H-24));line('#3a7bd5',q=>8+(100-Math.max(0,q[2]))/100*(H-24));
 g.fillStyle=dim;g.fillText('-'+Math.round((t1-t0)/60)+' phút',P,H-2);g.fillText('bây giờ',W-P-38,H-2)}
load();chart();setInterval(load,5000);setInterval(chart,30000);
</script></body></html>)HTML";
