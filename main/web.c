#include "web.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "diag.h"
#include "display.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_hosted_ota.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "eth.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "logbuf.h"
#include "net.h"
#include "pintest.h"
#include "profiles.h"
#include "rescue.h"
#include "sdkconfig.h"
#include "wifi.h"

static const char *TAG = "web";

static httpd_handle_t s_server;
static volatile bool s_ota_busy;
static volatile bool s_c5_ota_busy;
static char s_c5_ota_status[64] = "idle";

extern const char _binary_web_ui_css_start[];
extern const char _binary_web_ui_css_end[];
extern const char _binary_web_ui_js_start[];
extern const char _binary_web_ui_js_end[];

// Approved MyPi logo (white, transparent PNG data URI from tools/render_header_logo.py).
#include "header_logo.h"

// Shared header: logo + "Hotspot" + right-aligned STA Wi-Fi/dBm (filled by /ui.js from /api/status),
// the 8N1 "Hotspot" divider, then page buttons. Dashboard pages are hash routes of "/", so the
// same buttons work from /update, /c5update and /pins too.
#define MYPI_BRAND_HEADER \
    "<header class='mypi-brand'><div class='mypi-head'>" \
    "<img class='mypi-logo' src='" HEADER_LOGO_URI "' alt='MyPi' width='40' height='32'>" \
    "<span class='mypi-project'>Hotspot</span>" \
    "<span class='mypi-wifi' id='mypi-wifi' title='Wi-Fi uplink (STA) signal'>" \
    "<svg viewBox='0 0 24 18' aria-hidden='true'><path d='M2 7a14 14 0 0 1 20 0M5.5 10.5a9 9 0 0 1 13 0M9 14a4 4 0 0 1 6 0'/>" \
    "<circle cx='12' cy='16.5' r='1.5'/></svg><span>Wi-Fi</span> <b id='mypi-dbm'>&mdash;</b></span></div>" \
    "<div class='mypi-divider'><svg data-text='Hotspot'></svg></div>" \
    "<nav class='mypi-nav' aria-label='MyPi pages'><a href='/#overview'>Overview</a><a href='/#wifi'>Wi-Fi</a>" \
    "<a href='/#access-point'>Access point</a><a href='/#traffic'>Traffic</a><a href='/#ethernet'>Ethernet</a>" \
    "<a href='/#system'>System</a><a href='/#debug'>Debug</a></nav></header>"

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------

