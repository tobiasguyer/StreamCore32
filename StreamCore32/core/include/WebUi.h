// WebUI.cpp
#include "BellHTTPServer.h"
#include "AudioControl.h"

#include <BellLogger.h>
#include <civetweb.h>   // for mg_websocket_write, MG_WEBSOCKET_OPCODE_TEXT
#include <memory>
#include <mutex>
#include <string>

namespace WebUI {

  using OnWsMessage = std::function<void(struct mg_connection*, char*, size_t)>;
  // Adjust these to wherever you store the files (SPIFFS, FATFS, etc.)
  static const char* TEXT_INDEX = R"delim(
<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8" /><title>StreamCore32</title><meta name="viewport" content="width=device-width, initial-scale=1" /><link rel="stylesheet" href="style.css" /><link href="https://fonts.googleapis.com/css2?family=Material+Symbols+Outlined:opsz,wght,FILL,GRAD@48,700,1,150" rel="stylesheet" /><style>.player-controls > * { font-size: 3rem }.player-volume > * { font-size: 10px }</style></head><body><header class="topbar"><div class="topbar-left"><span class="brand">StreamCore32</span></div><nav class="topbar-nav"><button class="nav-link active" data-page="player">Player</button><button class="nav-link" data-page="radio">Radio</button><button class="nav-link" data-page="debug">Debug</button></nav></header><main class="page-wrap"><section id="page-player" class="page active"><div class="card"><h2 class="card-title">Now Playing</h2><div class="player-layout"><div class="player-cover"><div class="cover-placeholder">STREAMCORE32</div></div><div class="player-meta"><div class="track-title">Track Title</div><div class="track-artist">Artist Name</div><div class="track-album">Album Name</div><div class="player-progress"><span>01:23</span><input type="range" min="0" max="100" value="30" /><span>04:56</span></div><div class="player-controls"><button class="material-symbols-outlined btn">skip_previous</button><button class="material-symbols-outlined btn play">pause</button><button class="material-symbols-outlined btn"> skip_next</button><div class="player-volume"><span class="volume-icon">VOL</span><input type="range" min="0" max="100" value="70" /><span class="volume-label">70%</span></div></div></div></div><div class="player-footer"><span>Source: <strong class = "stream-src">Qobuz</strong></span><span>Quality: <strong class = "stream-qlty">24-bit / 96 kHz</strong></span><span>Device: <strong>StreamCore32</strong></span></div></div></section><section id="page-radio" class="page"><div class="card"><h2 class="card-title">Radio</h2><div class="radio-filters"><input type="text" placeholder="Search stations…" /><input type="text" placeholder="Taglist <…,…,…>" /><select><option>All countries</option></select><button class="btn small" data-action="radio-search">Search</button></div><table class="radio-table"><thead><tr><th>★</th><th>Name</th><th>Info</th><th class="radio-actions-col">Actions</th></tr></thead><tbody id="radio-table-body"></tbody></table><div class="radio-add"><h3>Add Station</h3><div class="form-grid"><label> Name <input type="text" placeholder="My Station" /></label><label> URL <input type="text" placeholder="http://stream.example.com" /></label></div><div class="form-actions"><button class="btn primary">Play</button><button class="btn primary">Save</button></div></div></section><section id="page-debug" class="page"><div class="card"><h2 class="card-title">Debug</h2><div class="info-list"><div class="info-row"><span>Wi-Fi signal</span><span>-58 dBm</span></div><div class="info-row"><span>Heap memory</span><span>172 KB</span></div><div class="info-row"><span>Threads</span></div></div><h3>Recent Log</h3><pre class="log-box"></pre></div></section></main><script src="app.js"></script></body></html>
)delim";
  static const char* TEXT_CSS = R"delim(
