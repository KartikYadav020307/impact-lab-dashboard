import {Monitor,LineBuffer,parseLine,COLORS,exportCSV} from './model.js';
const $ = id => document.getElementById(id);
const monitor = new Monitor();
let source = 'offline', paused = false, chartSnapshot = [], demoTimer = null, demoTick = 0;
let port = null, reader = null, readTask = null, closing = false, connecting = false;
let noticeOverride = '', overrideError = false, renderedEvents = -1;
const activeFlashes = [0,0,0,0];
const fmt = value => value === null || value === undefined ? '—' : Math.round(value).toLocaleString();
const shortTime = value => new Date(value).toLocaleTimeString([], {hour:'2-digit',minute:'2-digit',second:'2-digit'});
for(let i=0;i<4;i++) {
  $('zone-grid').insertAdjacentHTML('beforeend', `<div class="zone" id="zone-${i}" style="--color:${COLORS[i]}"><div class="zone-top"><span class="zone-name">ZONE ${i+1}</span><span class="zone-number">0${i+1}</span></div><div class="zone-bottom"><span class="zone-reading" id="zone-value-${i}">—<span class="zone-unit">ADC</span></span><span class="zone-pin" id="zone-pin-${i}">GPIO ${32+i}</span></div></div>`);
  $('sensor-rows').insertAdjacentHTML('beforeend', `<div class="sensor-row" style="--color:${COLORS[i]}"><div class="sensor-top"><span class="sensor-label"><i class="sensor-dot"></i>Zone ${i+1}<small id="sensor-pin-${i}">GPIO ${32+i}</small></span><span class="sensor-value"><span id="sensor-value-${i}">—</span> <small>/ 4095</small></span></div><div class="meter"><div class="meter-fill" id="meter-${i}"></div></div><div class="sensor-bottom"><span id="base-${i}">Baseline —</span><span id="trigger-${i}">Trigger Δ —</span></div></div>`);
}
function setNotice(text, error = false) { noticeOverride = text; overrideError = error; render(); }
function fresh() { return monitor.lastSeen > 0 && Date.now()-monitor.lastSeen < 5000; }
function resetSession() { monitor.reset(); renderedEvents = -1; paused = false; chartSnapshot = []; noticeOverride=''; overrideError=false; activeFlashes.fill(0); render(); }
function ingestLine(line, redraw=true) {
  const message=parseLine(line);
  if (!message) { if(line.trim()) monitor.invalid++; return false; }
  const hit=monitor.accept(message);
  if(hit && hit.zone>=1 && !hit.ambiguous) activeFlashes[hit.zone-1]=Date.now()+1100;
  if(redraw) render();
  return true;
}
function render() {
  const stale=source==='live' && !fresh();
  $('source-label').textContent = source==='demo'?'Demo · simulated':source==='recorded'?'Recorded log':source==='live'?(stale?'USB · waiting for data':'USB · live'):'Not connected';
  $('source-dot').className='status-dot'+(source==='live'&&!stale?' live':source==='demo'?' demo':'');
  $('connect-button').textContent=connecting?'Connecting…':source==='live'?'Disconnect':'⌁  Connect ESP32';
  $('connect-button').disabled=connecting;
  $('demo-button').querySelector('span').textContent=source==='demo'?'Stop demo':'Try demo';
  $('demo-button').disabled=source==='live'||connecting;
  $('import-button').disabled=source==='live'||connecting;
  const defaultNotice = source==='demo'?'Demo session — all signals and impacts shown here are simulated.':source==='recorded'?'Recorded session — reviewing imported output, not live board data.':source==='live'?(stale?'USB is open. Waiting for complete JSON messages from the board.':monitor.warning||'Receiving live sensor data. No firmware changes or commands are sent.'):'Connect your board to begin, or try a simulated session.';
  $('notice').firstChild.textContent=(noticeOverride||defaultNotice)+' ';
  $('notice').classList.toggle('error',overrideError);
  $('total-hits').textContent=monitor.total.toLocaleString();
  $('strongest').textContent=monitor.total?fmt(monitor.strongest):'—';
  const last=monitor.events[0];
  $('last-zone').textContent=last?(last.ambiguous?'Uncertain':`Zone ${last.zone}`):'—';
  $('last-time').textContent=last?(source==='recorded'?'From imported log':shortTime(last.time)):'Waiting for an event';
  let detector='Offline',detail='No live readings';
  if(source!=='offline') {
    detector=!monitor.lastSeen?'Waiting':stale?'No data':monitor.armed?'Armed':monitor.enabled?'Settling':'Not ready';
    detail=source==='recorded'?'Last recorded state':source==='demo'?'Simulated detector state':stale?'No messages in the last 5 seconds':monitor.armed?'Listening for a large spike':monitor.enabled?'Waiting for quiet / cooldown':'Waiting for detector readiness';
  }
  $('detector').textContent=detector; $('detector').style.color=detector==='Armed'?'var(--accent)':'var(--text)'; $('detector-detail').textContent=detail;
  $('zone-count').textContent=monitor.active+' active zones';
  for(let i=0;i<4;i++) {
    $('zone-'+i).classList.toggle('disabled',i>=monitor.active);
    $('zone-'+i).classList.toggle('hit',activeFlashes[i]>Date.now());
    $('zone-value-'+i).firstChild.textContent=fmt(monitor.readings[i]);
    $('zone-pin-'+i).textContent=i>=monitor.active?'INACTIVE':`GPIO ${monitor.pins[i]??'—'}`;
    $('sensor-pin-'+i).textContent=i>=monitor.active?'Inactive':`GPIO ${monitor.pins[i]??'—'}`;
    $('sensor-value-'+i).textContent=fmt(monitor.readings[i]);
    $('meter-'+i).style.width=((monitor.readings[i]??0)/4095*100)+'%';
    $('base-'+i).textContent=`Baseline ${fmt(monitor.baseline[i])}`;
    $('trigger-'+i).textContent=`Trigger Δ ${fmt(monitor.thresholds[i])}`;
  }
  $('zone-message').textContent=last?.ambiguous?`Similar peaks — location uncertain${last.candidate?` (strongest: zone ${last.candidate})`:''}.`:'Zone layout follows your sensor numbering.';
  $('firmware').textContent=monitor.firmware||'—';
  $('mode').textContent=monitor.mode==='large_spike'?'Large spike':monitor.mode==='auto'?'Automatic':monitor.mode||'—';
  $('calibration').textContent=monitor.calibration===false?(monitor.enabled?'High noise · detection on':'Failed · detection off'):monitor.calibration===true?'Passed':'—';
  $('calibration').style.color=monitor.calibration===false?'var(--z3)':'';
  $('transport').textContent=source==='demo'?'Simulated · no USB':source==='recorded'?'Imported JSON log':'115200 baud · USB';
  $('pause-button').textContent=paused?'Resume chart':'Pause chart';
  $('export-button').disabled=!monitor.events.length;
  $('event-count').textContent=monitor.events.length;
  $('events-empty').hidden=monitor.events.length>0;
  if(renderedEvents!==monitor.total) {renderEvents();renderedEvents=monitor.total;}
  renderChart();
  const skipped=monitor.invalid?` · ${monitor.invalid} non-JSON / invalid lines skipped`:'';
  $('footer-status').textContent=(source==='demo'?'Simulated session':source==='recorded'?'Imported data stays in this browser':'Data stays in this browser · no firmware changes')+skipped;
}
function renderEvents() {
  const fragment=document.createDocumentFragment();
  for(const hit of monitor.events.slice(0,100)) {
    const row=document.createElement('tr');
    const color=hit.ambiguous?'#eeae7d':COLORS[hit.zone-1];
    const timeCell=document.createElement('td');
    timeCell.textContent=source==='recorded'&&hit.timeUs!==null?`${(hit.timeUs/1000000).toFixed(3)} s after boot`:shortTime(hit.time);
    row.append(timeCell);
    const zoneCell=document.createElement('td'); zoneCell.className='table-zone'; zoneCell.style.setProperty('--color',color); zoneCell.textContent=hit.ambiguous?'Uncertain':`●  Zone ${hit.zone}`; row.append(zoneCell);
    const peak=document.createElement('td'); peak.textContent=fmt(hit.peak);row.append(peak);
    const intensity=document.createElement('td');intensity.style.setProperty('--color',color);
    const percent=Math.round(hit.intensity*100);
    intensity.innerHTML=`<div class="intensity-cell"><span class="mini-meter"><span style="width:${percent}%"></span></span>${percent}%</div>`;row.append(intensity);
    const result=document.createElement('td');const tag=document.createElement('span');tag.className='result-tag'+(hit.ambiguous||hit.clipped?' uncertain':'');tag.textContent=hit.ambiguous?'Similar peaks':hit.clipped?'ADC clipped':'Detected';result.append(tag);row.append(result);
    fragment.append(row);
  }
  $('event-body').replaceChildren(fragment);
}
function renderChart() {
  const points=paused?chartSnapshot:monitor.history;
  $('chart-empty').hidden=points.length>0; $('sample-count').textContent=points.length+' samples'+(paused?' · paused':'');
  const lines=document.createDocumentFragment();
  for(let zone=0;zone<4;zone++) {
    let d='',started=false;
    for(let i=0;i<points.length;i++) {
      const value=points[i].values[zone];if(value===null){started=false;continue;}
      const x=45+(points.length>1?i/(points.length-1)*1045:0),y=160-value/4095*150;
      d+=`${started?'L':'M'}${x.toFixed(1)},${y.toFixed(1)} `;started=true;
      if(points.length===1){d+=`L${(x+3).toFixed(1)},${y.toFixed(1)}`;}
    }
    const path=document.createElementNS('http://www.w3.org/2000/svg','path');path.setAttribute('d',d);path.setAttribute('stroke',COLORS[zone]);path.setAttribute('class','signal-line');lines.append(path);
  }
  $('chart-lines').replaceChildren(lines);
}
function stopDemo() { if(demoTimer)clearInterval(demoTimer);demoTimer=null; }
function demoStatus() {
  demoTick++;
  const readings=[0,1,2,3].map(i=>Math.round(680+140*Math.sin(demoTick*.48+i*.9)+Math.random()*80));
  monitor.accept({type:'status',firmware:'piezo-zones-1.2 (demo)',mode:'large_spike',active_sensors:4,pins:[32,33,34,35],readings,thresholds:[1800,1800,1800,1800],baseline:[750,750,750,750],calibration_ok:true,detection_enabled:true,armed:true,uptime_ms:demoTick*1000,events:monitor.total});
  if(demoTick%3===0) {
    const zone=(Math.floor(demoTick/3)-1)%4+1,peak=1900+Math.round(Math.random()*1000);
    const peaks=[130,210,110,180];peaks[zone-1]=peak;
    const hit={type:'hit',zone,candidate_zone:zone,peak,intensity:peak/4095,peaks,readings:peaks.map(v=>v+750),time_us:demoTick*1000000,ambiguous:false,clipped:false};
    monitor.accept(hit);activeFlashes[zone-1]=Date.now()+1100;
  }
  render();
}
$('demo-button').addEventListener('click',()=>{
  if(source==='live'||connecting)return;
  if(source==='demo'){stopDemo();source='offline';resetSession();return;}
  stopDemo();resetSession();source='demo';demoTick=0;demoStatus();demoTimer=setInterval(demoStatus,1000);
});
async function connect() {
  if(connecting)return;
  if(source==='live'){await disconnect();return;}
  if(!('serial' in navigator)||!window.isSecureContext){setNotice('Live USB needs desktop Chrome or Edge on HTTPS or localhost. You can still use demo or import a serial log here.',true);return;}
  connecting=true;render();
  let chosen;
  try {
    chosen=await navigator.serial.requestPort();
    await chosen.open({baudRate:115200,dataBits:8,stopBits:1,parity:'none',flowControl:'none',bufferSize:16384});
  } catch(error) {
    if(chosen)try{await chosen.close();}catch{}
    connecting=false;
    if(error.name==='NotFoundError'){render();return;}
    setNotice('Could not open USB. Close Arduino Serial Monitor and other apps using the port, then try again. '+String(error.message).slice(0,150),true);return;
  }
  stopDemo();resetSession();port=chosen;closing=false;connecting=false;source='live';render();
  readTask=readSerial(chosen);
}
async function readSerial(chosen) {
  const decoder=new TextDecoder();const lines=new LineBuffer(line=>ingestLine(line,false));
  try {
    while(chosen.readable&&!closing) {
      reader=chosen.readable.getReader();
      try {
        while(!closing) {
          const {value,done}=await reader.read();if(done)break;
          lines.push(decoder.decode(value,{stream:true}));render();
        }
      } finally {reader.releaseLock();reader=null;}
      if(!closing)break;
    }
    lines.push(decoder.decode());lines.flush();
  } catch(error) {
    if(!closing){noticeOverride='USB stream ended. Reconnect the board to resume.';overrideError=true;}
  } finally {
    try{await chosen.close();}catch{}
    if(port===chosen){port=null;source='offline';}
    closing=false;
    if(!noticeOverride)noticeOverride='Disconnected. Session data remains available for export.';
    render();
  }
}
async function disconnect() {
  closing=true;
  try{if(reader)await reader.cancel();}catch{}
  if(readTask)await readTask;
  readTask=null;
}
$('connect-button').addEventListener('click',connect);
$('pause-button').addEventListener('click',()=>{paused=!paused;if(paused)chartSnapshot=monitor.history.map(p=>({...p,values:[...p.values]}));render();});
$('clear-button').addEventListener('click',()=>{
  monitor.events=[];monitor.total=0;monitor.strongest=0;monitor.history=[];chartSnapshot=[];renderedEvents=-1;activeFlashes.fill(0);render();
});
$('export-button').addEventListener('click',()=>{
  const blob=new Blob([exportCSV(monitor.events)],{type:'text/csv;charset=utf-8'});const url=URL.createObjectURL(blob);const a=document.createElement('a');a.href=url;a.download=`impact-lab-${source}-${new Date().toISOString().replaceAll(':','-')}.csv`;a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
});
$('import-button').addEventListener('click',()=>{if(source==='live'||connecting)return;$('import-result').textContent='';$('import-dialog').showModal();});
$('import-file').addEventListener('change',async event=>{
  const file=event.target.files[0];if(!file)return;
  if(file.size>2*1024*1024){$('import-result').textContent='Choose a log smaller than 2 MB.';return;}
  try{$('import-text').value=await file.text();$('import-result').textContent=file.name;}catch{$('import-result').textContent='The file could not be read.';}
});
$('load-button').addEventListener('click',()=>{
  const text=$('import-text').value;if(text.length>2*1024*1024){$('import-result').textContent='Please import less than 2 MB at a time.';return;}
  const lines=text.split(/\r?\n/);const valid=lines.map(parseLine).filter(Boolean);
  if(!valid.length){$('import-result').textContent='No complete firmware JSON messages found. Paste whole lines, including { and }.';return;}
  stopDemo();resetSession();source='recorded';
  for(const message of valid)monitor.accept(message);
  monitor.invalid=lines.filter(s=>s.trim()).length-valid.length;
  activeFlashes.fill(0);renderedEvents=-1;
  noticeOverride=`Imported ${valid.length} messages. This is a recorded session, not a live connection.`;
  $('import-dialog').close();render();
});
setInterval(()=>{if(source==='live'||source==='demo'||activeFlashes.some(v=>v>Date.now()-1500))render();},400);
window.addEventListener('beforeunload',()=>{stopDemo();if(reader)reader.cancel().catch(()=>{});});
render();