static const char INDEX_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>MyPi-Hotspot</title><style>"
"body{font-family:system-ui,sans-serif;margin:1em;background:#111;color:#ddd}"
"h1{font-size:1.3em}h2{font-size:1.1em;margin-top:1.4em;border-top:1px solid #333;padding-top:.6em}"
"a{color:#6cf}table{border-collapse:collapse}"
"td,th{padding:2px 10px 2px 0;vertical-align:top;text-align:left}td:first-child{color:#999}th{color:#999;font-weight:normal}"
"pre{background:#000;color:#9f9;padding:8px;height:45vh;overflow:auto;font-size:12px;white-space:pre-wrap}"
"input,select,button{background:#222;color:#ddd;border:1px solid #555;margin:2px}input{width:9em}"
"fieldset{border:1px solid #444;margin:.5em 0}#msg{color:#fc6}"
".bad{color:#f66}.ok{color:#6f6}</style><link rel='stylesheet' href='/ui.css'></head><body>"
MYPI_BRAND_HEADER
"<p id='msg'></p>"
"<section class='page' data-page='overview'><h2>Overview</h2><table id='t'></table></section>"
"<section class='page' data-page='wifi'><h2>Wi-Fi uplink (STA profiles)</h2><table id='sp'></table>"
"<p><button onclick='conn(-1)'>Connect (auto, best RSSI)</button> <button onclick='disc()'>Disconnect</button> "
"<button onclick='scan()'>Scan</button></p><table id='sc'></table>"
"<fieldset><legend>Edit STA profile</legend>Slot <select id='ss'></select> SSID <input id='sn'> "
"Password <input id='spw' type='password' placeholder='empty = keep'> <label><input type='checkbox' id='sclr' style='width:auto'>open (no password)</label><br>"
"<label><input type='checkbox' id='sd' style='width:auto' checked>DHCP</label> IP <input id='sip'> Mask <input id='smk' value='255.255.255.0'> "
"Gateway <input id='sgw'> DNS <input id='sdn'> <button onclick='saveSta()'>Save</button></fieldset></section>"
"<section class='page' data-page='access-point'><h2>Access point (AP profiles)</h2><p id='aps'></p>"
"<p><button onclick='apStart(-1)'>Start default AP</button> <button onclick='apStop()'>Stop AP</button></p>"
"<table id='ap'></table><h3 style='font-size:1em'>Connected clients</h3><table id='cl'></table>"
"<fieldset><legend>Edit AP profile</legend>Slot <select id='as'></select> SSID <input id='an'> "
"Password <input id='apw' type='password' placeholder='empty = keep'> <label><input type='checkbox' id='aclr' style='width:auto'>open (no password)</label><br>"
"Subnet <select id='asub'></select> <label><input type='checkbox' id='anat' style='width:auto' checked>NAT (share uplink)</label> "
"<label><input type='checkbox' id='aboot' style='width:auto'>start at boot</label> <button onclick='saveAp()'>Save</button></fieldset></section>"
"<section class='page' data-page='traffic'><h2>Network traffic</h2><table id='ns'></table>"
"<p><button onclick='resetPeaks()'>Reset peaks</button> <button id='lb' onclick='logToggle()'>Start log</button> "
"<button onclick='dlLog()'>Download CSV</button> <span id='lst'></span></p>"
"<h2>Throughput test (iperf server)</h2><p>Window <input id='iw' value='300' style='width:5em'> s "
"<button onclick='ipStart()'>Arm</button> <button onclick='ipStop()'>Stop</button> <span id='is'></span></p>"
"<p><small>From a PC: <code>iperf -c &lt;device IP&gt; -t 10 -i 1</code> (iperf2, TCP port 5001). "
"NAT path: put a phone/PC on the AP and run an internet speed test.</small></p></section>"
"<section class='page' data-page='ethernet'><h2>Ethernet</h2><p id='es'></p><p>Mode <select id='em'><option value='lan'>LAN (bridged to the AP: same subnet as Wi-Fi clients)</option>"
"<option value='wan'>WAN (DHCP client, uplink)</option></select> <button onclick='saveEth()'>Save</button> "
"<button onclick='reboot()'>Reboot</button> <small>(mode change takes effect after a reboot)</small></p></section>"
"<section class='page' data-page='system'><h2>Radio (band / width / protocols / power)</h2><table id='rd'></table>"
"<p>STA width <select id='rsb'><option value='0'>auto</option><option value='1'>20</option><option value='2'>40</option></select> AP width <select id='rab'><option value='0'>auto</option><option value='1'>20</option><option value='2'>40</option></select><br>"
"STA protocols <label><input type='checkbox' id='sta-proto-b' style='width:auto' onchange=protoHint()>b</label> <label><input type='checkbox' id='sta-proto-g' style='width:auto' onchange=protoHint()>g</label> <label><input type='checkbox' id='sta-proto-n' style='width:auto' onchange=protoHint()>n</label> <label><input type='checkbox' id='sta-proto-a' style='width:auto' onchange=protoHint()>a</label> <label><input type='checkbox' id='sta-proto-ac' style='width:auto' onchange=protoHint()>ac</label> <label><input type='checkbox' id='sta-proto-ax' style='width:auto' disabled title='disabled until a fixed C5 radio firmware'>ax (off)</label> <span id='sta-proto-def'></span><br>"
"AP protocols <label><input type='checkbox' id='ap-proto-b' style='width:auto' onchange=protoHint()>b</label> <label><input type='checkbox' id='ap-proto-g' style='width:auto' onchange=protoHint()>g</label> <label><input type='checkbox' id='ap-proto-n' style='width:auto' onchange=protoHint()>n</label> <label><input type='checkbox' id='ap-proto-a' style='width:auto' onchange=protoHint()>a</label> <label><input type='checkbox' id='ap-proto-ac' style='width:auto' onchange=protoHint()>ac</label> <label><input type='checkbox' id='ap-proto-ax' style='width:auto' disabled title='disabled until a fixed C5 radio firmware'>ax (off)</label> <span id='ap-proto-def'></span><br>"
"Band <select id='band-mode'><option value='0'>Auto (2.4+5 GHz)</option><option value='1'>2.4 GHz only</option><option value='2'>5 GHz only</option></select> "
"Max TX power (dBm, 0=default) <input type=number id='tx-power' min=0 max=21 style='width:5em'><br>"
"AP beacon interval (&times;100 TU, 0=default=100) <input type=number id='ap-beacon' min=0 max=10 style='width:5em'> "
"AP DTIM period (0=default=1) <input type=number id='ap-dtim' min=0 max=255 style='width:5em'><br>"
"<button onclick='saveRadio()'>Save</button> <button onclick='reboot()'>Reboot</button><br>"
"<small>Width in MHz (auto = driver default / negotiated). Max 40 MHz (ESP32-C5). Protocols: none checked = default (b/g/n on 2.4 GHz, a/n/ac on 5 GHz); 11ax is disabled until a fixed C5 radio firmware is available (TX hang with ax + TCP upload, ESP-IDF #19238). TX power 2-21 dBm. The driver caps DTIM at 10. STA and AP share one radio and one channel. Changes take effect after a reboot.</small></p>"
"<h2>Display</h2><p><label><input type='checkbox' id='lrot' style='width:auto' onchange=saveRot()>Rotate screen &amp; touch 180&deg;</label> "
"<small>(applied immediately, saved across reboots)</small></p>"
"<h2>Firmware</h2><p class='links'><a href='/update'>P4 firmware update (OTA)</a> <a href='/c5update'>C5 radio firmware update</a> "
"<button onclick='reboot()'>Reboot</button></p></section>"
"<section class='page' data-page='debug'><h2>Diagnostics</h2><p class='links'><a href='/pins'>Pin test &amp; wiring check</a> "
"<a href='/api/status'>Status API</a> <a href='/api/log'>Device log (raw)</a></p>"
"<h2>Last touch events</h2><pre id='touch' style='height:auto'></pre>"
"<h2>Log <label style='font-size:.8em'><input type='checkbox' id='follow' style='width:auto' checked> follow</label></h2>"
"<pre id='log'></pre></section><script>"
"function $(i){return document.getElementById(i)}"
"function esc(s){return String(s).replace(/[&<>\"']/g,c=>'&#'+c.charCodeAt(0)+';')}"
"function band(c){return c>14?'5 GHz':'2.4 GHz'}"
"function chText(a){return 'ch '+a.ap_channel+' ('+band(a.ap_channel)+')'+(a.sta_channel?', shared with STA':'')}"
"function lanNet(s){return s.ap&&s.ap.running?s.ap.ip.replace(/\\.1$/,'.0/24'):'192.168.4.0/24'}"
"function fmtUp(s){let h=Math.floor(s/3600),m=Math.floor(s/60)%60;return h+'h '+m+'m '+(s%60)+'s'}"
"function row(k,v,c){return '<tr><td>'+k+'</td><td'+(c?' class='+c:'')+'>'+v+'</td></tr>'}"
"function tr(a,h){return '<tr>'+a.map(x=>(h?'<th>':'<td>')+x+(h?'</th>':'</td>')).join('')+'</tr>'}"
"function mb(v){return (+v).toFixed(2)}"
"async function post(u,b){try{let r=await fetch(u,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b||{})});"
"let t=await r.text(),j;try{j=JSON.parse(t)}catch(e){j={ok:r.ok,msg:t}}$('msg').textContent=u+': '+(j.ok?'OK':'ERROR')+(j.msg?' - '+j.msg:'')+(j.error?' - '+j.error:'');return j.ok}"
"catch(e){$('msg').textContent=u+': '+e;return false}}"
"let SP=[],AP=[];"
"async function ld(){try{"
"let p=await (await fetch('/api/profiles')).json();SP=p.profiles;"
"$('sp').innerHTML=tr(['Slot','SSID','IP','Pass',''],1)+SP.filter(x=>x.valid).map(x=>tr([x.slot+(x.slot==p.active?' *':''),esc(x.ssid),"
"x.dhcp?'DHCP':esc(x.ip),x.has_pass?'yes':'open',"
"'<button onclick=edS('+x.slot+')>Edit</button><button onclick=conn('+x.slot+')>Connect</button><button onclick=delS('+x.slot+')>Delete</button>'])).join('');"
"let a=await (await fetch('/api/approfiles')).json();AP=a.profiles;"
"if(!$('asub').options.length)$('asub').innerHTML=a.subnets.map((s,i)=>'<option value='+i+'>'+s+'</option>').join('');"
"$('ap').innerHTML=tr(['Slot','SSID','Subnet','Pass','NAT','Boot',''],1)+AP.filter(x=>x.valid).map(x=>tr([x.slot,esc(x.ssid),a.subnets[x.subnet],"
"x.has_pass?'WPA2':'open',x.nat?'yes':'no',x.on_boot?'yes':'',"
"'<button onclick=edA('+x.slot+')>Edit</button><button onclick=apStart('+x.slot+')>Start</button><button onclick=delA('+x.slot+')>Delete</button>'])).join('');"
"let e=await (await fetch('/api/eth')).json();$('em').value=e.saved_mode;"
"let r=await (await fetch('/api/radio')).json();$('rsb').value=r.sta_bw;$('rab').value=r.ap_bw;setProtoMask('sta',r.sta_proto);setProtoMask('ap',r.ap_proto);"
"let st=await (await fetch('/api/status')).json();$('lrot').checked=!!(st.display&&st.display.rotate);"
"$('band-mode').value=r.band_mode;$('tx-power').value=r.tx_power_dbm;$('ap-beacon').value=r.ap_beacon_tu;$('ap-dtim').value=r.ap_dtim;protoHint();"
"}catch(e){$('msg').textContent='load failed: '+e}}"
"function edS(i){let x=SP[i];$('ss').value=i;$('sn').value=x.ssid;$('spw').value='';$('sclr').checked=!x.has_pass;$('sd').checked=x.dhcp;"
"$('sip').value=x.ip;$('smk').value=x.mask||'255.255.255.0';$('sgw').value=x.gw;$('sdn').value=x.dns}"
"function edA(i){let x=AP[i];$('as').value=i;$('an').value=x.ssid;$('apw').value='';$('aclr').checked=!x.has_pass;$('asub').value=x.subnet;"
"$('anat').checked=x.nat;$('aboot').checked=x.on_boot}"
"async function saveSta(){await post('/api/profile/save',{slot:+$('ss').value,ssid:$('sn').value,pass:$('spw').value,clear_pass:$('sclr').checked,"
"dhcp:$('sd').checked,ip:$('sip').value,mask:$('smk').value,gw:$('sgw').value,dns:$('sdn').value});ld()}"
"async function saveAp(){await post('/api/approfile/save',{slot:+$('as').value,ssid:$('an').value,pass:$('apw').value,clear_pass:$('aclr').checked,"
"subnet:+$('asub').value,nat:$('anat').checked,on_boot:$('aboot').checked});ld()}"
"async function delS(i){if(confirm('Delete STA profile '+i+'?')){await post('/api/profile/del',{slot:i});ld()}}"
"async function delA(i){if(confirm('Delete AP profile '+i+'?')){await post('/api/approfile/del',{slot:i});ld()}}"
"async function conn(i){if(!confirm('Connecting may interrupt the current uplink. Continue?'))return;await post('/api/connect',{slot:i});ld()}"
"async function disc(){if(!confirm('Disconnect the Wi-Fi uplink? AP clients may lose internet access.'))return;await post('/api/disconnect');ld()}"
"async function apStart(i){if(confirm('Starting this profile may replace the running AP and disconnect clients. Continue?'))await post('/api/ap/start',{slot:i})}"
"async function apStop(){if(confirm('Stop the access point and disconnect its clients?'))await post('/api/ap/stop')}"
"const PBITS={b:1,g:2,n:4,a:16,ac:32};"
"function buildProtoMask(p){let m=0;for(let k in PBITS)if($(p+'-proto-'+k).checked)m|=PBITS[k];return m}"
"function setProtoMask(p,m){for(let k in PBITS)$(p+'-proto-'+k).checked=!!(m&PBITS[k])}"
"function protoHint(){for(let p of ['sta','ap'])$(p+'-proto-def').textContent=buildProtoMask(p)?'':'(default, no ax)'}"
"async function saveRadio(){await post('/api/radio',{sta_bw:+$('rsb').value,ap_bw:+$('rab').value,sta_proto:buildProtoMask('sta'),ap_proto:buildProtoMask('ap'),"
"band_mode:+$('band-mode').value,tx_power_dbm:+$('tx-power').value,ap_beacon_tu:+$('ap-beacon').value,ap_dtim:+$('ap-dtim').value})}"
"async function saveRot(){await post('/api/display',{rotate:$('lrot').checked?1:0})}"
"async function saveEth(){await post('/api/eth',{mode:$('em').value})}"
"async function reboot(){if(confirm('Reboot now?'))await post('/api/reboot')}"
"async function ipStart(){await post('/api/iperf/start',{window_s:+$('iw').value})}"
"async function ipStop(){await post('/api/iperf/stop')}"
"async function scan(){$('sc').innerHTML='<tr><td>scanning...</td></tr>';try{let s=await (await fetch('/api/scan')).json();"
"$('sc').innerHTML=tr(['SSID','RSSI','Ch','Auth','Saved'],1)+s.networks.map(n=>tr([esc(n.ssid),n.rssi,n.channel,n.auth,"
"n.profile>=0?'slot '+n.profile:''])).join('')}catch(e){$('sc').innerHTML=row('Scan failed',esc(e),'bad')}}"
"let tickBusy=false;async function tick(){if(tickBusy)return;tickBusy=true;try{"
"let s=await (await fetch('/api/status')).json();"
"$('t').innerHTML="
"row('Firmware',s.firmware)+row('ESP-IDF',s.idf)+row('Chip',s.chip)+row('Reset reason',s.reset_reason)+"
"row('Uptime',fmtUp(s.uptime_s))+row('Partition',s.partition)+"
"row('Heap free',(s.heap_free/1024).toFixed(1)+' KB (min '+(s.heap_min/1024).toFixed(1)+' KB)')+"
"row('PSRAM free',(s.psram_free/1024).toFixed(0)+' KB')+"
"row('Uplink',s.uplink,s.uplink=='none'?'bad':'ok')+"
"row('Wi-Fi STA',s.wifi_state+' (SSID '+esc(s.ssid)+', profile '+s.sta_profile+', retries '+s.wifi_retries+')',s.wifi_state=='connected'?'ok':'bad')+"
"row('STA IP',s.ip)+row('Management address',s.ap.running?s.ap.ip:(s.eth.mode=='wan'&&s.eth.has_ip?s.eth.ip:'-'))+row('RSSI',s.rssi+' dBm')+"
"row('AP',s.ap.running?esc(s.ap.ssid)+' @ '+s.ap.ip+', '+chText(s.ap)+', '+s.ap.clients+'/'+s.ap.max_clients+' clients, NAT '+(s.ap.nat?'on':'off'):"
"(s.ap.apsta_supported?'stopped':'unavailable (APSTA rejected)'),s.ap.running?'ok':'')+"
"(s.eth.rescue&&s.eth.rescue.active?row('EMERGENCY MODE','Ethernet only ('+esc(s.eth.rescue.reason)+'), option '+esc(s.eth.rescue.option)+(s.eth.rescue.error?' - error '+esc(s.eth.rescue.error):'')+'. Wi-Fi AP stopped; reboot to leave.','bad'):'')+"
"row('Ethernet',s.eth.initialized?(s.eth.mode=='lan'?'LAN (bridged to AP, '+lanNet(s)+', no own IP)'+(s.eth.bridged?'':' - bridge idle (AP off)'):'WAN'+(s.eth.ip&&s.eth.ip!='0.0.0.0'?', IP '+s.eth.ip+', GW '+s.eth.gw:', no IP yet'))+' - link '+(s.eth.link_up?'up '+s.eth.speed_mbps+' Mbit/s':'down'):"
"'not initialized '+esc(s.eth.error),s.eth.link_up?'ok':'')+"
"row('ESP-Hosted',s.hosted)+"
"(s.coproc?row('Co-processor',(s.coproc.target||'?')+' (chip id '+s.coproc.chip_id+'), slave FW '+(s.coproc.fw_valid?s.coproc.fw:'unknown')+"
"', host '+s.coproc.host+(s.coproc.version_match?'':' - <b>MISMATCH</b>, see <a href=/c5update>C5 update</a>')+"
"(s.coproc.mac_valid?'<br>STA MAC '+s.coproc.sta_mac+', AP MAC '+s.coproc.ap_mac:'')+"
"'<br>SDIO CLK '+s.coproc.sdio.clk+' CMD '+s.coproc.sdio.cmd+' D0-D3 '+s.coproc.sdio.d0+'/'+s.coproc.sdio.d1+'/'+s.coproc.sdio.d2+'/'+s.coproc.sdio.d3+"
"', reset GPIO'+s.coproc.sdio.reset+', '+(s.coproc.sdio.freq_khz/1000)+' MHz'+(s.c5_ota_status!='idle'?'<br>C5 OTA: '+esc(s.c5_ota_status):''),"
"s.coproc.version_match?'ok':'bad'):'')+"
"row('Display',s.display_status,s.display_status.startsWith('ok')?'ok':'bad')+"
"row('SPI clock',(s.spi_clock_hz/1e6)+' MHz')+row('LCD ID read',s.lcd_id)+"
"row('Frames flushed',s.frames_flushed+' (errors '+s.flush_errors+')')+"
"row('Touch',s.touch_status,s.touch_status.startsWith('ok')?'ok':'bad')+row('Touch events',s.touch_events)+"
"row('OTA',s.ota_status)+"
"row('Debug mode',(s.debug?'ON':'off')+' <button onclick=dbg('+(s.debug?0:1)+')>'+(s.debug?'Turn off':'Turn on')+"
"'</button> <small>(DEBUG logs, 5 s heartbeat; boot checks run after a reboot)</small>');"
"$('es').textContent='Active mode: '+s.eth.mode+', saved: '+s.eth.saved_mode+(s.eth.mac?', MAC '+s.eth.mac:'')+(s.eth.mode=='lan'?' - LAN (bridged to AP, '+lanNet(s)+'), no own IP, bridge '+(s.eth.bridged?'active':'idle')+', drops '+s.eth.bridge_drops+' (to host '+s.eth.bridge_inject_drops+')':(s.eth.ip&&s.eth.ip!='0.0.0.0'?', IP '+s.eth.ip+', GW '+s.eth.gw:''));"
"$('touch').textContent=s.touch_history.join('\\n')||'(none yet)';"
"let n=await (await fetch('/api/netstats')).json();"
"$('ns').innerHTML=tr(['If','RX Mbit/s','TX Mbit/s','RX peak','TX peak','RX total MB','TX total MB','RX/TX pkts','TX drops'],1)+n.interfaces.map(i=>tr([i.name,"
"i.present?mb(i.rx_mbps):'-',i.present?mb(i.tx_mbps):'-',i.present?mb(i.rx_mbps_peak):'-',i.present?mb(i.tx_mbps_peak):'-',"
"(i.rx_bytes/1e6).toFixed(1),(i.tx_bytes/1e6).toFixed(1),i.rx_pkts+'/'+i.tx_pkts,i.tx_drops])).join('');"
"logRun=n.log_running;$('lb').textContent=logRun?'Stop log':'Start log';"
"$('lst').textContent=n.log_ever?(logRun?'Logging: ':'Log stopped: ')+n.log_samples+'/'+n.log_max+' samples':'';"
"let a=await (await fetch('/api/apstatus')).json();"
"$('aps').textContent=a.running?'Running: '+a.ssid+' ('+(a.open?'open':'WPA2')+') at '+a.ip+', '+chText(a):'AP stopped';"
"$('cl').innerHTML=a.clients.length?tr(['MAC','IP','RSSI'],1)+a.clients.map(c=>tr([c.mac,c.ip,c.rssi+' dBm'])).join(''):'<tr><td>(none)</td></tr>';"
"let r=await (await fetch('/api/radio')).json(),f=r.info,BWN=['auto','20','40','40'],hx=m=>m?'0x'+m.toString(16):'default';"
"let rc=x=>'width '+BWN[x.sta_bw]+'/'+BWN[x.ap_bw]+', proto '+hx(x.sta_proto)+'/'+hx(x.ap_proto)+', band '+x.band_mode+', tx '+(x.tx_power_dbm||'default');"
"$('rd').innerHTML=tr(['','Width (C5)','Protocols (C5)','Setting (width / proto)'],1)+"
"tr(['STA (ch '+r.sta_channel+')',f.sta_width,f.sta_proto_str,BWN[r.active.sta_bw]+' / '+hx(r.active.sta_proto)])+"
"tr(['AP (ch '+r.ap_channel+')',f.ap_width,f.ap_proto_str,BWN[r.active.ap_bw]+' / '+hx(r.active.ap_proto)])+"
"tr(['Upstream AP','2nd ch '+f.upstream_second,f.upstream_phy,''])+"
"tr(['TX power',f.tx_power_dbm?f.tx_power_dbm+' dBm':'?','',r.active.tx_power_dbm?r.active.tx_power_dbm+' dBm':'default'])+"
"(r.reboot_pending?tr(['<span class=bad>Saved, reboot pending</span>','','',rc(r.saved)]):'');"
"let p=await (await fetch('/api/iperf')).json();"
"$('is').textContent=p.armed?'armed ('+p.armed_left_s+' s left) - '+(p.running?'RUNNING '+p.last_mbps.toFixed(1)+' Mbit/s':'waiting for client'):'not armed';"
"if(p.summary_mbps>0)$('is').textContent+=' | last test: '+p.summary_mbps.toFixed(1)+' Mbit/s';"
"let l=await (await fetch('/api/log')).text();let q=$('log');q.textContent=l;"
"if($('follow').checked)q.scrollTop=q.scrollHeight;"
"}catch(e){$('t').innerHTML=row('Error','device unreachable: '+esc(e),'bad');for(let id of ['ns','sp','sc','ap','cl','rd'])$(id).textContent='';$('aps').textContent='Device unreachable';$('es').textContent='Device unreachable';$('is').textContent='Device unreachable';$('log').textContent='Device unreachable'}finally{tickBusy=false}}"
"function dbg(on){fetch('/api/debug?on='+on,{method:'POST'}).then(tick)}"
"let logRun=false;"
"function resetPeaks(){fetch('/api/netstats/reset',{method:'POST'}).then(tick)}"
"function logToggle(){fetch('/api/netstats/log/'+(logRun?'stop':'start'),{method:'POST'}).then(tick)}"
"function dlLog(){location='/api/netstats/log'}"
"for(let i=0;i<10;i++)$('ss').innerHTML+='<option>'+i+'</option>';for(let i=0;i<4;i++)$('as').innerHTML+='<option>'+i+'</option>';"
"ld();tick();setInterval(tick,2000);</script><script src='/ui.js' defer></script></body></html>";