:root{--bg:#e8e8e8;--card-bg:#fafafa;--border:#bcbcbc;--border-soft:#d4d4d4;--text:#2e7da4;--muted:#2e7da4;--accent:#2e7da4;--danger:#c64a4a;--font-mono:"SF Mono", ui-monospace, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace;--input-bg:#f2f2f2;--input-border:#c8c8c8;--range-track:#c6c6c6;--range-thumb:#2e7da4}@media (prefers-color-scheme:dark){:root{--bg:#000000;--card-bg:#000000;--border:#1f2933;--border-soft:#374151;--text:#7dd3fc;--muted:#7dd3fc;--accent:#7dd3fc;--danger:#f97373;--input-bg:#020617;--input-border:#1f2933;--range-track:#111827;--range-thumb:#7dd3fc}}*,*::before,*::after{box-sizing:border-box}html,body{margin:0;padding:0;height:100%}body{font-family:var(--font-mono);font-size:13px;background:radial-gradient(circle at top,var(--bg) 0,var(--card-bg) 55%);color:var(--text)}.topbar{display:flex;align-items:baseline;justify-content:space-between;padding:8px 16px;background:var(--card-bg);color:var(--text);border-bottom:1px solid var(--border)}.brand{font-weight:600;letter-spacing:.15em;text-transform:uppercase;font-size:11px}.topbar-nav{display:flex;gap:16px}.nav-link{padding:0;margin:0;border:none;background:#fff0;color:var(--muted);font-family:inherit;font-size:12px;letter-spacing:.08em;text-transform:uppercase;cursor:pointer;position:relative}.nav-link::after{content:"";position:absolute;left:0;bottom:-4px;width:0;height:1px;background:var(--accent);transition:width 0.12s ease-out}.nav-link:hover{color:var(--text)}.nav-link.active{color:var(--accent)}.nav-link.active::after{width:100%}.page-wrap{max-width:960px;margin:16px auto 24px;padding:0 16px}.page{display:none}.page.active{display:block}.card{background:var(--card-bg);border-radius:0;border:1px solid var(--border);padding:16px}.card-title{margin:0 0 12px;font-size:14px;text-transform:uppercase;letter-spacing:.12em;color:var(--accent)}.player-layout{display:grid;grid-template-columns:minmax(400px,400px) minmax(0,1fr);gap:16px}.player-cover{width:100%;display:flex;justify-content:center}.cover-placeholder{width:100%;height:100%;padding:50%;border:1px solid var(--border);background:var(--border);display:flex;align-items:center;justify-content:center;font-size:2em;color:var(--muted)}.player-meta{min-width:0}.track-title{font-size:16px;text-transform:uppercase;letter-spacing:.08em}.track-artist{margin-top:2px;color:var(--muted)}.track-album{margin-top:2px;font-size:11px;color:var(--muted)}.player-progress{display:flex;justify-content:space-between;align-items:center;margin-top:12px;font-size:10px;color:var(--muted)}.player-progress input[type="range"]{width:100%}.player-controls{display:flex;align-items:center;gap:12px;margin-top:12px;font-size:35px;text-transform:uppercase}.player-controls .btn{padding:0;font-weight:700}.player-volume{font-size:10px;display:flex;align-items:center;padding-top:8px;gap:6px;width:100%}.player-volume input[type="range"]{flex:1}.player-footer{margin-top:14px;padding-top:8px;border-top:1px solid var(--border-soft);display:flex;flex-wrap:wrap;gap:12px;font-size:11px;color:var(--muted)}.radio-filters{display:flex;flex-wrap:wrap;gap:6px;margin-bottom:10px}.radio-table{width:100%;border-collapse:collapse;margin-bottom:12px;font-size:12px;text-align:left}.radio-table th,.radio-table td{padding:4px 4px;border-bottom:1px solid var(--border-soft);width:0%}.radio-table th{font-weight:500;color:var(--muted);text-transform:uppercase;letter-spacing:.06em;font-size:11px}.radio-table tbody tr:hover{background:rgb(100 100 100 / .08)}.radio-actions-col,.radio-actions{text-align:right;gap:8px;}.radio-add{margin-top:12px}.radio-add h3{margin:0 0 6px;font-size:12px;text-transform:uppercase;letter-spacing:.08em}.info-row table{border-collapse: collapse;text-align: right;width:100%;text-transform: uppercase;max-height:30vh;overflow-y:auto;display:block;scrollbar-width: none;-ms-overflow-style: none;}.info-row .td-or{text-align:left}.info-row th{position:sticky;top:0px;background:var(--bg);}.info-row th,td{padding-left:10px;width:10%;border-bottom: 1px solid var(--border-soft);}.tabs{display:inline-flex;gap:10px;margin-bottom:10px;border-bottom:1px solid var(--border)}.tab-link{border:none;background:#fff0;padding:0 0 4px;font-size:11px;text-transform:uppercase;letter-spacing:.08em;color:var(--muted);cursor:pointer;position:relative}.tab-link::after{content:"";position:absolute;left:0;bottom:-1px;width:0;height:1px;background:var(--accent);transition:width 0.12s}.tab-link:hover{color:var(--text)}.tab-link.active{color:var(--accent)}.tab-link.active::after{width:100%}.tab-panel{display:none;margin-top:8px;margin-bottom:10px}.tab-panel.active{display:block}.form-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:8px 12px;margin-bottom:8px}label{font-size:11px;color:var(--muted);display:flex;flex-direction:column;gap:3px}input[type="text"],input[type="email"],input[type="password"],input[type="number"],select{padding:3px 5px;border-radius:0;border:1px solid var(--input-border);font-size:12px;background:var(--input-bg);color:var(--text)}input:focus,select:focus{outline:1px solid var(--accent);border-color:var(--accent)}.file-input{display:inline-flex;align-items:center;gap:8px;padding:4px 8px;border-radius:0;border:1px dashed var(--border-soft);font-size:11px;color:var(--muted);cursor:pointer}.file-input input[type="file"]{display:none}.form-actions{display:flex;gap:10px;justify-content:flex-end;font-size:11px}.btn{border:none;background:#fff0;padding:0;font-family:inherit;font-size:inherit;color:var(--muted);cursor:pointer}.btn.primary{color:var(--muted)}.btn.ghost{color:var(--muted)}.btn.danger{color:var(--danger)}.btn.small{font-size:11px}.icon-btn{width:auto;height:auto}.info-list{border:1px solid var(--border-soft);background:var(--card-bg);margin-bottom:10px}.info-row{display:flex;justify-content:space-between;padding:4px 6px;font-size:11px}.info-row:nth-child(odd){background:rgb(0 0 0 / .03)}.info-row span:first-child{color:var(--muted)}.log-box{margin:0;padding:6px;border:1px solid var(--border-soft);background:#000;color:#cbd5f5;font-family:var(--font-mono);font-size:11px;max-height:200px;overflow:auto}input[type="range"]{-webkit-appearance:none;appearance:none;width:100%;height:3px;border-radius:999px;background:var(--range-track)}input[type="range"]::-webkit-slider-thumb{-webkit-appearance:none;width:1px;height:3px;border-radius:50%;background:var(--range-thumb);cursor:pointer;appearance:none}input[type="range"]::-moz-range-thumb{width:3px;height:3px;border-radius:50%;background:var(--range-thumb);border:none;cursor:pointer;display:none}@media (max-width:720px){.player-layout{grid-template-columns:1fr}.player-footer{flex-direction:column;align-items:flex-start}.topbar-nav{gap:10px}}
)delim";
  static const char* TEXT_JS = R"delim(
