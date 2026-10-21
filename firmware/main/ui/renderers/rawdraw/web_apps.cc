/**
 * @file web_apps.cc
 * @brief Device web UI: home page (app launcher) and weather settings. See web_apps.h.
 */

#include "web_apps.h"

#include "common/ai_chat.h"
#include "common/token_board.h"
#include "common/chinese_calendar.h"
#include "common/notice_api.h"
#include "common/reminders.h"
#include "common/weather_api.h"

#include <cJSON.h>
#include <esp_log.h>

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace rawdraw {
namespace web {

namespace {

constexpr const char* kTag = "WebApps";

const char kStyle[] = R"CSS(
*{box-sizing:border-box}body{margin:0;background:#ece8dc;color:#171717;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;font-size:13px}
.app{max-width:520px;margin:0 auto;padding:12px}.top{display:flex;align-items:center;justify-content:space-between;margin-bottom:10px}
.brand{font-weight:800;font-size:17px}a{color:inherit}.muted{color:#555}
.panel{background:#fff;border:2px solid #111;border-radius:6px;box-shadow:3px 3px 0 #111;margin-bottom:12px;padding:12px}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.btn{border:2px solid #111;background:#ff3b30;color:#fff;border-radius:5px;padding:8px 12px;font-weight:800;font-size:13px;box-shadow:2px 2px 0 #111;cursor:pointer}
.btn.yellow{background:#ffd900;color:#111}.btn.secondary{background:#fff;color:#111}.btn:disabled{opacity:.5}
input[type=text]{flex:1;min-width:0;padding:8px;border:1px solid #111;font-size:14px}
.card{display:block;text-decoration:none;color:inherit}.card .t{font-weight:800;font-size:16px}
.city{display:block;width:100%;text-align:left;background:#fff;border:1px solid #111;padding:8px;margin-top:6px;border-radius:4px;font-size:14px;cursor:pointer}
.city.sel{background:#ffd900}
)CSS";

const char kHomeHtml[] = R"HTML(<!DOCTYPE html><html><head><meta charset="UTF-8"><title>墨水屏设置</title>
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1"><style>%STYLE%</style></head><body><main class="app">
<div class="top"><div><div class="brand">墨水屏设置</div><div class="muted" id="ep"></div></div></div>
<a class="panel card" href="/album"><div class="t">相册 · 传图</div><div class="muted">上传照片、填写名字、管理设备相册和轮播周期</div></a>
<a class="panel card" href="/weather"><div class="t">天气</div><div class="muted" id="wsum">管理最多 5 个城市和数据有效期</div></a>
<a class="panel card" href="/calendar"><div class="t">日历 · 提醒事项</div><div class="muted" id="csum">为日期添加提醒，设备日历上会显示铃铛图标</div></a>
<a class="panel card" href="/ai"><div class="t">AI Key设置</div><div class="muted">填写阿里云百炼的 API Key，AI 问答和 AI 画图共用（使用都在设备上用语音完成）</div></a>
<a class="panel card" href="/tokenboard"><div class="t">Token 看板</div><div class="muted">AI 余额和费用：阿里百炼、DeepSeek 的密钥和刷新频率</div></a>
</main><script>
fetch('/status').then(r=>r.json()).then(j=>{document.getElementById('ep').textContent=j.ip}).catch(()=>{});
fetch('/api/reminders').then(r=>r.json()).then(j=>{const n=Object.keys(j.reminders).length;if(n)document.getElementById('csum').textContent='共 '+n+' 天有提醒'}).catch(()=>{});
fetch('/api/weather').then(r=>r.json()).then(j=>{document.getElementById('wsum').textContent=j.cities[j.selected].name+' · 共 '+j.cities.length+' 个城市 · '+j.interval+' 分钟有效'}).catch(()=>{});
</script></body></html>)HTML";

const char kWeatherHtml[] = R"HTML(<!DOCTYPE html><html><head><meta charset="UTF-8"><title>天气设置</title>
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1"><style>%STYLE%
.item{display:flex;align-items:center;gap:8px;border:1px solid #111;border-radius:4px;padding:8px;margin-top:6px}.item.cur{background:#ffd900}.item .n{flex:1;font-size:14px}
.item button{border:1px solid #111;background:#fff;border-radius:4px;padding:5px 8px;cursor:pointer}</style></head><body><main class="app">
<div class="top"><div><div class="brand">天气设置</div><div class="muted">数据来源 Open-Meteo（免费，无需 Key）</div></div><a class="btn secondary" href="/" style="text-decoration:none">首页</a></div>
<section class="panel"><b>当前天气</b><div id="cur" class="muted" style="margin-top:6px">读取中...</div></section>
<section class="panel"><b>我的城市 <span class="muted" id="cnt"></span></b><div class="muted" style="margin-top:4px">设备天气页按 UP/DOWN 切换这些城市；点"设为当前"选择开机显示的城市。</div><div id="list"></div></section>
<section class="panel"><b>添加城市</b>
<div class="row" style="margin-top:8px"><input type="text" id="q" placeholder="输入城市名，如 杭州 / Shanghai"><button class="btn yellow" id="search">搜索</button></div>
<div id="results"></div></section>
<section class="panel"><b>数据有效期</b> <span class="muted">进入天气页时，数据超过这个时间才重新获取；设备不会在后台定时刷新</span>
<div class="row" style="margin-top:8px"><label><input name="iv" type="radio" value="5">5 分钟</label><label><input name="iv" type="radio" value="10">10 分钟</label><label><input name="iv" type="radio" value="30" checked>30 分钟</label></div></section>
<div class="row"><button class="btn" id="save">保存并刷新</button><span id="msg" class="muted"></span></div>
</main><script>
const $=id=>document.getElementById(id),MAX=5;let cfg={cities:[],selected:0,interval:30};
function msg(t){$('msg').textContent=t}
function render(){$('cnt').textContent=cfg.cities.length+'/'+MAX;$('list').innerHTML='';
 cfg.cities.forEach((c,i)=>{const d=document.createElement('div');d.className='item'+(i===cfg.selected?' cur':'');
  d.innerHTML='<span class="n"></span>';d.firstChild.textContent=c.name+(c.admin?' · '+c.admin:'')+(i===cfg.selected?'（当前）':'');
  if(i!==cfg.selected){const b=document.createElement('button');b.textContent='设为当前';b.onclick=()=>{cfg.selected=i;render()};d.appendChild(b)}
  if(cfg.cities.length>1){const x=document.createElement('button');x.textContent='删除';x.onclick=()=>{cfg.cities.splice(i,1);if(cfg.selected>=i&&cfg.selected>0)cfg.selected--;render()};d.appendChild(x)}
  $('list').appendChild(d)})}
function show(j){cfg={cities:j.cities,selected:j.selected,interval:j.interval};document.querySelectorAll('input[name=iv]').forEach(r=>r.checked=(+r.value===j.interval));render();
 $('cur').textContent=j.last?(j.last.city+' '+j.last.temp+'°C '+j.last.text+'（更新 '+j.last.time+'）'):'尚未获取到天气（需设备已联网）'}
function load(){return fetch('/api/weather').then(r=>r.json()).then(show).catch(()=>{$('cur').textContent='读取失败'})}
function add(c){if(cfg.cities.length>=MAX){msg('最多 '+MAX+' 个城市');return}
 if(cfg.cities.some(x=>x.lat===c.lat&&x.lon===c.lon)){msg('该城市已添加');return}
 cfg.cities.push({name:c.name,admin:c.admin,lat:c.lat,lon:c.lon});msg('已添加，记得点"保存并刷新"');render()}
$('search').onclick=async()=>{const q=$('q').value.trim();if(!q)return;$('results').innerHTML='<div class="muted" style="margin-top:6px">搜索中...</div>';
 try{const j=await (await fetch('/api/weather/search?q='+encodeURIComponent(q))).json();
  if(!j.success){$('results').innerHTML='<div class="muted" style="margin-top:6px">搜索失败（设备需要联网）</div>';return}
  $('results').innerHTML=j.cities.length?'':'<div class="muted" style="margin-top:6px">没有找到</div>';
  for(const c of j.cities){const d=document.createElement('div');d.className='item';d.innerHTML='<span class="n"></span><button>添加</button>';
   d.firstChild.textContent=c.name+(c.admin?' · '+c.admin:'');d.lastChild.onclick=()=>add(c);$('results').appendChild(d)}
 }catch(e){$('results').innerHTML='<div class="muted" style="margin-top:6px">网络错误</div>'}};
$('q').onkeydown=e=>{if(e.key==='Enter')$('search').click()};
$('save').onclick=async()=>{const iv=+document.querySelector('input[name=iv]:checked').value;$('save').disabled=true;msg('保存中...');
 try{const r=await fetch('/api/weather',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({cities:cfg.cities,selected:cfg.selected,interval:iv})});const j=await r.json();
  msg(j.success?'已保存，设备正在刷新天气':'保存失败: '+(j.error||'未知错误'));if(j.success){show(j);setTimeout(load,6000)}}catch(e){msg('网络错误')}$('save').disabled=false};
load();
</script></body></html>)HTML";

const char kCalendarHtml[] = R"HTML(<!DOCTYPE html><html><head><meta charset="UTF-8"><title>日历提醒</title>
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1"><style>%STYLE%
.nav{display:flex;align-items:center;justify-content:space-between;margin-bottom:8px}.nav b{font-size:16px}
.nav button{border:2px solid #111;background:#fff;border-radius:5px;padding:4px 12px;font-size:15px;font-weight:800;cursor:pointer}
.grid{display:grid;grid-template-columns:repeat(7,1fr);gap:3px}.wd{text-align:center;background:#111;color:#fff;font-size:12px;padding:3px 0;border-radius:3px}.wd.we{color:#ffd900}
.day{position:relative;height:44px;border:1px solid #bbb;border-radius:4px;background:#fff;font-size:14px;cursor:pointer;padding:0}
.day.we{color:#e0201b}.day.today{background:#ffd900;border-color:#ffd900;color:#111;font-weight:800}.day.sel{outline:3px solid #e0201b;outline-offset:1px}.day.off{visibility:hidden}
.day .bell{position:absolute;top:1px;right:3px;font-size:11px}.day .fest{position:absolute;bottom:2px;left:0;right:0;font-size:10px;color:#e0a800;line-height:1}.day.today .fest{color:#111}.day.today .bell{filter:grayscale(1)}
.legend{font-size:11px;color:#555;margin-top:8px}.legend i{display:inline-block;width:10px;height:10px;border-radius:2px;margin:0 3px 0 8px;vertical-align:-1px}.item{display:flex;gap:6px;margin-top:6px}.item input{flex:1;min-width:0;padding:8px;border:1px solid #111;font-size:14px}
.item button{border:1px solid #111;background:#fff;border-radius:4px;padding:5px 9px;cursor:pointer}
.up{display:block;width:100%;text-align:left;background:#fff;border:1px solid #111;padding:8px;margin-top:6px;border-radius:4px;font-size:13px;cursor:pointer}
</style></head><body><main class="app">
<div class="top"><div><div class="brand">日历提醒</div><div class="muted">设备上：UP/DOWN 翻月，BOOT 回本月 / 看今日提醒</div></div><a class="btn secondary" href="/" style="text-decoration:none">首页</a></div>
<section class="panel"><div class="nav"><button id="prev">‹</button><b id="ym"></b><span><button id="now" style="font-size:12px">今天</button> <button id="next">›</button></span></div>
<div class="grid" id="grid"></div>
<div class="legend"><i style="background:#ffd900"></i>今天<i style="background:#e0a800"></i>节日名<i style="background:#e0201b"></i>周末（数字）<span style="margin-left:8px">🔔 有提醒</span></div></section>
<section class="panel"><b id="dt"></b><div id="items"></div>
<div class="row" style="margin-top:10px"><button class="btn secondary" id="add">添加一条</button><button class="btn" id="save">保存</button><span id="msg" class="muted"></span></div></section>
<section class="panel"><b>即将到来的提醒</b><div id="upcoming"></div></section>
<section class="panel"><b>最新提醒（日历底部一行）</b><div class="muted">进入日历应用时才从服务器拉取；这里是数据有效期，没过期时再次进入不重复拉取</div>
<div class="row" style="margin-top:8px"><label><input type="radio" name="niv" value="10"> 10 分钟</label> <label><input type="radio" name="niv" value="30"> 30 分钟</label> <label><input type="radio" name="niv" value="60"> 1 小时</label> <label><input type="radio" name="niv" value="0"> 不拉取</label>
<button class="btn" id="nsave">保存</button><span id="nmsg" class="muted"></span></div></section>
</main><script>
const $=id=>document.getElementById(id),WD=['日','一','二','三','四','五','六'];
const pad=n=>String(n).padStart(2,'0'),key=(y,m,d)=>y+'-'+pad(m)+'-'+pad(d);
let fest={},festYears={},data={},lim={perDay:6,textBytes:96},now=new Date(),vy=now.getFullYear(),vm=now.getMonth()+1,sel=key(now.getFullYear(),now.getMonth()+1,now.getDate()),draft=[];
const todayKey=sel;
function msg(t){$('msg').textContent=t}
function select(k){sel=k;draft=(data[k]||[]).slice();msg('');renderAll()}
function loadFest(y){if(festYears[y])return;festYears[y]=1;fetch('/api/festivals?year='+y).then(r=>r.json()).then(j=>{if(j.success){Object.assign(fest,j.festivals);renderAll()}}).catch(()=>{festYears[y]=0})}
function renderGrid(){loadFest(vy);$('ym').textContent=vy+'年'+vm+'月';const g=$('grid');g.innerHTML='';
 WD.forEach((w,i)=>{const e=document.createElement('div');e.className='wd'+(i===0||i===6?' we':'');e.textContent=w;g.appendChild(e)});
 const first=new Date(vy,vm-1,1).getDay(),days=new Date(vy,vm,0).getDate();
 for(let i=0;i<first;i++){const e=document.createElement('div');e.className='day off';g.appendChild(e)}
 for(let d=1;d<=days;d++){const k=key(vy,vm,d),b=document.createElement('button');
  const wd=(first+d-1)%7;b.className='day'+(wd===0||wd===6?' we':'')+(k===todayKey?' today':'')+(k===sel?' sel':'');b.textContent=d;
  if(fest[k]){const f=document.createElement('span');f.className='fest';f.textContent=fest[k][0];b.appendChild(f)}
  if(data[k]){const s=document.createElement('span');s.className='bell';s.textContent='🔔';b.appendChild(s)}
  b.onclick=()=>select(k);g.appendChild(b)}}
function renderEditor(){const p=sel.split('-').map(Number);
 $('dt').textContent=p[0]+'年'+p[1]+'月'+p[2]+'日 周'+WD[new Date(p[0],p[1]-1,p[2]).getDay()]+(sel===todayKey?'（今天）':'')+(fest[sel]?' · '+fest[sel].join(' '):'');
 const box=$('items');box.innerHTML='';
 if(!draft.length){const e=document.createElement('div');e.className='muted';e.style.marginTop='6px';e.textContent='这一天还没有提醒';box.appendChild(e)}
 draft.forEach((t,i)=>{const d=document.createElement('div');d.className='item';
  const inp=document.createElement('input');inp.type='text';inp.value=t;inp.placeholder='提醒内容，如 09:30 开会';inp.oninput=()=>{draft[i]=inp.value};
  const x=document.createElement('button');x.textContent='删除';x.onclick=()=>{draft.splice(i,1);renderEditor()};
  d.appendChild(inp);d.appendChild(x);box.appendChild(d)});
 $('add').disabled=draft.length>=lim.perDay}
function renderUpcoming(){const box=$('upcoming');box.innerHTML='';
 const ks=Object.keys(data).filter(k=>k>=todayKey).sort();
 if(!ks.length){const e=document.createElement('div');e.className='muted';e.style.marginTop='6px';e.textContent='暂无';box.appendChild(e)}
 ks.forEach(k=>{const b=document.createElement('button');b.className='up';b.textContent=k+'  '+data[k].join('；');
  b.onclick=()=>{const p=k.split('-').map(Number);vy=p[0];vm=p[1];select(k)};box.appendChild(b)})}
function renderAll(){renderGrid();renderEditor();renderUpcoming()}
function apply(j){data=j.reminders;if(j.limits)lim=j.limits}
$('prev').onclick=()=>{vm--;if(vm<1){vm=12;vy--}renderGrid()};
$('next').onclick=()=>{vm++;if(vm>12){vm=1;vy++}renderGrid()};
$('now').onclick=()=>{vy=now.getFullYear();vm=now.getMonth()+1;select(todayKey)};
$('add').onclick=()=>{if(draft.length<lim.perDay){draft.push('');renderEditor();const l=document.querySelectorAll('#items input');l[l.length-1].focus()}};
const ERR={bad_date:'日期无效（支持 2000–2099 年）',too_many:'每天提醒条数已达上限',text_too_long:'有一条太长了',bad_text:'内容含有无效字符',too_many_dates:'有提醒的日期太多了，请先删除一些',save_failed:'设备保存失败',bad_json:'请求无效'};
$('save').onclick=async()=>{const items=draft.map(s=>s.trim()).filter(Boolean),enc=new TextEncoder();
 if(items.some(s=>enc.encode(s).length>lim.textBytes)){msg('每条最长 '+lim.textBytes+' 字节（约 '+Math.floor(lim.textBytes/3)+' 个汉字）');return}
 $('save').disabled=true;msg('保存中...');
 try{const r=await fetch('/api/reminders',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({date:sel,items})});const j=await r.json();
  if(j.success){apply(j);draft=(data[sel]||[]).slice();msg('已保存'+(items.length?'':'（该日提醒已清空）'));renderAll()}else msg('保存失败：'+(ERR[j.error]||j.error||'未知错误'))}
 catch(e){msg('网络错误')}$('save').disabled=false};
fetch('/api/notice').then(r=>r.json()).then(j=>{document.querySelectorAll('input[name=niv]').forEach(r=>r.checked=(+r.value===j.interval))}).catch(()=>{$('nmsg').textContent='读取失败'});
$('nsave').onclick=async()=>{const v=+(document.querySelector('input[name=niv]:checked')||{}).value;try{const r=await fetch('/api/notice',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({interval:v})});const j=await r.json();$('nmsg').textContent=j.success?'已保存':'保存失败'}catch(e){$('nmsg').textContent='保存失败'}};
fetch('/api/reminders').then(r=>r.json()).then(j=>{apply(j);draft=(data[sel]||[]).slice();renderAll()}).catch(()=>{renderAll();msg('读取失败（设备离线？）')});
</script></body></html>)HTML";

const char kAiHtml[] =R"HTML(<!DOCTYPE html><html><head><meta charset="UTF-8"><title>AI Key设置</title>
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1"><style>%STYLE%</style></head><body><main class="app">
<div class="top"><div><div class="brand">AI Key设置</div><div class="muted">AI 问答和 AI 画图共用这个 Key；使用都在设备上：进入对应应用，长按 UP 说话</div></div><a class="btn secondary" href="/" style="text-decoration:none">首页</a></div>
<section class="panel"><b>阿里云百炼 API Key</b> <span class="muted" id="keyst"></span><div class="muted" style="margin:4px 0 8px">同一个 Key 同时用于语音识别（Qwen3-ASR）、AI 问答（Qwen-Flash）和 AI 画图（Qwen-Image）。保存在设备上，不会再显示出来。</div>
<div class="row"><input type="text" id="key" placeholder="sk-..." autocomplete="off"><button class="btn yellow" id="savekey">保存</button></div></section>
<div id="msg" class="muted"></div>
</main><script>
const $=id=>document.getElementById(id);
async function api(path,body){const r=await fetch(path,body?{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)}:undefined);return r.json()}
async function load(){try{const j=await api('/api/ai/state');$('keyst').textContent=j.hasKey?'（已设置）':'（未设置）'}catch(e){$('msg').textContent='读取失败（设备离线？）'}}
$('savekey').onclick=async()=>{const j=await api('/api/ai/key',{key:$('key').value});if(j.success){$('key').value='';$('msg').textContent='Key 已保存'}else $('msg').textContent=j.error||'Key 无效';load()};
load();
</script></body></html>)HTML";

const char kTokenBoardHtml[] =R"HTML(<!DOCTYPE html><html><head><meta charset="UTF-8"><title>Token 看板设置</title>
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1"><style>%STYLE%</style></head><body><main class="app">
<div class="top"><div><div class="brand">Token 看板设置</div><div class="muted">设备上的“Token 看板”显示余额和费用（百炼有按模型明细，DeepSeek 只有余额）；在设备上按 BOOT 刷新，UP/DOWN 切换提供商</div></div><a class="btn secondary" href="/" style="text-decoration:none">首页</a></div>
<section class="panel"><b>阿里云 AccessKey</b> <span class="muted" id="keyst"></span><div class="muted" style="margin:4px 0 8px">用 RAM 子账号的 AccessKey，只授权 AliyunBSSReadOnlyAccess（费用中心只读），不要用主账号。保存在设备上，不会再显示出来。目前只支持中国站（aliyun.com）。</div>
<div class="row" style="margin-bottom:6px"><input type="text" id="kid" placeholder="AccessKey ID（LTAI...）" autocomplete="off"></div>
<div class="row"><input type="text" id="ksec" placeholder="AccessKey Secret" autocomplete="off"><button class="btn yellow" id="save">保存</button></div></section>
<section class="panel"><b>DeepSeek API Key</b> <span class="muted" id="dskeyst"></span><div class="muted" style="margin:4px 0 8px">在 DeepSeek 开放平台（platform.deepseek.com）的 API keys 页面创建，用来查余额；Key 以 sk- 开头。保存在设备上，不会再显示出来。DeepSeek 没有开放用量接口，所以只显示余额。</div>
<div class="row"><input type="text" id="dskey" placeholder="sk-..." autocomplete="off"><button class="btn yellow" id="savedskey">保存</button></div></section>
<section class="panel"><b>刷新频率</b><div class="muted" style="margin:4px 0 8px">进入看板时，数据比这更旧才会重新获取；按 BOOT 随时立即刷新。设备不会在后台定时拉取。</div>
<label><input type="radio" name="iv" value="10"> 10 分钟</label> &nbsp; <label><input type="radio" name="iv" value="30"> 30 分钟</label> &nbsp; <label><input type="radio" name="iv" value="60"> 1 小时</label>
<div style="margin-top:8px"><button class="btn yellow" id="saveiv">保存</button></div></section>
<div id="msg" class="muted"></div>
</main><script>
const $=id=>document.getElementById(id);
async function api(path,body){const r=await fetch(path,body?{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)}:undefined);return r.json()}
async function load(){try{const j=await api('/api/tokenboard/state');$('keyst').textContent=j.hasKey?'（已设置）':'（未设置）';$('dskeyst').textContent=j.hasDeepseekKey?'（已设置）':'（未设置）';document.querySelectorAll('input[name=iv]').forEach(r=>r.checked=(+r.value===j.interval))}catch(e){$('msg').textContent='读取失败（设备离线？）'}}
$('savedskey').onclick=async()=>{const j=await api('/api/tokenboard/deepseek',{key:$('dskey').value});if(j.success){$('dskey').value='';$('msg').textContent='DeepSeek Key 已保存'}else $('msg').textContent=j.error||'Key 无效';load()};
$('saveiv').onclick=async()=>{const v=+(document.querySelector('input[name=iv]:checked')||{}).value;const j=await api('/api/tokenboard/interval',{interval:v});$('msg').textContent=j.success?'刷新频率已保存':(j.error||'保存失败')};
$('save').onclick=async()=>{const j=await api('/api/tokenboard/key',{id:$('kid').value,secret:$('ksec').value});if(j.success){$('kid').value='';$('ksec').value='';$('msg').textContent='已保存，到设备上打开 Token 看板'}else $('msg').textContent=j.error||'保存失败';load()};
load();
</script></body></html>)HTML";

void CloseSession(httpd_req_t* req) {
    if (!req || !req->handle) return;
    const int sockfd = httpd_req_to_sockfd(req);
    if (sockfd >= 0) httpd_sess_trigger_close(req->handle, sockfd);
}

// Pages embed %STYLE% so the stylesheet exists once in flash.
esp_err_t SendPage(httpd_req_t* req, const char* html) {
    std::string page(html);
    const size_t pos = page.find("%STYLE%");
    if (pos != std::string::npos) page.replace(pos, 7, kStyle);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    const esp_err_t ret = httpd_resp_send(req, page.c_str(), static_cast<ssize_t>(page.size()));
    CloseSession(req);
    return ret;
}

esp_err_t SendJsonTree(httpd_req_t* req, cJSON* root) {
    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    const esp_err_t ret = httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    free(text);
    CloseSession(req);
    return ret;
}

esp_err_t SendError(httpd_req_t* req, const char* code) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", false);
    cJSON_AddStringToObject(root, "error", code);
    return SendJsonTree(req, root);
}

cJSON* ReadBody(httpd_req_t* req, size_t max_len = 2048) {
    if (req->content_len == 0 || req->content_len > max_len) return nullptr;
    std::string buf(req->content_len, '\0');
    size_t got = 0;
    while (got < req->content_len) {
        const int n = httpd_req_recv(req, &buf[got], req->content_len - got);
        if (n <= 0) return nullptr;
        got += static_cast<size_t>(n);
    }
    return cJSON_Parse(buf.c_str());
}

}  // namespace

std::string UrlDecode(const char* in) {
    std::string out;
    for (size_t i = 0; in[i]; ++i) {
        if (in[i] == '%' && isxdigit(static_cast<unsigned char>(in[i + 1])) &&
            isxdigit(static_cast<unsigned char>(in[i + 2]))) {
            const char hex[3] = {in[i + 1], in[i + 2], 0};
            out += static_cast<char>(strtol(hex, nullptr, 16));
            i += 2;
        } else {
            out += in[i] == '+' ? ' ' : in[i];
        }
    }
    return out;
}

esp_err_t HomeHandler(httpd_req_t* req) { return SendPage(req, kHomeHtml); }

esp_err_t WeatherPageHandler(httpd_req_t* req) { return SendPage(req, kWeatherHtml); }

esp_err_t WeatherConfigHandler(httpd_req_t* req) {
    if (req->method == HTTP_POST) {
        cJSON* body = ReadBody(req);
        if (!body) return SendError(req, "bad_json");
        WeatherConfig config = weather_api_get_config();
        cJSON* cities = cJSON_GetObjectItemCaseSensitive(body, "cities");
        if (cJSON_IsArray(cities)) {
            config.cities.clear();
            cJSON* item = nullptr;
            cJSON_ArrayForEach(item, cities) {
                auto text = [item](const char* key) -> std::string {
                    cJSON* v = cJSON_GetObjectItemCaseSensitive(item, key);
                    return cJSON_IsString(v) ? v->valuestring : "";
                };
                config.cities.push_back({text("name"), text("admin"), text("lat"), text("lon")});
                if (config.cities.size() > kMaxWeatherCities) break;  // rejected by validation below
            }
        }
        cJSON* selected = cJSON_GetObjectItemCaseSensitive(body, "selected");
        cJSON* interval = cJSON_GetObjectItemCaseSensitive(body, "interval");
        if (cJSON_IsNumber(selected)) config.selected = selected->valueint;
        if (cJSON_IsNumber(interval)) config.interval_min = interval->valueint;
        cJSON_Delete(body);
        if (!weather_api_set_config(config)) {
            ESP_LOGW(kTag, "Rejected weather config");
            return SendError(req, "invalid_config");
        }
    }

    const WeatherConfig config = weather_api_get_config();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON* list = cJSON_AddArrayToObject(root, "cities");
    for (const auto& c : config.cities) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", c.name.c_str());
        cJSON_AddStringToObject(item, "admin", c.admin.c_str());
        cJSON_AddStringToObject(item, "lat", c.lat.c_str());
        cJSON_AddStringToObject(item, "lon", c.lon.c_str());
        cJSON_AddItemToArray(list, item);
    }
    cJSON_AddNumberToObject(root, "selected", config.selected);
    cJSON_AddNumberToObject(root, "interval", config.interval_min);
    WeatherData last;
    if (weather_api_get_last_data(&last)) {
        cJSON* l = cJSON_AddObjectToObject(root, "last");
        cJSON_AddStringToObject(l, "city", last.city.c_str());
        cJSON_AddStringToObject(l, "temp", last.temp.c_str());
        cJSON_AddStringToObject(l, "text", last.weather_text.c_str());
        cJSON_AddStringToObject(l, "time", last.update_time.c_str());
    } else {
        cJSON_AddNullToObject(root, "last");
    }
    return SendJsonTree(req, root);
}

esp_err_t NoticeConfigHandler(httpd_req_t* req) {
    if (req->method == HTTP_POST) {
        cJSON* body = ReadBody(req);
        if (!body) return SendError(req, "bad_json");
        cJSON* interval = cJSON_GetObjectItemCaseSensitive(body, "interval");
        const bool ok = cJSON_IsNumber(interval) && notice_api_set_interval(interval->valueint);
        cJSON_Delete(body);
        if (!ok) return SendError(req, "invalid_interval");
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddNumberToObject(root, "interval", notice_api_get_interval());
    return SendJsonTree(req, root);
}

esp_err_t WeatherSearchHandler(httpd_req_t* req) {
    char query[160] = {};
    char encoded[128] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "q", encoded, sizeof(encoded)) != ESP_OK || !encoded[0]) {
        return SendError(req, "missing_query");
    }
    // The value is forwarded into a URL as-is, so accept only percent-encoded/URL-safe characters.
    for (const char* p = encoded; *p; ++p) {
        if (!isalnum(static_cast<unsigned char>(*p)) && !strchr("%+-._~", *p)) return SendError(req, "bad_query");
    }
    std::vector<WeatherCity> cities;
    if (!weather_api_search_city(encoded, &cities)) return SendError(req, "search_failed");

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON* list = cJSON_AddArrayToObject(root, "cities");
    for (const auto& c : cities) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", c.name.c_str());
        cJSON_AddStringToObject(item, "admin", c.admin.c_str());
        cJSON_AddStringToObject(item, "lat", c.lat.c_str());
        cJSON_AddStringToObject(item, "lon", c.lon.c_str());
        cJSON_AddItemToArray(list, item);
    }
    return SendJsonTree(req, root);
}

// ---- Calendar reminders -------------------------------------------------------------------------

esp_err_t CalendarPageHandler(httpd_req_t* req) { return SendPage(req, kCalendarHtml); }

esp_err_t RemindersHandler(httpd_req_t* req) {
    auto& store = reminders::Store::Shared();
    if (req->method == HTTP_POST) {
        cJSON* body = ReadBody(req, 4096);
        if (!body) return SendError(req, "bad_json");
        cJSON* date = cJSON_GetObjectItemCaseSensitive(body, "date");
        cJSON* items = cJSON_GetObjectItemCaseSensitive(body, "items");
        int y = 0, m = 0, d = 0;
        if (!cJSON_IsString(date) || !reminders::ParseDateKey(date->valuestring, &y, &m, &d)) {
            cJSON_Delete(body);
            return SendError(req, reminders::SetResultName(reminders::SetResult::BadDate));
        }
        if (!cJSON_IsArray(items)) {
            cJSON_Delete(body);
            return SendError(req, "bad_json");
        }
        std::vector<std::string> list;
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, items) {
            if (!cJSON_IsString(item)) {
                cJSON_Delete(body);
                return SendError(req, "bad_json");
            }
            list.push_back(item->valuestring);
        }
        cJSON_Delete(body);
        const reminders::SetResult result = store.Set(y, m, d, list);
        if (result != reminders::SetResult::Ok) {
            ESP_LOGW(kTag, "Rejected reminders: %s", reminders::SetResultName(result));
            return SendError(req, reminders::SetResultName(result));
        }
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON* all = cJSON_AddObjectToObject(root, "reminders");
    for (const auto& [key, items] : store.All()) {
        cJSON* list = cJSON_AddArrayToObject(all, key.c_str());
        for (const auto& text : items) cJSON_AddItemToArray(list, cJSON_CreateString(text.c_str()));
    }
    cJSON* limits = cJSON_AddObjectToObject(root, "limits");
    cJSON_AddNumberToObject(limits, "perDay", reminders::kMaxPerDay);
    cJSON_AddNumberToObject(limits, "textBytes", reminders::kMaxTextBytes);
    return SendJsonTree(req, root);
}

esp_err_t FestivalsHandler(httpd_req_t* req) {
    char query[64] = {};
    char value[16] = {};
    int year = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "year", value, sizeof(value)) == ESP_OK) {
        year = atoi(value);
    }
    if (year < cncal::kMinYear || year > cncal::kMaxYear) return SendError(req, "bad_date");

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON* all = cJSON_AddObjectToObject(root, "festivals");
    for (int month = 1; month <= 12; ++month) {
        for (int day = 1; day <= reminders::DaysInMonth(year, month); ++day) {
            const char* names[cncal::kMaxFestivalsPerDay];
            const int n = cncal::FestivalsOn(year, month, day, names, cncal::kMaxFestivalsPerDay);
            if (n == 0) continue;
            cJSON* list = cJSON_AddArrayToObject(all, reminders::DateKey(year, month, day).c_str());
            for (int i = 0; i < n; ++i) cJSON_AddItemToArray(list, cJSON_CreateString(names[i]));
        }
    }
    return SendJsonTree(req, root);
}

// ---- AI Q&A -------------------------------------------------------------------------------------

esp_err_t AiPageHandler(httpd_req_t* req) { return SendPage(req, kAiHtml); }

esp_err_t AiStateHandler(httpd_req_t* req) {
    auto& service = ai::ChatService::Get();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddBoolToObject(root, "started", service.Started());
    cJSON_AddBoolToObject(root, "hasKey", service.HasApiKey());  // the key itself is never sent back
    return SendJsonTree(req, root);
}

esp_err_t AiKeyHandler(httpd_req_t* req) {
    cJSON* body = ReadBody(req);
    if (!body) return SendError(req, "bad_json");
    cJSON* key = cJSON_GetObjectItemCaseSensitive(body, "key");
    const std::string value = cJSON_IsString(key) ? key->valuestring : "";
    cJSON_Delete(body);
    auto& service = ai::ChatService::Get();
    if (!service.SetApiKey(value)) {
        return SendError(req, "Key 无效，或设备尚未联网");
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    return SendJsonTree(req, root);
}

// ---- Token board --------------------------------------------------------------------------------

esp_err_t TokenBoardPageHandler(httpd_req_t* req) { return SendPage(req, kTokenBoardHtml); }

esp_err_t TokenBoardStateHandler(httpd_req_t* req) {
    auto& service = board::BoardService::Get();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddBoolToObject(root, "started", service.Started());
    cJSON_AddBoolToObject(root, "hasKey", service.HasCredentials());  // the key itself is never sent back
    cJSON_AddBoolToObject(root, "hasDeepseekKey", board::DeepSeekService::Get().HasApiKey());
    cJSON_AddNumberToObject(root, "interval", service.IntervalMinutes());
    return SendJsonTree(req, root);
}

esp_err_t TokenBoardDeepSeekHandler(httpd_req_t* req) {
    cJSON* body = ReadBody(req);
    if (!body) return SendError(req, "bad_json");
    cJSON* key = cJSON_GetObjectItemCaseSensitive(body, "key");
    const std::string value = cJSON_IsString(key) ? key->valuestring : "";
    cJSON_Delete(body);
    if (!board::DeepSeekService::Get().SetApiKey(value)) return SendError(req, "API Key 无效，或设备尚未联网");
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    return SendJsonTree(req, root);
}

esp_err_t TokenBoardIntervalHandler(httpd_req_t* req) {
    cJSON* body = ReadBody(req);
    if (!body) return SendError(req, "bad_json");
    cJSON* interval = cJSON_GetObjectItemCaseSensitive(body, "interval");
    const int minutes = cJSON_IsNumber(interval) ? interval->valueint : 0;
    cJSON_Delete(body);
    if (!board::BoardService::Get().SetIntervalMinutes(minutes)) return SendError(req, "刷新频率无效，或设备尚未联网");
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    return SendJsonTree(req, root);
}

esp_err_t TokenBoardKeyHandler(httpd_req_t* req) {
    cJSON* body = ReadBody(req);
    if (!body) return SendError(req, "bad_json");
    cJSON* id = cJSON_GetObjectItemCaseSensitive(body, "id");
    cJSON* secret = cJSON_GetObjectItemCaseSensitive(body, "secret");
    const std::string id_value = cJSON_IsString(id) ? id->valuestring : "";
    const std::string secret_value = cJSON_IsString(secret) ? secret->valuestring : "";
    cJSON_Delete(body);
    if (!board::BoardService::Get().SetCredentials(id_value, secret_value)) {
        return SendError(req, "ID 或 Secret 无效，或设备尚未联网");
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    return SendJsonTree(req, root);
}

}  // namespace web
}  // namespace rawdraw