static const char UPDATE_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>MyPi-Hotspot OTA</title><style>body{font-family:system-ui,sans-serif;margin:1em;background:#111;color:#ddd}"
"a{color:#6cf}progress{width:100%;height:1.5em}</style><link rel='stylesheet' href='/ui.css'></head><body>"
MYPI_BRAND_HEADER
"<h1 style='font-size:1.3em'>Firmware update (OTA)</h1>"
"<p>Upload the <b>app image</b> <code>mypi_hotspot.bin</code> (a.k.a. <code>mypi-hotspot-*-ota.bin</code>), "
"<b>not</b> the merged full-flash image.</p>"
"<input type='file' id='f' accept='.bin'> <button onclick='up()'>Upload &amp; flash</button>"
"<p><progress id='p' value='0' max='100'></progress></p><p id='s'></p><p><a href='/'>&larr; Status</a></p>"
"<script>"
"let pt=0,pb=0;function poll(t,s){if(pb)return;pb=1;fetch('/api/status').then(r=>r.json()).then(j=>{pb=0;let o=j.ota||{};if(o.target!=t)return;"
"let m={writing:'Writing flash '+o.pct+'%',verifying:'Verifying image...',verified:'Upload complete - image verified',"
"done:'Done - rebooting into the new firmware...',failed:'FAILED: '+o.err}[o.phase];if(m)s.textContent=m;"
"if(o.phase=='done'||o.phase=='failed'){clearInterval(pt);pt=0}}).catch(()=>{pb=0;s.textContent='Device is rebooting...'})}"
"function up(){let f=document.getElementById('f').files[0];let s=document.getElementById('s');"
"if(!f){s.textContent='Choose a file first';return}"
"let x=new XMLHttpRequest();x.open('POST','/update');x.setRequestHeader('Content-Type','application/octet-stream');"
"x.upload.onprogress=e=>{if(e.lengthComputable){let q=Math.floor(e.loaded/e.total*100);document.getElementById('p').value=q;"
"s.textContent=q<100?'Uploading '+q+'% ('+e.loaded+' / '+e.total+' bytes)...':'Upload sent, waiting for the device to verify the image...';"
"if(q>=100&&!pt)pt=setInterval(()=>poll('P4',s),500)}};"
"x.timeout=180000;x.ontimeout=()=>s.textContent='No answer from the device within 3 min. Check /api/log, then power-cycle and retry (or flash via USB).';"
"x.onload=()=>{if(x.status!=200){clearInterval(pt);pt=0}s.textContent=x.status+': '+x.responseText;if(x.status==200)setTimeout(()=>location.href='/',15000)};"
"x.onerror=()=>s.textContent='Upload failed (connection error)';"
"s.textContent='Uploading 0% ('+f.size+' bytes)...';x.send(f)}</script><script src='/ui.js' defer></script></body></html>";

static const char C5UPDATE_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>MyPi-Hotspot C5 update</title><style>body{font-family:system-ui,sans-serif;margin:1em;background:#111;color:#ddd}"
"a{color:#6cf}progress{width:100%;height:1.5em}td{padding:2px 10px 2px 0}td:first-child{color:#999}"
".bad{color:#f66}.ok{color:#6f6}.warn{color:#fc6}</style><link rel='stylesheet' href='/ui.css'></head><body>"
MYPI_BRAND_HEADER
"<h1 style='font-size:1.3em'>Co-processor (ESP32-C5) firmware update</h1><p><a href='/'>&larr; Status</a></p>"
"<table id='t'></table>"
"<p>Upload the ESP-Hosted <b>slave</b> app image for the C5: <code>network_adapter.bin</code> "
"(a.k.a. <code>c5-slave-esp-hosted-*.bin</code>). Not the P4 firmware, not a merged image.</p>"
"<p class='warn'><b>Important:</b> ESP-Hosted 3.x slave firmware does not talk to a 2.x host (and vice versa). "
"After flashing C5 v3.x, Wi-Fi on this P4 firmware stops working until the P4 is updated to a matching host build "
"(v0.6.0+). Make sure you can reach this board over <b>Ethernet</b> (or USB) to do the P4 OTA afterwards. "
"A 3.x host (v0.6.0+) cannot reach a 2.x slave, so the upload then fails cleanly: flash the C5 over USB instead.</p>"
"<input type='file' id='f' accept='.bin'> <button onclick='up()'>Upload &amp; flash C5</button>"
"<p><progress id='p' value='0' max='100'></progress></p><p id='s'></p>"
"<script>const $=i=>document.getElementById(i);function esc(s){return String(s).replace(/[&<>\"']/g,c=>'&#'+c.charCodeAt(0)+';')}"
"let pt=0,pb=0;function poll(t,s){if(pb)return;pb=1;fetch('/api/status').then(r=>r.json()).then(j=>{pb=0;let o=j.ota||{};if(o.target!=t)return;"
"let m={writing:'Writing flash '+o.pct+'%',verifying:'Verifying image...',verified:'Upload complete - image verified',"
"done:'Done - rebooting into the new firmware...',failed:'FAILED: '+o.err}[o.phase];if(m)s.textContent=m;"
"if(o.phase=='done'||o.phase=='failed'){clearInterval(pt);pt=0}}).catch(()=>{pb=0;s.textContent='Device is rebooting...'})}"

"fetch('/api/status').then(r=>r.json()).then(j=>{let c=j.coproc||{};let v=c.fw_valid?c.fw:'unknown';"
"$('t').innerHTML='<tr><td>Chip</td><td>'+esc(c.target||'?')+' (id '+c.chip_id+')</td></tr>'"
"+'<tr><td>Slave firmware</td><td>'+esc(v)+'</td></tr><tr><td>Host component</td><td>'+esc(c.host)+'</td></tr>'"
"+'<tr><td>Match</td><td class='+(c.version_match?'ok':'bad')+'>'+(c.version_match?'yes':'NO')+'</td></tr>'"
"+'<tr><td>OTA</td><td>'+esc(j.c5_ota_status||'-')+'</td></tr>'});"
"function up(){let f=$('f').files[0];let s=$('s');if(!f){s.textContent='Choose a file first';return}"
"if(!confirm('Flash '+f.name+' to the C5 co-processor?'))return;"
"let x=new XMLHttpRequest();x.open('POST','/api/c5update');x.setRequestHeader('Content-Type','application/octet-stream');"
"x.upload.onprogress=e=>{if(e.lengthComputable){let q=Math.floor(e.loaded/e.total*100);$('p').value=q;"
"s.textContent=q<100?'Uploading '+q+'% ('+e.loaded+' / '+e.total+' bytes, slow: ~1-3 min)...':'Upload sent, waiting for the C5 to verify the image...';"
"if(q>=100&&!pt)pt=setInterval(()=>poll('C5',s),500)}};"
"x.timeout=300000;x.ontimeout=()=>s.textContent='No answer from the device within 5 min. Check /api/log, then power-cycle and retry.';"
"x.onload=()=>{if(x.status!=200){clearInterval(pt);pt=0}s.textContent=x.status+': '+x.responseText;if(x.status==200)setTimeout(()=>location.href='/',20000)};"
"x.onerror=()=>s.textContent='Upload failed (connection error)';"
"s.textContent='Uploading '+f.size+' bytes (slow: ~1-3 min)...';x.send(f)}</script><script src='/ui.js' defer></script></body></html>";

static const char PINS_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>MyPi-Hotspot pins</title><style>body{font-family:system-ui,sans-serif;margin:1em;background:#111;color:#ddd}"
"a{color:#6cf}pre{background:#000;color:#9f9;padding:8px;font-size:12px;white-space:pre-wrap}"
"td{padding:3px 8px}button{margin:0 2px}</style><link rel='stylesheet' href='/ui.css'></head><body>"
MYPI_BRAND_HEADER
"<h1 style='font-size:1.3em'>Wiring diagnostics</h1><p><a href='/'>&larr; Dashboard</a></p>"
"<h2 style='font-size:1.1em'>Boot-time wiring check</h2><pre id='r'></pre>"
"<h2 style='font-size:1.1em'>Manual pin control</h2>"
"<p>Set a pin HIGH/LOW and measure it with a multimeter at the LCD module (against GND): HIGH should be ~3.3 V, LOW ~0 V. "
"<b>This detaches the pin from SPI/I2C until reboot.</b></p>"
"<p><b>Logic level check:</b> set SCLK to <i>in</i> and measure SCLK against GND. ~3.3 V is correct. "
"Power the module from ONE supply pin only (leave VCC unconnected when 3V3 is wired) - feeding both kept the screen dark "
"and broke touch I2C.</p>"
"<table id='t'></table>"
"<p><button onclick=\"fetch('/api/reboot',{method:'POST'}).then(()=>document.getElementById('o').textContent='Rebooting...')\">Reboot</button></p>"
"<p id='o'></p><script>"
"fetch('/api/pins').then(r=>r.json()).then(j=>{document.getElementById('r').textContent=j.report;"
"document.getElementById('t').innerHTML=j.names.split(',').map(n=>'<tr><td>'+n+'</td><td>'+"
"['high','low','in','inpd'].map(m=>\"<button onclick=\\\"setp('\"+n+\"','\"+m+\"')\\\">\"+m+'</button>').join('')+"
"\"</td><td id='v_\"+n+\"'></td></tr>\").join('')});"
"function setp(n,m){fetch('/api/pin?name='+n+'&mode='+m).then(r=>r.text()).then(t=>document.getElementById('v_'+n).textContent=t)}"
"</script><script src='/ui.js' defer></script></body></html>";

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

static esp_err_t ui_css_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/css");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, _binary_web_ui_css_start,
                           _binary_web_ui_css_end - _binary_web_ui_css_start - 1);
}

static esp_err_t ui_js_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, _binary_web_ui_js_start,
                           _binary_web_ui_js_end - _binary_web_ui_js_start - 1);
}