(function(){const radioState={items:[],query:"",genre:"",country:""};const RADIO_SERVERS=["https://de1.api.radio-browser.info","https://de2.api.radio-browser.info","https://fi1.api.radio-browser.info"];const radioApiBase=RADIO_SERVERS[Math.floor(Math.random()*RADIO_SERVERS.length)];function renderRadioStations(){const tbody=document.getElementById("radio-table-body");if(!tbody)
return;if(!radioState.items||radioState.items.length===0){tbody.innerHTML=`<tr><td colspan="4">No stations found. Try another search.</td></tr>`;return}
tbody.innerHTML=radioState.items.map((st)=>{const infoParts=[];if(st.genre)
infoParts.push(st.genre);if(st.country)
infoParts.push(st.country);const info=infoParts.join(" · ")||"";const fav=st.favorite?"★":"☆";return ` <tr data-url="${st.url || ""}" data-name="${st.name || ""}"> <td> <button class="btn small star-btn" data-action="radio-favorite" data-fav="${st.favorite ? "1" : "0"}"> ${fav}</button></td> <td>${st.name || ""}</td> <td>${info}</td> <td class="radio-actions"> <button class="btn small" data-action="radio-play">Play</button> <button class="btn small ghost" data-action="radio-save">Save</button> </td> </tr>`}).join("")}
const rootStyle=getComputedStyle(document.documentElement);const rangeTrackColor=(rootStyle.getPropertyValue("--range-track")||"#c6c6c6").trim();const rangeThumbColor=(rootStyle.getPropertyValue("--range-thumb")||"#2e7da4").trim();function styleRange(input){const min=Number(input.min)||0;const max=Number(input.max)||100;const val=Number(input.value);const span=max-min||1;const pct=((val-min)*100)/span;input.style.background=`linear-gradient(to right, ${rangeThumbColor} 0%, ${rangeThumbColor} ${pct}%, ${rangeTrackColor} ${pct}%, ${rangeTrackColor} 100%)`}
function initAllRanges(){document.querySelectorAll('input[type="range"]').forEach((r)=>{styleRange(r);r.addEventListener("input",()=>styleRange(r))})}
function initNav(){const navLinks=document.querySelectorAll(".nav-link");const pages=document.querySelectorAll(".page");navLinks.forEach((btn)=>{btn.addEventListener("click",()=>{const pageId="page-"+btn.dataset.page;wsSend({type:"page",page:pageId});navLinks.forEach((b)=>b.classList.remove("active"));btn.classList.add("active");pages.forEach((p)=>{p.classList.toggle("active",p.id===pageId)})})})}
function initSettingsTabs(){const tabLinks=document.querySelectorAll(".tab-link");const tabPanels=document.querySelectorAll(".tab-panel");tabLinks.forEach((btn)=>{btn.addEventListener("click",()=>{const target="tab-"+btn.dataset.tab;tabLinks.forEach((b)=>b.classList.remove("active"));btn.classList.add("active");tabPanels.forEach((p)=>{p.classList.toggle("active",p.id===target)})})})}
let ws;let reconnectTimer=null;const outbox=[];function wsSend(obj){const payload=JSON.stringify(obj);if(ws&&ws.readyState===WebSocket.OPEN){ws.send(payload)}else{outbox.push(payload)}}
function flushOutbox(){while(ws&&ws.readyState===WebSocket.OPEN&&outbox.length>0){ws.send(outbox.shift())}}
function connectWebSocket(){const proto=location.protocol==="https:"?"wss":"ws";const url=`${proto}://${location.host}/ws`;ws=new WebSocket(url);ws.onopen=()=>{flushOutbox();wsSend({type:"hello",ui:"streamcore32",version:1})};ws.onmessage=(ev)=>{try{const msg=JSON.parse(ev.data);handleWsMessage(msg)}catch{const log_box=document.querySelector(".log-box");if(log_box){var text=log_box.textContent;if(text.length>10000){log_box.textContent=text.substring(text.indexOf("\n")+1)}
log_box.textContent+=`${ev.data}\n`;log_box.scrollTop=log_box.scrollHeight}}};ws.onclose=()=>{if(reconnectTimer)
clearTimeout(reconnectTimer);reconnectTimer=setTimeout(connectWebSocket,3000)};ws.onerror=()=>{ws.close()}}
async function wikipediaSearchImageForQuery(query){if(!query)
return null;const params=new URLSearchParams({action:"query",generator:"search",gsrsearch:query,gsrlimit:"3",prop:"pageimages",piprop:"original|thumbnail",pilimit:"3",pithumbsize:"600",format:"json",origin:"*"});const url="https://en.wikipedia.org/w/api.php?"+params.toString();const res=await fetch(url);if(!res.ok){console.warn("Wikipedia search+pageimages failed:",res.status);return null}
const data=await res.json();if(!data.query||!data.query.pages){return null}
const pages=Object.values(data.query.pages);pages.sort((a,b)=>{const ia=typeof a.index==="number"?a.index:9999;const ib=typeof b.index==="number"?b.index:9999;return ia-ib});for(const page of pages){if(page.original&&page.original.source){return page.original.source}
if(page.thumbnail&&page.thumbnail.source){return page.thumbnail.source}}
return null}
async function fetchCoverFromWikipedia(artist,title){try{const queries=[];if(artist&&title){queries.push(`${artist} ${title} album`);queries.push(`${artist} ${title}`)}
if(artist){queries.push(`${artist} album`);queries.push(artist); var seperator="-(/[{,";for(var i=0;i<seperator.length;i++){const seperatorChar=seperator[i];if(artist.indexOf(seperatorChar)>-1){var newArtists=artist.split(seperatorChar);for(const newArtist of newArtists){queries.push(`${newArtist} album`);queries.push(newArtist)}}}}
var seperator="-(/[{,";if(title){queries.push(title);for(var i=0;i<seperator.length;i++){const seperatorChar=seperator[i];if(title.indexOf(seperatorChar)>-1){var newTitle=title.split(seperatorChar)[0];queries.push(newTitle)}}}
for(const q of queries){const img=await wikipediaSearchImageForQuery(q);if(img){return img}}
return null}catch(e){console.warn("Wikipedia cover fetch failed:",e);return null}}
async function fetchCoverFromMusicBrainz(artist,releaseTitle){if(!artist&&!releaseTitle)
return null;const query=[artist?`artist:${artist}`:"",releaseTitle?`recording:${releaseTitle}`:""].filter(Boolean).join(" AND ");if(!query)
return null;const mbSearchUrl="https://musicbrainz.org/ws/2/recording/?"+new URLSearchParams({query,fmt:"json",limit:"10"}).toString();const mbRes=await fetch(mbSearchUrl,{headers:{"User-Agent":"StreamCore32/1.0 (https://example.com)"}});if(!mbRes.ok){console.warn("MusicBrainz search failed:",mbRes.status);return null}
const mbData=await mbRes.json();var recordings=mbData.recordings;var url=null;for(var i=0;i<recordings.length;i++){const recording=recordings[i];if(!recording||!recording.id){console.warn("No release found for query:",query)}else{var mbids=[];mbids.push(recording.id);for(var j=recording.releases-1;j>=0;j--){mbids.push(recording.releases[j].id)}
for(var j=0;j<mbids.length;j++){var caaUrl=`https://coverartarchive.org/release/${mbids[j]}/front-1200`;var caaRes=await fetch(caaUrl,{});if(!caaRes.ok){console.warn("Cover Art Archive returned:",caaRes.status)}else{if(!caaRes.url){return caaRes.url}}}}}
if(!url)
url=await fetchCoverFromWikipedia(artist,releaseTitle);return url}
function handleWsMessage(msg){if(!msg||typeof msg!=="object")
return;switch(msg.type){case "playback":updatePlayback(msg);break;case "radio":msg.stations.forEach((item)=>{item.favorite=!0});for(var i=0;i<radioState.items.length;i++){if(radioState.items[i].favorite){radioState.items.splice(i,1)}}
msg.stations.concat(radioState.items);radioState.items=msg.stations;renderRadioStations();break;case "settings":break;case "debug":const log_box=document.getElementById("page-debug");if(log_box){var elem=log_box.querySelectorAll(".info-row");if(msg.heap){elem[1].children[1].textContent=msg.heap+" kB"}
if(msg.rssi){elem[0].children[1].textContent=msg.rssi+" dBm"}
if(msg.tasks){elem[2].removeChild(elem[2].lastChild);var new_table=document.createElement("table");new_table.innerHTML=`<thead><tr><th class="td-or">Thread</th><th>State</th><th>free</th><th>Prio</th></tr></thead>`
var td5=document.createElement("tbody");for(var i=0;i<msg.tasks.length;i++){var tr=document.createElement("tr");var td1=document.createElement("td");td1.classList.add("td-or");var td2=document.createElement("td");td1.textContent=msg.tasks[i].task;td2.textContent=msg.tasks[i].state;var td3=document.createElement("td");td3.textContent=msg.tasks[i].stack;var td4=document.createElement("td");td4.textContent=msg.tasks[i].priority;tr.appendChild(td1);tr.appendChild(td2);tr.appendChild(td3);tr.appendChild(td4);td5.appendChild(tr)}
new_table.appendChild(td5);elem[2].appendChild(new_table)}}
break;default:break}}
function initVolumeControl(){const volSlider=document.querySelector(".player-volume input[type='range']");const volLabel=document.querySelector(".player-volume .volume-label");if(!volSlider)
return;volSlider.addEventListener("input",()=>{const v=Number(volSlider.value)||0;if(volLabel)
volLabel.textContent=`${v}%`;styleRange(volSlider);wsSend({type:"cmd",cmd:"set_volume",value:v})})}
function initProgressControl(){const progSlider=document.querySelector(".player-progress input[type='range']");if(!progSlider)
return;progSlider.addEventListener("change",()=>{const v=Number(progSlider.value)||0;wsSend({type:"cmd",cmd:"seek_percent",value:v})})}
function initSettingsSave(){const saveBtn=document.querySelector('[data-action="save-settings"]');if(!saveBtn)
return;saveBtn.addEventListener("click",()=>{const deviceName=document.querySelector('input[name="device_name"]');const timezone=document.querySelector('input[name="timezone"]');const volumeCurve=document.querySelector('select[name="volume_curve"]');const payload={type:"settings.update",data:{device_name:deviceName?deviceName.value:undefined,timezone:timezone?timezone.value:undefined,volume_curve:volumeCurve?volumeCurve.value:undefined}};wsSend(payload)})}
let lastPlayback={position_ms:0,duration_ms:0,state:0};let lastUpdateTs=Date.now();let progressTimer=null;function updatePlayback(p){const titleEl=document.querySelector(".track-title");const artistEl=document.querySelector(".track-artist");const albumEl=document.querySelector(".track-album");const playBtn=document.querySelector(".btn.play");const artEl=document.querySelector(".cover-placeholder");const volSlider=document.querySelector(".player-volume input[type='range']");const volLabel=document.querySelector(".player-volume .volume-label");const progSlider=document.querySelector(".player-progress input[type='range']");const timeEls=document.querySelectorAll(".player-progress span");const src=document.querySelector(".stream-src");const qlty=document.querySelector(".stream-qlty");if(p.track){if(titleEl&&p.track.title)
titleEl.textContent=p.track.title;if(artistEl&&p.track.artist)
artistEl.textContent=p.track.artist;if(albumEl&&p.track.album)
albumEl.textContent=p.track.album;if(artEl){if(p.track.image){artEl.style.backgroundImage=`url('${p.track.image}')`;artEl.style.backgroundSize="cover";artEl.style.backgroundPosition="center";artEl.style.backgroundRepeat="no-repeat";artEl.textContent=""}else{(async()=>{var arturl=await fetchCoverFromMusicBrainz(p.track.artist,p.track.title);if(arturl){artEl.style.backgroundImage=`url('${arturl}')`;artEl.style.backgroundSize="cover";artEl.style.backgroundPosition="center";artEl.style.backgroundRepeat="no-repeat";artEl.textContent=""}})()}}}
if(typeof p.volume==="number"&&volSlider){volSlider.value=p.volume;styleRange(volSlider);if(volLabel)
volLabel.textContent=`${p.volume}%`}
if(typeof p.position_ms==="number"){lastPlayback.position_ms=p.position_ms}
if(typeof p.duration_ms==="number"){lastPlayback.duration_ms=p.duration_ms}
if(typeof p.state==="number"){lastPlayback.state=p.state;if(playBtn){playBtn.textContent=(p.state===1)?"pause":"play_arrow"}}
lastUpdateTs=Date.now();if(progSlider&&lastPlayback.duration_ms>0){const percent=(lastPlayback.position_ms/lastPlayback.duration_ms)*100;progSlider.value=Math.max(0,Math.min(100,percent));styleRange(progSlider);if(timeEls.length===2){timeEls[0].textContent=formatTime(lastPlayback.position_ms);timeEls[1].textContent=formatTime(lastPlayback.duration_ms)}}
if(p.src){src.textContent=p.src}
if(p.quality){qlty.textContent=p.quality}}
function formatTime(ms){const totalSec=Math.floor(ms/1000);const m=Math.floor(totalSec/60);const s=totalSec%60;return `${m.toString().padStart(1, "0")}:${s.toString().padStart(2, "0")}`}
function startProgressTimer(){if(progressTimer!==null)
return;progressTimer=setInterval(()=>{const progSlider=document.querySelector(".player-progress input[type='range']");if(!progSlider)
return;if(lastPlayback.state!==1)
return;if(!lastPlayback.duration_ms||lastPlayback.duration_ms<=0)
return;const now=Date.now();const delta=now-lastUpdateTs;if(delta<=0)
return;lastPlayback.position_ms+=delta;if(lastPlayback.position_ms>lastPlayback.duration_ms){lastPlayback.position_ms=lastPlayback.duration_ms}
lastUpdateTs=now;const percent=(lastPlayback.position_ms/lastPlayback.duration_ms)*100;progSlider.value=Math.max(0,Math.min(100,percent));styleRange(progSlider);const timeEls=document.querySelectorAll(".player-progress span");if(timeEls.length===2){timeEls[0].textContent=formatTime(lastPlayback.position_ms);timeEls[1].textContent=formatTime(lastPlayback.duration_ms)}},1000)}
async function fetchRadioStationsFromWeb(){const queryInput=document.querySelectorAll("#page-radio .radio-filters input[type='text']");const selects=document.querySelectorAll("#page-radio .radio-filters select");const countrySelect=selects[0]||null;radioState.query=queryInput[0]?queryInput[0].value.trim():"";radioState.genre=queryInput[1]?queryInput[1].value.trim():"";radioState.country=countrySelect?countrySelect.value||"":"";const params=new URLSearchParams();params.set("limit","50");params.set("hidebroken","true");if(radioState.query)
params.set("name",radioState.query);if(radioState.genre&&radioState.genre!=="All genres")
params.set("tagList",radioState.genre.toLowerCase());if(radioState.country&&radioState.country!=="All countries")
params.set("countrycode",radioState.country);const url=`${radioApiBase}/json/stations/search?${params.toString()}`;var xhr=new XMLHttpRequest();xhr.timeout=2000;xhr.onreadystatechange=function(){if(xhr.readyState===4){if(xhr.status===200){const results=JSON.parse(xhr.responseText).map((st)=>({name:st.name,url:st.url_resolved||st.url,country:st.countrycode||st.country,genre:(st.tags||"").split(",")[0]||"",homepage:st.homepage,favicon:st.favicon,bitrate:st.bitrate}));var favs=radioState.items.filter((st)=>st.favorite);for(let i=0;i<results.length;i++){favs.forEach((f)=>{if(f.url===results[i].url){results.splice(i,1)}})}
radioState.items=favs.concat(results);renderRadioStations()}else{const tbody=document.getElementById("radio-table-body");if(tbody){tbody.innerHTML=` <tr><td colspan="4">Error while searching radio stations.</td></tr>`}}}}
xhr.open("GET",url,!0);xhr.ontimeout=(e)=>{};xhr.send()}
function initRadioSearch(){const queryInput=document.querySelectorAll("#page-radio .radio-filters input[type='text']");const countrySelect=document.querySelector("#page-radio .radio-filters select");const searchBtn=document.querySelector('[data-action="radio-search"]');const tableBody=document.getElementById("radio-table-body");function triggerSearch(){fetchRadioStationsFromWeb()}
if(searchBtn){searchBtn.addEventListener("click",(e)=>{e.preventDefault();triggerSearch()})}
if(queryInput){queryInput[0].addEventListener("keydown",(e)=>{if(e.key==="Enter"){e.preventDefault();triggerSearch()}});queryInput[1].addEventListener("keydown",(e)=>{if(e.key==="Enter"){e.preventDefault();triggerSearch()}});const url=`${radioApiBase}/json/countrycodes`;var xhr=new XMLHttpRequest();xhr.timeout=2000;xhr.onreadystatechange=function(){if(xhr.readyState===4){if(xhr.status===200){const selects=document.querySelectorAll("#page-radio .radio-filters select");var countries=JSON.parse(xhr.responseText);countries.forEach((country)=>{const option=document.createElement("option");option.textContent=country.name;selects[0].appendChild(option)})}}}
xhr.open("GET",url,!0);xhr.ontimeout=(e)=>{};xhr.send()}
if(countrySelect){countrySelect.addEventListener("change",triggerSearch)}
if(tableBody){tableBody.addEventListener("click",(e)=>{const btn=e.target.closest("button[data-action]");if(!btn)
return;const action=btn.getAttribute("data-action");const row=btn.closest("tr");if(!row)
return;const url=row.getAttribute("data-url")||"";const name=row.getAttribute("data-name")||"";if(!url)
return;if(action==="radio-play"){wsSend({type:"radio.cmd",cmd:"play_station",station:{url,name}})}else if(action==="radio-save"){wsSend({type:"radio.cmd",cmd:"save_station",station:{url,name}})}else if(action==="radio-favorite"){wsSend({type:"radio.cmd",cmd:"remove_station",station:{url,name}})}})}
const addRadio=document.querySelectorAll('.radio-add .btn');const addRadioInputs=document.querySelectorAll(".radio-add input[type='text']");if(addRadio){addRadio[0].addEventListener("click",(e)=>{e.preventDefault();wsSend({type:"radio.cmd",cmd:"play_station",station:{name:addRadioInputs[0].value.trim(),url:addRadioInputs[1].value.trim()}})});addRadio[1].addEventListener("click",(e)=>{e.preventDefault();wsSend({type:"radio.cmd",cmd:"save_station",station:{name:addRadioInputs[0].value.trim(),url:addRadioInputs[1].value.trim()}})})}}
function init(){initNav();initSettingsTabs();initAllRanges();initVolumeControl();initProgressControl();initSettingsSave();initRadioSearch();connectWebSocket();startProgressTimer()}
if(document.readyState==="loading"){document.addEventListener("DOMContentLoaded",init)}else{init()}})()
)delim";

  static std::shared_ptr<AudioControl> g_audio;
  static std::unique_ptr<AudioControl::FeedControl> g_feed;
  static bell::BellHTTPServer* g_http = nullptr;
  static OnWsMessage            on_ws_msg_ = nullptr;
  static std::vector<mg_connection*> g_ws_conn{};
  static std::mutex            g_ws_mtx;

  // --- Small helper to read a whole file into memory ---
  static bool readFile(const char* path, std::string& out) {
    FILE* f = fopen(path, "rb");
    if (!f) {
      BELL_LOG(error, "WebUI", "Failed to open file: %s", path);
      return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return false; }

    out.resize(sz);
    if (sz > 0 && fread(&out[0], 1, sz, f) != (size_t)sz) {
      fclose(f);
      return false;
    }
    fclose(f);
    return true;
  }

  // --- Helper to build an HTTP response from a string ---
  static std::unique_ptr<bell::BellHTTPServer::HTTPResponse>
    makeTextResponse(const std::string& body, const std::string& contentType) {
    auto resp = std::make_unique<bell::BellHTTPServer::HTTPResponse>();
    resp->status = 200;
    resp->bodySize = body.size();
    resp->body = (uint8_t*)malloc(body.size());
    if (!resp->body && body.size() > 0) {
      // allocation failed; fall back to empty response
      resp->bodySize = 0;
    }
    else if (resp->bodySize > 0) {
      memcpy(resp->body, body.data(), body.size());
    }
    resp->headers["Content-Type"] = contentType;
    return resp;
  }

  // --- Static file handler (index.html, css, js) ---
  static std::unique_ptr<bell::BellHTTPServer::HTTPResponse>
    serveStaticFile(const char* path, const char* mime) {
    std::string body;
    if (!readFile(path, body)) {
      // 404
      auto notFound = std::make_unique<bell::BellHTTPServer::HTTPResponse>();
      notFound->status = 404;
      notFound->body = nullptr;
      notFound->bodySize = 0;
      notFound->headers["Content-Type"] = "text/plain; charset=utf-8";
      return notFound;
    }
    return makeTextResponse(body, mime);
  }
  static std::unique_ptr<bell::BellHTTPServer::HTTPResponse>
    serveStatic(const char* body, const char* mime) {
    return makeTextResponse(body, mime);
  }
  static std::string g_status_cache = "";
  // --- JSON send over websocket---
  static bool isConnected() { return g_ws_conn.size() > 0; }
  static void wsSendJson(const std::string& json, mg_connection* conn = nullptr) {
    std::lock_guard<std::mutex> lk(g_ws_mtx);
    if (!g_ws_conn.size()) {
      BELL_LOG(error, "WebUI", "WS not connected");
      return;
    }
    auto send_one = [&](mg_connection* c) -> bool {
      int rc = mg_websocket_write(
        c,
        MG_WEBSOCKET_OPCODE_TEXT,
        json.c_str(),
        json.size());
      if (rc <= 0) {
        BELL_LOG(info, "WebUI", "WS write failed, closing conn %p", c);
        mg_close_connection(c);
        return false;  // remove from list
      }
      return true;
      };
    if (conn) {
      // Target a specific connection and prune it on failure.
      for (auto it = g_ws_conn.begin(); it != g_ws_conn.end(); ++it) {
        if (*it == conn) {
          if (!send_one(conn)) {
            mg_close_connection(conn);
            g_ws_conn.erase(it);
          }
          break;
        }
      }
    }
    else {
      // Broadcast and prune any that fail.
      for (auto it = g_ws_conn.begin(); it != g_ws_conn.end(); ) {
        mg_connection* c = *it;
        if (!send_one(c)) {
          mg_close_connection(c);
          it = g_ws_conn.erase(it);
        }
        else {
          ++it;
        }
      }
    }
  }
  // --- JSON send state over websocket and save to cache---
  static void wsSendJsonStatus(const std::string& json) {
    if (json == g_status_cache) {
      // no change
      return;
    }
    g_status_cache = json;
    wsSendJson(json);
  }

  // --- WebSocket state handler (connect / ready / closed) ---
  static void wsStateHandler(mg_connection* conn,
    bell::BellHTTPServer::WSState st) {
    switch (st) {
    case bell::BellHTTPServer::WSState::CONNECTED:
      BELL_LOG(info, "WebUI", "WS CONNECTED");
      break;
    case bell::BellHTTPServer::WSState::READY: {
      BELL_LOG(info, "WebUI", "WS READY");
      {
        std::lock_guard<std::mutex> lk(g_ws_mtx);
        if (g_ws_conn.size() > 3) {
          mg_close_connection(g_ws_conn[0]);
          g_ws_conn.erase(g_ws_conn.begin());
        }
        g_ws_conn.push_back(conn);
      }
      // send initial state
      if (!g_status_cache.empty()) {
        wsSendJson(g_status_cache, conn);
      }
      on_ws_msg_(conn, nullptr, 0);
      break;
    }
    case bell::BellHTTPServer::WSState::CLOSED: {
      BELL_LOG(info, "WebUI", "WS CLOSED");
      std::lock_guard<std::mutex> lk(g_ws_mtx);
      if (!g_ws_conn.size()) return;
      for (auto it = g_ws_conn.begin(); it != g_ws_conn.end(); ++it) {
        if (*it == conn) {
          g_ws_conn.erase(it);
          break;
        }
      }
      mg_close_connection(conn);
      break;
    }
    }
  }

  // --- HTTP handlers ---------------------------------------------------------

  // GET /  → index.html
  static std::unique_ptr<bell::BellHTTPServer::HTTPResponse>
    handleRoot(struct mg_connection* conn) {
    (void)conn;
    return serveStatic(TEXT_INDEX, "text/html; charset=utf-8");
  }

  // GET /style.css
  static std::unique_ptr<bell::BellHTTPServer::HTTPResponse>
    handleCSS(struct mg_connection* conn) {
    (void)conn;
    return serveStatic(TEXT_CSS, "text/css; charset=utf-8");
  }

  // GET /app.js
  static std::unique_ptr<bell::BellHTTPServer::HTTPResponse>
    handleJS(struct mg_connection* conn) {
    (void)conn;
    return serveStatic(TEXT_JS, "application/javascript; charset=utf-8");
  }

  // --- Public init function you can call from app_main -----------------------

  void WebUI_start(int port, OnWsMessage on_ws_msg__) {
    static bell::BellHTTPServer server(port, {
        {"thread_stack_size", "12288"},
        {"num_threads","3"},
        {"prespawn_threads", "2"},
        {"connection_queue", "5"},
      });
    g_http = &server;
    on_ws_msg_ = on_ws_msg__;
    // HTTP routes
    server.registerGet("/", handleRoot);
    server.registerGet("/index.html", handleRoot);
    server.registerGet("/style.css", handleCSS);
    server.registerGet("/app.js", handleJS);

    // WebSocket endpoint
    server.registerWS("/ws", on_ws_msg_, wsStateHandler);

    BELL_LOG(info, "WebUI", "HTTP/WebSocket UI started on port %d", port);
  }
}