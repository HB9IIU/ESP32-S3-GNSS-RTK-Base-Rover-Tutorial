#pragma once
#include <pgmspace.h>

// Minimal browser log viewer — connects to /logsse and displays live output.
// Late-connecting clients receive the full boot log on connect (see onConnect).
static const char WEBSERIAL_HTML[] = R"html(<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>HB9IIU Log</title><style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#1e1e1e;color:#d4d4d4;font-family:monospace;padding:12px 12px 12px 18px;height:100vh;display:flex;flex-direction:column}
h2{color:#569cd6;margin-bottom:6px;font-size:15px}
#bar{display:flex;align-items:center;gap:8px;margin-bottom:6px}
#st{font-size:12px;flex:1}
button{background:#2d2d2d;color:#d4d4d4;border:1px solid #555;padding:3px 10px;font-size:12px;cursor:pointer;border-radius:3px}
button:hover{background:#3a3a3a}
button.on{border-color:#569cd6;color:#569cd6}
select{background:#2d2d2d;color:#d4d4d4;border:1px solid #555;padding:3px 6px;font-size:12px;cursor:pointer;border-radius:3px}
#log{flex:1;background:#252526;border:1px solid #3c3c3c;padding:8px 8px 8px 10px;overflow-y:auto;white-space:pre-wrap;word-break:break-all;font-size:13px}
</style></head><body>
<h2>HB9IIU GNSS &#8212; Live Log</h2>
<div id="bar">
  <div id="st">Connecting...</div>
  <button id="bscroll" class="on" onclick="toggleScroll()">Auto-scroll ON</button>
  <button onclick="clearLog()">Clear</button>
  <select onchange="setSize(this.value)" title="Font size">
    <option value="11">11px</option>
    <option value="13" selected>13px</option>
    <option value="15">15px</option>
    <option value="17">17px</option>
    <option value="20">20px</option>
  </select>
</div>
<div id="log"></div>
<script>
const log=document.getElementById('log'),st=document.getElementById('st'),bs=document.getElementById('bscroll');
const MAX=500;
let autoScroll=true,lines=0;
function appendLine(t){
  log.textContent+=t+'\n';lines++;
  if(lines>MAX){const i=log.textContent.indexOf('\n');if(i!==-1){log.textContent=log.textContent.slice(i+1);}else lines--;}
  if(autoScroll)log.scrollTop=log.scrollHeight;
}
function toggleScroll(){
  autoScroll=!autoScroll;
  bs.textContent=autoScroll?'Auto-scroll ON':'Auto-scroll OFF';
  bs.className=autoScroll?'on':'';
  if(autoScroll)log.scrollTop=log.scrollHeight;
}
function clearLog(){log.textContent='';lines=0;}
function setSize(v){log.style.fontSize=v+'px';}
const es=new EventSource('/logsse');
es.addEventListener('log',e=>{appendLine(e.data);});
es.onopen=()=>{st.textContent='● Connected';st.style.color='#6a9955';};
es.onerror=()=>{st.textContent='● Disconnected — retrying...';st.style.color='#f44747';};
</script></body></html>)html";