static esp_err_t pins_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PINS_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_pins_get(httpd_req_t *req)
{
    char *rep = malloc(2048);
    if (!rep) return httpd_resp_send_500(req);
    pintest_report(rep, 2048);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "report", rep);
    cJSON_AddStringToObject(j, "names", pintest_names());
    free(rep);
    char *out = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!out) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return err;
}

static esp_err_t api_pin_get(httpd_req_t *req)
{
    char q[64], name[16] = "", mode[8] = "";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "name", name, sizeof(name)) != ESP_OK ||
        httpd_query_key_value(q, "mode", mode, sizeof(mode)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "usage: /api/pin?name=LCD_RESET&mode=high|low|in|inpd");
    }
    int level = -1;
    esp_err_t err = pintest_set(name, mode, &level);
    char out[64];
    if (err == ESP_OK) {
        snprintf(out, sizeof(out), "%s -> %s, reads %d", name, mode, level);
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        snprintf(out, sizeof(out), "error: %s", esp_err_to_name(err));
    }
    return httpd_resp_sendstr(req, out);
}

static void reboot_task(void *arg);
static void add_ap_info(cJSON *o, const wifi_ap_info_t *a);
static void add_eth_info(cJSON *o);

static void add_coproc_info(cJSON *o)
{
    wifi_coproc_info_t c;
    wifi_get_coproc_info(&c);
    char s[24];
    cJSON_AddBoolToObject(o, "fw_valid", c.fw_valid);
    snprintf(s, sizeof(s), "%lu.%lu.%lu", (unsigned long)c.fw_major, (unsigned long)c.fw_minor, (unsigned long)c.fw_patch);
    cJSON_AddStringToObject(o, "fw", c.fw_valid ? s : "");
    cJSON_AddBoolToObject(o, "cp_valid", c.cp_valid);
    cJSON_AddNumberToObject(o, "chip_id", c.chip_id);
    cJSON_AddStringToObject(o, "target", c.cp_valid ? c.target : "");
    snprintf(s, sizeof(s), "%d.%d.%d", c.host_major, c.host_minor, c.host_patch);
    cJSON_AddStringToObject(o, "host", s);
    cJSON_AddBoolToObject(o, "version_match", c.version_match);
    cJSON_AddBoolToObject(o, "wifi_init_ok", c.wifi_init_ok);
    cJSON_AddBoolToObject(o, "mac_valid", c.mac_valid);
    if (c.mac_valid) {
        snprintf(s, sizeof(s), MACSTR, MAC2STR(c.sta_mac));
        cJSON_AddStringToObject(o, "sta_mac", s);
        snprintf(s, sizeof(s), MACSTR, MAC2STR(c.ap_mac));
        cJSON_AddStringToObject(o, "ap_mac", s);
    }
    cJSON *sd = cJSON_AddObjectToObject(o, "sdio");
    cJSON_AddNumberToObject(sd, "clk", c.sdio_clk);
    cJSON_AddNumberToObject(sd, "cmd", c.sdio_cmd);
    cJSON_AddNumberToObject(sd, "d0", c.sdio_d0);
    cJSON_AddNumberToObject(sd, "d1", c.sdio_d1);
    cJSON_AddNumberToObject(sd, "d2", c.sdio_d2);
    cJSON_AddNumberToObject(sd, "d3", c.sdio_d3);
    cJSON_AddNumberToObject(sd, "reset", c.reset_gpio);
    cJSON_AddNumberToObject(sd, "freq_khz", c.sdio_freq_khz);
    cJSON_AddBoolToObject(o, "ota_busy", s_c5_ota_busy);
}

// POST /api/debug?on=1|0 - switch debug mode (saved in NVS, survives reboot).
static esp_err_t api_debug_post(httpd_req_t *req)
{
    char q[16], v[4] = "";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "on", v, sizeof(v)) != ESP_OK || (v[0] != '0' && v[0] != '1')) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "usage: POST /api/debug?on=1|0");
    }
    esp_err_t err = diag_set_debug(v[0] == '1');
    char out[64];
    snprintf(out, sizeof(out), "debug %s (saved: %s)", v[0] == '1' ? "on" : "off", esp_err_to_name(err));
    return httpd_resp_sendstr(req, out);
}

static esp_err_t api_reboot_post(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"rebooting\"}");
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t update_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, UPDATE_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get(httpd_req_t *req)
{
    diag_status_t *st = malloc(sizeof(*st));
    if (!st) return httpd_resp_send_500(req);
    diag_get(st);

    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    char chipstr[48];
    snprintf(chipstr, sizeof(chipstr), "%s rev v%d.%d, %d cores", CONFIG_IDF_TARGET,
             chip.revision / 100, chip.revision % 100, chip.cores);

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "firmware", app->version);
    cJSON_AddNumberToObject(cJSON_AddObjectToObject(j, "display"), "rotate", display_get_rotated() ? 1 : 0);
    cJSON_AddStringToObject(j, "idf", app->idf_ver);
    cJSON_AddStringToObject(j, "chip", chipstr);
    cJSON_AddStringToObject(j, "reset_reason", diag_reset_reason_name());
    cJSON_AddNumberToObject(j, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(j, "partition", running ? running->label : "?");
    cJSON_AddNumberToObject(j, "heap_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(j, "heap_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(j, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddStringToObject(j, "wifi_state", diag_wifi_state_name(st->wifi_state));
    cJSON_AddStringToObject(j, "ssid", st->ssid);
    cJSON_AddNumberToObject(j, "wifi_retries", st->wifi_retries);
    cJSON_AddStringToObject(j, "ip", st->ip);
    cJSON_AddNumberToObject(j, "rssi", st->rssi);
    cJSON_AddStringToObject(j, "hosted", st->hosted_info);
    cJSON_AddStringToObject(j, "display_status", st->display_status);
    cJSON_AddNumberToObject(j, "spi_clock_hz", st->spi_clock_hz);
    cJSON_AddStringToObject(j, "lcd_id", st->lcd_id);
    cJSON_AddNumberToObject(j, "frames_flushed", st->frames_flushed);
    cJSON_AddNumberToObject(j, "flush_errors", st->flush_errors);
    cJSON_AddStringToObject(j, "touch_status", st->touch_status);
    cJSON_AddNumberToObject(j, "touch_events", st->touch_events);
    cJSON *hist = cJSON_AddArrayToObject(j, "touch_history");
    for (int i = 0; i < DIAG_TOUCH_HISTORY; i++) {   // Oldest first
        const char *e = st->touch_history[(st->touch_history_head + i) % DIAG_TOUCH_HISTORY];
        if (e[0]) cJSON_AddItemToArray(hist, cJSON_CreateString(e));
    }
    cJSON_AddStringToObject(j, "ota_status", st->ota_status);
    cJSON_AddBoolToObject(j, "debug", diag_debug_enabled());
    free(st);

    wifi_sta_extra_t ex;
    wifi_get_sta_extra(&ex);
    cJSON_AddStringToObject(j, "uplink", ex.uplink);
    cJSON_AddNumberToObject(j, "sta_profile", ex.sta_profile);
    cJSON_AddBoolToObject(j, "sta_manual_off", ex.sta_manual_off);
    wifi_ap_info_t ap;
    wifi_get_ap_info(&ap);
    add_ap_info(cJSON_AddObjectToObject(j, "ap"), &ap);
    cJSON_AddNumberToObject(j, "ap_channel", ap.channel);
    cJSON_AddNumberToObject(j, "sta_channel", ap.sta_channel);
    add_eth_info(cJSON_AddObjectToObject(j, "eth"));
    add_coproc_info(cJSON_AddObjectToObject(j, "coproc"));
    cJSON_AddStringToObject(j, "c5_ota_status", s_c5_ota_status);
    diag_ota_t ota;
    diag_get_ota(&ota);
    cJSON *oj = cJSON_AddObjectToObject(j, "ota");
    cJSON_AddBoolToObject(oj, "active", ota.active);
    cJSON_AddStringToObject(oj, "target", ota.target == OTA_TARGET_C5 ? "C5" : "P4");
    cJSON_AddStringToObject(oj, "phase", diag_ota_phase_name(ota.phase));
    cJSON_AddNumberToObject(oj, "pct", ota.pct);
    cJSON_AddNumberToObject(oj, "bytes_done", ota.bytes_done);
    cJSON_AddNumberToObject(oj, "bytes_total", ota.bytes_total);
    cJSON_AddStringToObject(oj, "err", ota.err);

    char *out = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!out) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return err;
}

static esp_err_t log_get(httpd_req_t *req)
{
    size_t cap = logbuf_capacity() + 1;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(cap);
    if (!buf) return httpd_resp_send_500(req);
    size_t n = logbuf_read(buf, cap);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, buf, n);
    free(buf);
    return err;
}

static void reboot_task(void *arg)
{
    // >= 1.5 s: lets the HTTP response go out and the LCD draw "Done - rebooting".
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGW(TAG, "Rebooting into new firmware");
    esp_restart();
}

#define UPLOAD_RECV_TIMEOUT_S 15    // must match cfg.recv_wait_timeout
#define UPLOAD_IDLE_MAX       3     // give up after 3 consecutive receive timeouts (45 s without data)

static esp_err_t ota_fail(httpd_req_t *req, esp_ota_handle_t h, const char *status, const char *msg)
{
    if (h) esp_ota_abort(h);
    ESP_LOGE(TAG, "OTA failed: %s", msg);
    diag_set_ota_status("failed: %s", msg);
    diag_ota_fail("%s", msg);
    s_ota_busy = false;
    httpd_resp_set_status(req, status);
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t update_post(httpd_req_t *req)
{
    if (s_ota_busy) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_send(req, "Another update is in progress", HTTPD_RESP_USE_STRLEN);
    }
    s_ota_busy = true;
    diag_ota_begin(OTA_TARGET_P4, (uint32_t)req->content_len);

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) return ota_fail(req, 0, "500 Internal Server Error", "No OTA partition (wrong partition table?)");
    if (req->content_len == 0 || req->content_len > target->size) {
        return ota_fail(req, 0, "400 Bad Request", "Image size is 0 or larger than the OTA slot");
    }
    ESP_LOGI(TAG, "OTA start: %u bytes -> %s @ 0x%06lx", (unsigned)req->content_len, target->label,
             (unsigned long)target->address);
    diag_set_ota_status("receiving %u bytes", (unsigned)req->content_len);

    const size_t bufsize = 4096;
    char *buf = malloc(bufsize);
    if (!buf) return ota_fail(req, 0, "500 Internal Server Error", "Out of memory");

    esp_ota_handle_t h = 0;
    size_t received = 0;
    bool header_checked = false;
    int last_pct = -1;
    esp_err_t err;

    int idle = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf, bufsize);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "OTA: no data for %d s (%u / %u bytes)", (idle + 1) * UPLOAD_RECV_TIMEOUT_S,
                     (unsigned)received, (unsigned)req->content_len);
            if (++idle < UPLOAD_IDLE_MAX) continue;
            free(buf);
            return ota_fail(req, h, "408 Request Timeout", "Upload stalled (no data received for 45 s)");
        }
        idle = 0;
        if (r <= 0) {
            free(buf);
            return ota_fail(req, h, "400 Bad Request", "Connection closed during upload");
        }
        if (!header_checked) {
            // Validate the image header before touching flash.
            const size_t need = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
            if ((size_t)r < need || (uint8_t)buf[0] != ESP_IMAGE_HEADER_MAGIC) {
                free(buf);
                return ota_fail(req, 0, "400 Bad Request",
                                "Not an ESP app image (did you upload the merged full-flash bin? Use mypi_hotspot.bin)");
            }
            const esp_image_header_t *hdr = (const esp_image_header_t *)buf;
            const esp_app_desc_t *desc = (const esp_app_desc_t *)(buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
            if (hdr->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID || desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
                free(buf);
                return ota_fail(req, 0, "400 Bad Request", "Image is for a different chip or has no app descriptor");
            }
            ESP_LOGI(TAG, "OTA image: project '%s' version '%s' built %s %s, IDF %s",
                     desc->project_name, desc->version, desc->date, desc->time, desc->idf_ver);
            err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &h);
            if (err != ESP_OK) {
                free(buf);
                return ota_fail(req, 0, "500 Internal Server Error", esp_err_to_name(err));
            }
            header_checked = true;
        }
        err = esp_ota_write(h, buf, r);
        if (err != ESP_OK) {
            free(buf);
            return ota_fail(req, h, "500 Internal Server Error", esp_err_to_name(err));
        }
        received += r;
        diag_ota_progress((uint32_t)received);
        int pct = (int)(received * 100 / req->content_len);
        if (pct / 10 != last_pct / 10) {
            ESP_LOGI(TAG, "OTA %d%% (%u / %u)", pct, (unsigned)received, (unsigned)req->content_len);
            diag_set_ota_status("writing %d%%", pct);
            last_pct = pct;
        }
    }
    free(buf);

    diag_ota_phase(OTA_PHASE_VERIFYING);
    err = esp_ota_end(h);   // Verifies image integrity (checksum / SHA-256)
    if (err != ESP_OK) {
        return ota_fail(req, 0, "400 Bad Request",
                        err == ESP_ERR_OTA_VALIDATE_FAILED ? "Image validation failed (corrupt upload?)" : esp_err_to_name(err));
    }
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        return ota_fail(req, 0, "500 Internal Server Error", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "OTA complete, boot partition set to %s", target->label);
    diag_set_ota_status("done, rebooting into %s", target->label);
    diag_ota_phase(OTA_PHASE_VERIFIED);
    httpd_resp_sendstr(req, "OK - image verified, rebooting into the new firmware...");
    diag_ota_phase(OTA_PHASE_DONE);
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Co-processor (ESP32-C5) OTA via ESP-Hosted RPC (v0.5.5+)
// ---------------------------------------------------------------------------

#define C5_CHIP_ID      23          // ESP_CHIP_ID_ESP32C5
#define C5_MAX_IMAGE    (4 * 1024 * 1024)
#define C5_CHUNK        1480        // per-RPC payload, as in the ESP-Hosted host_performs_slave_ota example

static void c5_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void c5_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_c5_ota_status, sizeof(s_c5_ota_status), fmt, ap);
    va_end(ap);
}

static esp_err_t c5_fail(httpd_req_t *req, bool locked, bool begun, const char *status, const char *msg)
{
    if (begun) esp_hosted_slave_ota_end();      // Close the session; an incomplete image fails validation on the C5
    if (locked) wifi_rpc_unlock();
    ESP_LOGE(TAG, "C5 OTA failed: %s", msg);
    c5_status("failed: %s", msg);
    diag_ota_fail("%s", msg);
    s_c5_ota_busy = false;
    httpd_resp_set_status(req, status);
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t c5update_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, C5UPDATE_HTML, HTTPD_RESP_USE_STRLEN);
}

// POST /api/c5update - body = ESP-Hosted slave app image (network_adapter.bin) for the ESP32-C5.
static esp_err_t api_c5update_post(httpd_req_t *req)
{
    if (s_c5_ota_busy || s_ota_busy) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_send(req, "Another update is in progress", HTTPD_RESP_USE_STRLEN);
    }
    s_c5_ota_busy = true;
    diag_ota_begin(OTA_TARGET_C5, (uint32_t)req->content_len);
    if (req->content_len < 1024 || req->content_len > C5_MAX_IMAGE) {
        return c5_fail(req, false, false, "400 Bad Request", "Image size is too small or larger than 4 MB");
    }
    wifi_coproc_info_t cp;
    wifi_get_coproc_info(&cp);
    if (cp.cp_valid && cp.chip_id != C5_CHIP_ID) {
        return c5_fail(req, false, false, "400 Bad Request", "Co-processor is not an ESP32-C5");
    }
    // Hold the RPC lock for the whole transfer so scans/reconfig don't interleave with OTA RPCs.
    if (!wifi_rpc_lock(10000)) {
        return c5_fail(req, false, false, "503 Service Unavailable", "Co-processor busy (RPC lock timeout)");
    }
    ESP_LOGI(TAG, "C5 OTA start: %u bytes (slave FW now %s%lu.%lu.%lu)", (unsigned)req->content_len,
             cp.fw_valid ? "v" : "unknown ", (unsigned long)cp.fw_major, (unsigned long)cp.fw_minor,
             (unsigned long)cp.fw_patch);
    c5_status("receiving %u bytes", (unsigned)req->content_len);

    const size_t bufsize = 4096;
    uint8_t *buf = malloc(bufsize);
    if (!buf) return c5_fail(req, true, false, "500 Internal Server Error", "Out of memory");

    size_t received = 0;
    bool begun = false;
    int last_pct = -1;
    esp_err_t err;
    char newver[32] = "?";

    int idle = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, (char *)buf, bufsize);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "C5 OTA: no data for %d s (%u / %u bytes)", (idle + 1) * UPLOAD_RECV_TIMEOUT_S,
                     (unsigned)received, (unsigned)req->content_len);
            if (++idle < UPLOAD_IDLE_MAX) continue;
            free(buf);
            return c5_fail(req, true, begun, "408 Request Timeout", "Upload stalled (no data received for 45 s)");
        }
        idle = 0;
        if (r <= 0) {
            free(buf);
            return c5_fail(req, true, begun, "400 Bad Request", "Connection closed during upload");
        }
        if (!begun) {
            const size_t need = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
            const esp_image_header_t *hdr = (const esp_image_header_t *)buf;
            const esp_app_desc_t *desc = (const esp_app_desc_t *)(buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
            if ((size_t)r < need || buf[0] != ESP_IMAGE_HEADER_MAGIC || desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
                free(buf);
                return c5_fail(req, true, false, "400 Bad Request",
                               "Not an ESP app image (use network_adapter.bin, not a merged/bootloader image)");
            }
            if (hdr->chip_id != C5_CHIP_ID) {
                bool is_p4 = hdr->chip_id == CONFIG_IDF_FIRMWARE_CHIP_ID;
                free(buf);
                return c5_fail(req, true, false, "400 Bad Request",
                               is_p4 ? "This is a P4 image - use /update for it" : "Image is not built for the ESP32-C5");
            }
            strlcpy(newver, desc->version, sizeof(newver));
            ESP_LOGI(TAG, "C5 image: project '%s' version '%s' built %s %s, IDF %s",
                     desc->project_name, desc->version, desc->date, desc->time, desc->idf_ver);
            err = esp_hosted_slave_ota_begin();
            if (err != ESP_OK) {
                free(buf);
                return c5_fail(req, true, false, "500 Internal Server Error", "esp_hosted_slave_ota_begin failed");
            }
            begun = true;
        }
        for (int off = 0; off < r; off += C5_CHUNK) {
            uint32_t n = (uint32_t)((r - off) > C5_CHUNK ? C5_CHUNK : (r - off));
            err = esp_hosted_slave_ota_write(buf + off, n);
            if (err != ESP_OK) {
                free(buf);
                return c5_fail(req, true, begun, "500 Internal Server Error", "esp_hosted_slave_ota_write failed");
            }
        }
        received += r;
        diag_ota_progress((uint32_t)received);
        int pct = (int)(received * 100 / req->content_len);
        if (pct / 10 != last_pct / 10) {
            ESP_LOGI(TAG, "C5 OTA %d%% (%u / %u)", pct, (unsigned)received, (unsigned)req->content_len);
            c5_status("writing %d%%", pct);
            last_pct = pct;
        }
    }
    free(buf);

    diag_ota_phase(OTA_PHASE_VERIFYING);
    err = esp_hosted_slave_ota_end();   // C5 validates the image (checksum/SHA) and sets its boot partition
    if (err != ESP_OK) {
        return c5_fail(req, true, false, "400 Bad Request", "C5 rejected the image (validation failed)");
    }
    diag_ota_phase(OTA_PHASE_VERIFIED);
    // ota_activate exists in slave firmware >= 2.6.0; older slaves reboot into the new image on their own.
    bool can_activate = cp.fw_valid && (cp.fw_major > 2 || (cp.fw_major == 2 && cp.fw_minor > 5));
    if (can_activate) {
        err = esp_hosted_slave_ota_activate();
        ESP_LOGI(TAG, "C5 OTA activate: %s", esp_err_to_name(err));
    }
    wifi_rpc_unlock();
    ESP_LOGW(TAG, "C5 OTA complete (new slave FW '%s'), restarting P4 to resync", newver);
    c5_status("done (%s), rebooting", newver);
    char msg[160];
    snprintf(msg, sizeof(msg), "OK - C5 image '%s' verified and %s. Rebooting the P4 to resync with the co-processor...",
             newver, can_activate ? "activated" : "flashed");
    httpd_resp_sendstr(req, msg);
    diag_ota_phase(OTA_PHASE_DONE);
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// JSON helpers and network API (v0.5.0+)
// ---------------------------------------------------------------------------

static esp_err_t send_json(httpd_req_t *req, cJSON *j)
{
    char *out = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!out) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return err;
}

/** {"ok":true,"msg":...} or 400 {"ok":false,"error":...}. */
static esp_err_t send_result(httpd_req_t *req, esp_err_t res, const char *msg, const char *errhint)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", res == ESP_OK);
    if (res == ESP_OK) {
        if (msg) cJSON_AddStringToObject(j, "msg", msg);
    } else {
        char e[160];
        snprintf(e, sizeof(e), "%s%s%s", esp_err_to_name(res), errhint ? ": " : "", errhint ? errhint : "");
        cJSON_AddStringToObject(j, "error", e);
        httpd_resp_set_status(req, "400 Bad Request");
    }
    return send_json(req, j);
}

/** Read a small JSON request body (empty body = empty object). NULL on error. */
static cJSON *read_json(httpd_req_t *req)
{
    if (req->content_len == 0) return cJSON_CreateObject();
    if (req->content_len > 1024) return NULL;
    char *buf = malloc(req->content_len + 1);
    if (!buf) return NULL;
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        got += r;
    }
    buf[got] = 0;
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    return j;
}

static int jint(const cJSON *j, const char *k, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, k);
    if (cJSON_IsNumber(v)) return v->valueint;
    if (cJSON_IsString(v) && v->valuestring[0]) return atoi(v->valuestring);
    return def;
}

static bool jbool(const cJSON *j, const char *k, bool def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, k);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (cJSON_IsNumber(v)) return v->valueint != 0;
    return def;
}

static const char *jstr(const cJSON *j, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, k);
    return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : "";
}

#define READ_BODY_OR_400(var)                                                   \
    cJSON *var = read_json(req);                                                \
    if (!var) return send_result(req, ESP_ERR_INVALID_ARG, NULL, "body must be JSON (max 1 KB)")

static void mac_str(const uint8_t m[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void subnet_str(int idx, char *out, size_t n)
{
    if (idx < 0 || idx >= AP_SUBNET_COUNT) idx = 0;
    snprintf(out, n, "%u.%u.%u.0/24", AP_SUBNET_BASE[idx][0], AP_SUBNET_BASE[idx][1], AP_SUBNET_BASE[idx][2]);
}

// --- STA profiles ---

// GET /api/profiles - passwords are never returned, only has_pass.
static esp_err_t api_profiles_get(httpd_req_t *req)
{
    wifi_sta_extra_t ex;
    wifi_get_sta_extra(&ex);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "active", ex.sta_profile);
    cJSON_AddBoolToObject(j, "manual_off", ex.sta_manual_off);
    cJSON *arr = cJSON_AddArrayToObject(j, "profiles");
    for (int i = 0; i < STA_PROFILE_MAX; i++) {
        sta_profile_t p;
        sta_profile_get(i, &p);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "slot", i);
        cJSON_AddBoolToObject(o, "valid", p.valid);
        cJSON_AddStringToObject(o, "ssid", p.valid ? p.ssid : "");
        cJSON_AddBoolToObject(o, "has_pass", p.valid && p.pass[0]);
        cJSON_AddBoolToObject(o, "dhcp", !p.valid || p.dhcp);
        cJSON_AddStringToObject(o, "ip", p.valid ? p.ip : "");
        cJSON_AddStringToObject(o, "mask", p.valid ? p.mask : "");
        cJSON_AddStringToObject(o, "gw", p.valid ? p.gw : "");
        cJSON_AddStringToObject(o, "dns", p.valid ? p.dns : "");
        cJSON_AddItemToArray(arr, o);
    }
    return send_json(req, j);
}

// POST /api/profile/save {slot,ssid,pass,clear_pass,dhcp,ip,mask,gw,dns}
// An empty pass keeps the stored password of the same SSID unless clear_pass is set.
static esp_err_t api_profile_save(httpd_req_t *req)
{
    READ_BODY_OR_400(j);
    int slot = jint(j, "slot", -1);
    sta_profile_t old, p = { 0 };
    if (!sta_profile_get(slot, &old)) {
        cJSON_Delete(j);
        return send_result(req, ESP_ERR_INVALID_ARG, NULL, "slot must be 0-9");
    }
    strlcpy(p.ssid, jstr(j, "ssid"), sizeof(p.ssid));
    const char *pw = jstr(j, "pass");
    if (pw[0]) {
        strlcpy(p.pass, pw, sizeof(p.pass));
    } else if (!jbool(j, "clear_pass", false) && old.valid && strcmp(old.ssid, p.ssid) == 0) {
        strlcpy(p.pass, old.pass, sizeof(p.pass));
    }
    p.dhcp = jbool(j, "dhcp", true);
    strlcpy(p.ip, jstr(j, "ip"), sizeof(p.ip));
    strlcpy(p.mask, jstr(j, "mask"), sizeof(p.mask));
    strlcpy(p.gw, jstr(j, "gw"), sizeof(p.gw));
    strlcpy(p.dns, jstr(j, "dns"), sizeof(p.dns));
    p.valid = true;
    cJSON_Delete(j);
    return send_result(req, sta_profile_save(slot, &p), "saved",
                       "SSID required, password empty or >= 8 chars, static IP needs valid ip/mask/gw");
}

static esp_err_t api_profile_del(httpd_req_t *req)
{
    READ_BODY_OR_400(j);
    int slot = jint(j, "slot", -1);
    cJSON_Delete(j);
    return send_result(req, sta_profile_delete(slot), "deleted", "slot must be 0-9");
}

// POST /api/connect {slot} (-1 = automatic, best RSSI)
static esp_err_t api_connect_post(httpd_req_t *req)
{
    READ_BODY_OR_400(j);
    int slot = jint(j, "slot", -1);
    cJSON_Delete(j);
    return send_result(req, wifi_connect_profile(slot), "connecting", "invalid or empty profile slot");
}

static esp_err_t api_disconnect_post(httpd_req_t *req)
{
    return send_result(req, wifi_disconnect(), "disconnected (stays off until connect)", NULL);
}

// GET /api/scan - blocking 2-4 s
static esp_err_t api_scan_get(httpd_req_t *req)
{
    const int max = 32;
    wifi_scan_entry_t *e = calloc(max, sizeof(*e));
    if (!e) return httpd_resp_send_500(req);
    int n = wifi_scan(e, max);
    if (n < 0) {
        free(e);
        return send_result(req, ESP_FAIL, NULL, "scan failed (Wi-Fi not started?)");
    }
    cJSON *j = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(j, "networks");
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", e[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", e[i].rssi);
        cJSON_AddNumberToObject(o, "channel", e[i].channel);
        cJSON_AddStringToObject(o, "auth", e[i].auth == 0 ? "open" : "secured");
        cJSON_AddNumberToObject(o, "profile", e[i].profile);
        cJSON_AddItemToArray(arr, o);
    }
    free(e);
    return send_json(req, j);
}

// --- AP profiles ---

static esp_err_t api_approfiles_get(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateObject();
    cJSON *subs = cJSON_AddArrayToObject(j, "subnets");
    char s[24];
    for (int i = 0; i < AP_SUBNET_COUNT; i++) {
        subnet_str(i, s, sizeof(s));
        cJSON_AddItemToArray(subs, cJSON_CreateString(s));
    }
    cJSON *arr = cJSON_AddArrayToObject(j, "profiles");
    for (int i = 0; i < AP_PROFILE_MAX; i++) {
        ap_profile_t p;
        ap_profile_get(i, &p);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "slot", i);
        cJSON_AddBoolToObject(o, "valid", p.valid);
        cJSON_AddStringToObject(o, "ssid", p.valid ? p.ssid : "");
        cJSON_AddBoolToObject(o, "has_pass", p.valid && p.pass[0]);
        cJSON_AddNumberToObject(o, "subnet", p.valid ? p.subnet : 0);
        cJSON_AddBoolToObject(o, "nat", !p.valid || p.nat);
        cJSON_AddBoolToObject(o, "on_boot", p.valid && p.on_boot);
        cJSON_AddItemToArray(arr, o);
    }
    return send_json(req, j);
}

// POST /api/approfile/save {slot,ssid,pass,clear_pass,subnet,nat,on_boot}
static esp_err_t api_approfile_save(httpd_req_t *req)
{
    READ_BODY_OR_400(j);
    int slot = jint(j, "slot", -1);
    ap_profile_t old, p = { 0 };
    if (!ap_profile_get(slot, &old)) {
        cJSON_Delete(j);
        return send_result(req, ESP_ERR_INVALID_ARG, NULL, "slot must be 0-3");
    }
    strlcpy(p.ssid, jstr(j, "ssid"), sizeof(p.ssid));
    const char *pw = jstr(j, "pass");
    if (pw[0]) {
        strlcpy(p.pass, pw, sizeof(p.pass));
    } else if (!jbool(j, "clear_pass", false) && old.valid && strcmp(old.ssid, p.ssid) == 0) {
        strlcpy(p.pass, old.pass, sizeof(p.pass));
    }
    int sub = jint(j, "subnet", 0);
    p.subnet = (sub >= 0 && sub < AP_SUBNET_COUNT) ? sub : 0;
    p.nat = jbool(j, "nat", true);
    p.on_boot = jbool(j, "on_boot", false);
    p.valid = true;
    cJSON_Delete(j);
    return send_result(req, ap_profile_save(slot, &p), "saved (restart the AP to apply)",
                       "SSID required, password empty (open) or >= 8 chars");
}

static esp_err_t api_approfile_del(httpd_req_t *req)
{
    READ_BODY_OR_400(j);
    int slot = jint(j, "slot", -1);
    cJSON_Delete(j);
    return send_result(req, ap_profile_delete(slot), "deleted", "slot must be 0-3");
}

// POST /api/ap/start {slot} (-1 = Kconfig default AP)
static esp_err_t api_ap_start(httpd_req_t *req)
{
    READ_BODY_OR_400(j);
    int slot = jint(j, "slot", -1);
    cJSON_Delete(j);
    return send_result(req, wifi_ap_start(slot), "AP started", "empty slot or APSTA not supported");
}

static esp_err_t api_ap_stop(httpd_req_t *req)
{
    return send_result(req, wifi_ap_stop(), "AP stopped", NULL);
}

static void add_ap_info(cJSON *o, const wifi_ap_info_t *a)
{
    cJSON_AddBoolToObject(o, "apsta_supported", a->apsta_supported);
    cJSON_AddBoolToObject(o, "running", a->running);
    cJSON_AddNumberToObject(o, "slot", a->slot);
    cJSON_AddStringToObject(o, "ssid", a->ssid);
    cJSON_AddBoolToObject(o, "open", a->open);
    cJSON_AddStringToObject(o, "ip", a->ip);
    cJSON_AddBoolToObject(o, "nat", a->nat);
    cJSON_AddNumberToObject(o, "channel", a->channel);
    cJSON_AddNumberToObject(o, "ap_channel", a->channel);
    cJSON_AddNumberToObject(o, "sta_channel", a->sta_channel);
    cJSON_AddNumberToObject(o, "clients", a->clients);
    cJSON_AddNumberToObject(o, "max_clients", a->max_clients);
}

// GET /api/apstatus - AP state plus connected clients (MAC, IP, RSSI)
static esp_err_t api_apstatus_get(httpd_req_t *req)
{
    wifi_ap_info_t a;
    wifi_get_ap_info(&a);
    cJSON *j = cJSON_CreateObject();
    add_ap_info(j, &a);
    cJSON *arr = cJSON_AddArrayToObject(j, "clients");
    if (a.running) {
        wifi_client_t cl[12];
        int n = wifi_ap_clients(cl, 12);
        for (int i = 0; i < n; i++) {
            char mac[18];
            mac_str(cl[i].mac, mac);
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "mac", mac);
            cJSON_AddStringToObject(o, "ip", cl[i].ip);
            cJSON_AddNumberToObject(o, "rssi", cl[i].rssi);
            cJSON_AddItemToArray(arr, o);
        }
    }
    return send_json(req, j);
}

// --- Ethernet ---

static void add_eth_info(cJSON *o)
{
    eth_info_t e;
    eth_get_info(&e);
    cJSON_AddBoolToObject(o, "initialized", e.initialized);
    cJSON_AddStringToObject(o, "mode", eth_mode_name(e.mode));
    cJSON_AddStringToObject(o, "saved_mode", eth_mode_name(e.saved_mode));
    cJSON_AddBoolToObject(o, "link_up", e.link_up);
    cJSON_AddBoolToObject(o, "has_ip", e.has_ip);
    cJSON_AddNumberToObject(o, "speed_mbps", e.speed_mbps);
    cJSON_AddBoolToObject(o, "full_duplex", e.full_duplex);
    cJSON_AddStringToObject(o, "ip", e.ip);
    cJSON_AddStringToObject(o, "gw", e.gw);
    cJSON_AddStringToObject(o, "mac", e.mac);
    cJSON_AddStringToObject(o, "error", e.error);
    cJSON_AddBoolToObject(o, "bridged", e.bridged);
    cJSON_AddNumberToObject(o, "bridge_drops", e.bridge_drops);
    cJSON_AddNumberToObject(o, "bridge_inject_drops", e.bridge_inject_drops);
    rescue_state_t r;
    rescue_get(&r);
    cJSON *rj = cJSON_AddObjectToObject(o, "rescue");
    cJSON_AddBoolToObject(rj, "active", r.active);
    cJSON_AddStringToObject(rj, "reason", rescue_reason_text(r.reason));
    char label[20] = "";
    if (r.option >= 0) rescue_option_label(r.option, label, sizeof(label));
    cJSON_AddStringToObject(rj, "option", label);
    cJSON_AddBoolToObject(rj, "busy", r.busy);
    cJSON_AddStringToObject(rj, "error", r.err == ESP_OK ? "" : esp_err_to_name(r.err));
}

static esp_err_t api_eth_get(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateObject();
    add_eth_info(j);
    return send_json(req, j);
}

// POST /api/eth {mode:"lan"|"wan"} - stored in NVS, applied after reboot
static esp_err_t api_eth_post(httpd_req_t *req)
{
    READ_BODY_OR_400(j);
    const char *m = jstr(j, "mode");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (strcmp(m, "lan") == 0) err = eth_set_mode(ETH_MODE_LAN);
    else if (strcmp(m, "wan") == 0) err = eth_set_mode(ETH_MODE_WAN);
    cJSON_Delete(j);
    return send_result(req, err, "saved - reboot to apply", "mode must be \"lan\" or \"wan\"");
}

// --- Radio settings ---

static void add_radio_cfg(cJSON *o, const wifi_radio_cfg_t *c)
{
    cJSON_AddNumberToObject(o, "sta_bw", c->sta_bw);
    cJSON_AddNumberToObject(o, "ap_bw", c->ap_bw);
    cJSON_AddNumberToObject(o, "sta_proto", c->sta_proto);
    cJSON_AddNumberToObject(o, "ap_proto", c->ap_proto);
    cJSON_AddNumberToObject(o, "band_mode", c->band_mode);
    cJSON_AddNumberToObject(o, "tx_power_dbm", c->tx_power_dbm);
    cJSON_AddNumberToObject(o, "ap_beacon_tu", c->ap_beacon_interval_tu);
    cJSON_AddNumberToObject(o, "ap_dtim", c->ap_dtim);
}

static void add_width(cJSON *o, const char *k, int mhz, bool known)
{
    char b[16];
    if (mhz) snprintf(b, sizeof(b), "%d MHz", mhz);
    else strlcpy(b, known ? "?" : "-", sizeof(b));
    cJSON_AddStringToObject(o, k, b);
}

// GET /api/radio - saved settings (top level), active/saved objects and what the C5 reports (info)
static esp_err_t api_radio_get(httpd_req_t *req)
{
    wifi_radio_info_t r;
    wifi_get_radio_info(&r);
    cJSON *j = cJSON_CreateObject();
    add_radio_cfg(j, &r.saved);
    add_radio_cfg(cJSON_AddObjectToObject(j, "active"), &r.active);
    add_radio_cfg(cJSON_AddObjectToObject(j, "saved"), &r.saved);
    cJSON_AddBoolToObject(j, "reboot_pending", memcmp(&r.active, &r.saved, sizeof(r.active)) != 0);
    cJSON_AddNumberToObject(j, "sta_channel", r.sta_channel);
    cJSON_AddNumberToObject(j, "ap_channel", r.ap_channel);
    cJSON *f = cJSON_AddObjectToObject(j, "info");
    add_width(f, "sta_width", r.sta_bw_mhz, true);
    cJSON_AddStringToObject(f, "sta_proto_str", r.sta_proto);
    cJSON_AddNumberToObject(f, "sta_proto_actual", r.sta_proto_actual);
    add_width(f, "ap_width", r.ap_bw_mhz, false);
    cJSON_AddStringToObject(f, "ap_proto_str", r.ap_proto);
    cJSON_AddNumberToObject(f, "ap_proto_actual", r.ap_proto_actual);
    cJSON_AddStringToObject(f, "upstream_second", r.up_second == 1 ? "above" : r.up_second == 2 ? "below" : "none");
    cJSON_AddStringToObject(f, "upstream_phy", r.up_phy);
    cJSON_AddNumberToObject(f, "tx_power_dbm", r.tx_power_dbm);
    return send_json(req, j);
}

// Read an optional numeric field into *dst if present; clears *ok if out of [lo, hi].
static void radio_field(const cJSON *j, const char *k, int lo, int hi, uint8_t *dst, bool *ok)
{
    if (!cJSON_GetObjectItemCaseSensitive(j, k)) return;
    int x = jint(j, k, -1);
    if (x < lo || x > hi) { *ok = false; return; }
    *dst = (uint8_t)x;
}

// POST /api/display {"rotate":0|1} - rotate screen and touch 180 degrees (saved in NVS, applied now)
static esp_err_t api_display_post(httpd_req_t *req)
{
    READ_BODY_OR_400(j);
    int rot = jint(j, "rotate", -1);
    cJSON_Delete(j);
    if (rot != 0 && rot != 1) return send_result(req, ESP_ERR_INVALID_ARG, NULL, "rotate must be 0 or 1");
    return send_result(req, display_set_rotated(rot == 1), rot ? "rotated 180" : "normal orientation", NULL);
}

// POST /api/radio {sta_bw,ap_bw (0-3), sta_proto,ap_proto (bitmask), band_mode, tx_power_dbm,
// ap_beacon_tu, ap_dtim} - missing keys keep the saved value; applied after reboot
static esp_err_t api_radio_post(httpd_req_t *req)
{
    READ_BODY_OR_400(j);
    wifi_radio_info_t r;
    wifi_get_radio_info(&r);
    wifi_radio_cfg_t c = r.saved;
    bool ok = true;
    radio_field(j, "sta_bw", RADIO_BW_AUTO, RADIO_BW_MAX, &c.sta_bw, &ok);
    radio_field(j, "ap_bw", RADIO_BW_AUTO, RADIO_BW_MAX, &c.ap_bw, &ok);
    radio_field(j, "sta_proto", 0, RADIO_PROTO_ALL, &c.sta_proto, &ok);
    radio_field(j, "ap_proto", 0, RADIO_PROTO_ALL, &c.ap_proto, &ok);
    c.sta_proto &= RADIO_PROTO_ALLOWED;     // 11ax is stripped (ESP-IDF issue #19238)
    c.ap_proto &= RADIO_PROTO_ALLOWED;
    radio_field(j, "band_mode", RADIO_BAND_AUTO, RADIO_BAND_5G, &c.band_mode, &ok);
    radio_field(j, "tx_power_dbm", 0, 21, &c.tx_power_dbm, &ok);
    radio_field(j, "ap_beacon_tu", 0, 10, &c.ap_beacon_interval_tu, &ok);
    radio_field(j, "ap_dtim", 0, 255, &c.ap_dtim, &ok);
    cJSON_Delete(j);
    return send_result(req, ok ? wifi_radio_save(&c) : ESP_ERR_INVALID_ARG, "saved - reboot to apply",
                       "*_bw: 0-2 (auto/20/40), *_proto: bitmask b=1 g=2 n=4 a=16 ac=32 (ax=64 is stripped), band_mode: 0-2, "
                       "tx_power_dbm: 0 or 2-21, ap_beacon_tu: 0-10, ap_dtim: 0-255");
}

// --- Traffic stats ---

/** Round to 2 decimals so cJSON prints e.g. 8.37 instead of 8.3699998855590820. */
static double mbps2(float v) { return round((double)v * 100.0) / 100.0; }

static esp_err_t api_netstats_get(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(j, "interfaces");
    for (int i = 0; i < NET_IF_COUNT; i++) {
        net_if_stats_t s;
        net_stats_get((net_if_id_t)i, &s);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", net_if_name((net_if_id_t)i));
        cJSON_AddBoolToObject(o, "present", s.present);
        cJSON_AddNumberToObject(o, "rx_bytes", (double)s.rx_bytes);
        cJSON_AddNumberToObject(o, "tx_bytes", (double)s.tx_bytes);
        cJSON_AddNumberToObject(o, "rx_pkts", s.rx_pkts);
        cJSON_AddNumberToObject(o, "tx_pkts", s.tx_pkts);
        cJSON_AddNumberToObject(o, "rx_mbps", mbps2(s.rx_mbps));   // Mbit/s, 2 decimals
        cJSON_AddNumberToObject(o, "tx_mbps", mbps2(s.tx_mbps));
        cJSON_AddNumberToObject(o, "rx_mbps_peak", mbps2(s.rx_mbps_peak));
        cJSON_AddNumberToObject(o, "tx_mbps_peak", mbps2(s.tx_mbps_peak));
        cJSON_AddNumberToObject(o, "tx_drops", s.tx_drops);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddBoolToObject(j, "log_running", net_log_is_running());
    cJSON_AddBoolToObject(j, "log_ever", net_log_ever_started());
    cJSON_AddNumberToObject(j, "log_samples", net_log_sample_count());
    cJSON_AddNumberToObject(j, "log_max", NET_LOG_MAX_SAMPLES);
    return send_json(req, j);
}

static esp_err_t api_netstats_reset(httpd_req_t *req)
{
    net_stats_reset_peaks();
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    return send_json(req, j);
}

static esp_err_t api_netlog_start(httpd_req_t *req)
{
    esp_err_t err = net_log_start();
    if (err != ESP_OK) return send_result(req, err, NULL, "PSRAM allocation failed");
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddBoolToObject(j, "running", true);
    return send_json(req, j);
}

static esp_err_t api_netlog_stop(httpd_req_t *req)
{
    net_log_stop();
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddBoolToObject(j, "running", false);
    cJSON_AddNumberToObject(j, "samples", net_log_sample_count());
    return send_json(req, j);
}

static bool csv_chunk_cb(const char *buf, size_t len, void *ctx)
{
    return httpd_resp_send_chunk((httpd_req_t *)ctx, buf, (ssize_t)len) == ESP_OK;
}

static esp_err_t api_netlog_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"hotspot_log.csv\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = net_log_write_csv(csv_chunk_cb, req);
    if (err != ESP_OK) return err;  // connection is broken; httpd closes it
    return httpd_resp_send_chunk(req, NULL, 0);
}

// --- iperf ---

static esp_err_t api_iperf_start(httpd_req_t *req)
{
    READ_BODY_OR_400(j);
    int w = jint(j, "window_s", 300);
    cJSON_Delete(j);
    return send_result(req, net_iperf_start(w > 0 ? (uint32_t)w : 300),
                       "iperf TCP server armed on port 5001", NULL);
}

static esp_err_t api_iperf_stop(httpd_req_t *req)
{
    net_iperf_stop();
    return send_result(req, ESP_OK, "iperf stopped", NULL);
}

static esp_err_t api_iperf_get(httpd_req_t *req)
{
    net_iperf_status_t s;
    net_iperf_get(&s);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "port", 5001);
    cJSON_AddBoolToObject(j, "armed", s.armed);
    cJSON_AddNumberToObject(j, "armed_left_s", s.armed_left_s);
    cJSON_AddBoolToObject(j, "running", s.running);
    cJSON_AddNumberToObject(j, "last_mbps", s.last_mbps);
    cJSON_AddNumberToObject(j, "summary_mbps", s.summary_mbps);
    cJSON_AddNumberToObject(j, "total_bytes", (double)s.total_bytes);
    return send_json(req, j);
}

esp_err_t web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 10240;
    cfg.recv_wait_timeout = UPLOAD_RECV_TIMEOUT_S;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 48;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] httpd_start: %s", esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/",           .method = HTTP_GET,  .handler = index_get },
        { .uri = "/ui.css",     .method = HTTP_GET,  .handler = ui_css_get },
        { .uri = "/ui.js",      .method = HTTP_GET,  .handler = ui_js_get },
        { .uri = "/api/status", .method = HTTP_GET,  .handler = status_get },
        { .uri = "/api/log",    .method = HTTP_GET,  .handler = log_get },
        { .uri = "/update",     .method = HTTP_GET,  .handler = update_get },
        { .uri = "/update",     .method = HTTP_POST, .handler = update_post },
        { .uri = "/c5update",   .method = HTTP_GET,  .handler = c5update_get },
        { .uri = "/api/c5update", .method = HTTP_POST, .handler = api_c5update_post },
        { .uri = "/pins",       .method = HTTP_GET,  .handler = pins_get },
        { .uri = "/api/pins",   .method = HTTP_GET,  .handler = api_pins_get },
        { .uri = "/api/pin",    .method = HTTP_GET,  .handler = api_pin_get },
        { .uri = "/api/reboot", .method = HTTP_POST, .handler = api_reboot_post },
        { .uri = "/api/debug",  .method = HTTP_POST, .handler = api_debug_post },
        { .uri = "/api/profiles",       .method = HTTP_GET,  .handler = api_profiles_get },
        { .uri = "/api/profile/save",   .method = HTTP_POST, .handler = api_profile_save },
        { .uri = "/api/profile/del",    .method = HTTP_POST, .handler = api_profile_del },
        { .uri = "/api/connect",        .method = HTTP_POST, .handler = api_connect_post },
        { .uri = "/api/disconnect",     .method = HTTP_POST, .handler = api_disconnect_post },
        { .uri = "/api/scan",           .method = HTTP_GET,  .handler = api_scan_get },
        { .uri = "/api/approfiles",     .method = HTTP_GET,  .handler = api_approfiles_get },
        { .uri = "/api/approfile/save", .method = HTTP_POST, .handler = api_approfile_save },
        { .uri = "/api/approfile/del",  .method = HTTP_POST, .handler = api_approfile_del },
        { .uri = "/api/ap/start",       .method = HTTP_POST, .handler = api_ap_start },
        { .uri = "/api/ap/stop",        .method = HTTP_POST, .handler = api_ap_stop },
        { .uri = "/api/apstatus",       .method = HTTP_GET,  .handler = api_apstatus_get },
        { .uri = "/api/eth",            .method = HTTP_GET,  .handler = api_eth_get },
        { .uri = "/api/eth",            .method = HTTP_POST, .handler = api_eth_post },
        { .uri = "/api/radio",          .method = HTTP_GET,  .handler = api_radio_get },
        { .uri = "/api/radio",          .method = HTTP_POST, .handler = api_radio_post },
        { .uri = "/api/display",        .method = HTTP_POST, .handler = api_display_post },
        { .uri = "/api/netstats",       .method = HTTP_GET,  .handler = api_netstats_get },
        { .uri = "/api/netstats/reset", .method = HTTP_POST, .handler = api_netstats_reset },
        { .uri = "/api/netstats/log/start", .method = HTTP_POST, .handler = api_netlog_start },
        { .uri = "/api/netstats/log/stop",  .method = HTTP_POST, .handler = api_netlog_stop },
        { .uri = "/api/netstats/log",   .method = HTTP_GET,  .handler = api_netlog_get },
        { .uri = "/api/iperf/start",    .method = HTTP_POST, .handler = api_iperf_start },
        { .uri = "/api/iperf/stop",     .method = HTTP_POST, .handler = api_iperf_stop },
        { .uri = "/api/iperf",          .method = HTTP_GET,  .handler = api_iperf_get },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
    ESP_LOGI(TAG, "HTTP server on port 80");
    return ESP_OK;
}
