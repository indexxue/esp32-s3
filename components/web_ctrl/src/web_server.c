/**
 * @file web_server.c
 * @brief HTTP 服务：`GET /`（Wi‑Fi 配网 + 命令行单页）、`GET /api/health`、`POST /api/cmd`。
 */

#include "web_server.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#ifndef CONFIG_WEB_CTRL_CMD_SYNC_TIMEOUT_MS
#define CONFIG_WEB_CTRL_CMD_SYNC_TIMEOUT_MS (3000)
#endif

#include "esp_http_server.h"
#include "esp_log.h"

#include "web_ctrl_cmd.h"
#include "web_bmp_upload.h"

#include "cmd.h"

static const char *TAG = "web_server";

#define WEB_CMD_BODY_MAX (256U)
#define WEB_CMD_JSON_OUT (1024U)

static httpd_handle_t s_server;

static esp_err_t root_get_handler(httpd_req_t *req)
{
    static const char html[] =
        "<!DOCTYPE html><html lang='zh-CN'><head><meta charset='utf-8'><meta name='viewport' "
        "content='width=device-width,initial-scale=1'><title>设备控制</title>"
        "<style>"
        ":root{--bg:#0f1419;--surface:#1a2332;--surface2:#243044;--border:#2d3a4d;--text:#e8eef7;"
        "--muted:#8b9cb3;--accent:#3d8bfd;--accent-dim:#2a6fd6;--danger:#e85d6c;--danger-bg:#3a1f26;"
        "--ok:#3dd68c;--radius:12px;--shadow:0 8px 32px rgba(0,0,0,.35);--font:system-ui,-apple-system,'Segoe UI',sans-serif}"
        "*{box-sizing:border-box}body{margin:0;min-height:100vh;background:linear-gradient(160deg,#0a0e14 0%,#121a24 50%,#0f1419 100%);"
        "color:var(--text);font-family:var(--font);font-size:15px;line-height:1.5;-webkit-font-smoothing:antialiased}"
        ".wrap{max-width:640px;margin:0 auto;padding:20px 16px 48px}"
        ".top{margin-bottom:24px}.top h1{font-size:1.5rem;font-weight:700;margin:0 0 6px;letter-spacing:-.02em}"
        ".top p{margin:0;color:var(--muted);font-size:.9rem}"
        ".card{background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);"
        "box-shadow:var(--shadow);margin-bottom:16px;overflow:hidden}"
        ".card-h{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:14px 16px;"
        "background:var(--surface2);border-bottom:1px solid var(--border)}"
        ".card-h h2{font-size:1rem;font-weight:600;margin:0}"
        ".card-b{padding:16px}"
        ".row{display:flex;flex-wrap:wrap;gap:10px;align-items:center}"
        ".btn{display:inline-flex;align-items:center;justify-content:center;gap:6px;padding:9px 16px;border-radius:8px;"
        "border:1px solid transparent;font:inherit;font-weight:500;cursor:pointer;transition:background .15s,transform .1s}"
        ".btn:active{transform:scale(.98)}.btn:disabled{opacity:.45;cursor:not-allowed;transform:none}"
        ".btn-primary{background:var(--accent);color:#fff}.btn-primary:hover:not(:disabled){background:var(--accent-dim)}"
        ".btn-ghost{background:transparent;color:var(--text);border-color:var(--border)}"
        ".btn-ghost:hover:not(:disabled){background:var(--surface2)}"
        ".btn-danger{background:var(--danger-bg);color:var(--danger);border-color:#5c2a32}"
        ".btn-danger:hover:not(:disabled){background:#4a252c}"
        ".status{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:14px}"
        ".chip{display:inline-flex;align-items:center;padding:6px 11px;border-radius:999px;font-size:.8rem;"
        "background:var(--surface2);border:1px solid var(--border);color:var(--muted)}"
        ".chip strong{color:var(--text);font-weight:600;margin-left:4px}"
        ".chip.ok{border-color:#2a5c45;color:#9ae6c3}.chip.warn{border-color:#5c4a2a;color:#e6d39a}"
        ".field{margin-bottom:14px}.field label{display:block;font-size:.8rem;color:var(--muted);margin-bottom:6px;font-weight:500}"
        ".inp{width:100%;padding:11px 12px;border-radius:8px;border:1px solid var(--border);background:#0d1218;color:var(--text);"
        "font:inherit}.inp:focus{outline:none;border-color:var(--accent);box-shadow:0 0 0 3px rgba(61,139,253,.2)}"
        ".ap-list{max-height:240px;overflow:auto;border:1px solid var(--border);border-radius:8px;margin:12px 0;background:#0d1218}"
        ".ap-item{display:flex;align-items:center;justify-content:space-between;gap:10px;padding:10px 12px;"
        "border-bottom:1px solid var(--border);cursor:pointer;transition:background .12s}"
        ".ap-item:last-child{border-bottom:0}.ap-item:hover{background:var(--surface2)}"
        ".ap-name{font-weight:500;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;max-width:55%}"
        ".ap-meta{font-size:.75rem;color:var(--muted);text-align:right}"
        ".sig{display:inline-block;width:36px;height:4px;border-radius:2px;background:var(--border);vertical-align:middle;margin-right:6px;overflow:hidden}"
        ".sig i{display:block;height:100%;background:var(--accent);border-radius:2px}"
        ".hint{font-size:.8rem;color:var(--muted);margin-top:10px}"
        ".log{white-space:pre-wrap;background:#0d1218;border:1px solid var(--border);border-radius:8px;padding:12px;"
        "font-size:.8rem;max-height:200px;overflow:auto;margin-top:12px;color:#c5d4e8}"
        ".danger-zone{margin-top:20px;padding-top:16px;border-top:1px dashed var(--border)}"
        ".danger-zone .dz-title{font-size:.85rem;font-weight:600;color:var(--danger);margin:0 0 8px}"
        ".danger-zone .dz-desc{font-size:.8rem;color:var(--muted);margin:0 0 12px}"
        ".modal-wrap{position:fixed;inset:0;background:rgba(0,0,0,.55);display:none;align-items:center;justify-content:center;"
        "padding:16px;z-index:50}.modal-wrap.on{display:flex}"
        ".modal{background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);max-width:400px;width:100%;"
        "box-shadow:var(--shadow);padding:20px}"
        ".modal h3{margin:0 0 10px;font-size:1.05rem}.modal p{margin:0 0 18px;color:var(--muted);font-size:.9rem}"
        ".modal-actions{display:flex;gap:10px;justify-content:flex-end;flex-wrap:wrap}"
        ".scan-h{font-size:.8rem;color:var(--muted);min-height:1.2em}"
        ".cmd-row{display:flex;gap:8px;flex-wrap:wrap}.cmd-row .inp{flex:1;min-width:200px}"
        ".gal-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(136px,1fr));gap:12px;margin-top:8px}"
        ".gal-item{background:#0d1218;border:1px solid var(--border);border-radius:8px;padding:10px;text-align:center}"
        ".gal-item img{max-width:100%;max-height:96px;object-fit:contain;border-radius:4px;background:rgba(0,0,0,.15);display:block;margin:0 auto}"
        ".gal-ph{height:96px;display:flex;align-items:center;justify-content:center;color:var(--muted);font-size:.85rem}"
        ".gal-item .nm{font-size:.72rem;color:var(--muted);word-break:break-all;margin:8px 0 4px;line-height:1.3}"
        ".gal-item .gal-by{font-size:.68rem;color:var(--muted)}"
        ".gal-actions{display:flex;flex-direction:column;gap:6px;margin-top:10px}"
        ".gal-actions .btn{padding:6px 8px;font-size:.74rem;width:100%}"
        ".gal-tag{font-size:.65rem;color:var(--ok);margin-top:2px;text-align:center}"
        ".fx-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(108px,1fr));gap:8px;width:100%;margin-top:10px}"
        ".fx-grid .btn{width:100%;padding:8px 10px;font-size:.82rem}"
        ".fx-btn.on{background:var(--accent);color:#fff;border-color:var(--accent-dim);box-shadow:0 0 0 2px rgba(61,139,253,.35)}"
        ".fx-btn.on:hover:not(:disabled){background:var(--accent-dim)}"
        "</style></head><body><div class='wrap'>"
        "<header class='top'><h1>设备控制</h1><p>网络配网与串口命令（与 USB <code>cmd</code> 一致）</p></header>"
        "<section class='card' id='card-wifi'>"
        "<div class='card-h'><h2>Wi‑Fi 配网</h2><div class='row'><button type='button' class='btn btn-ghost' id='btn-refresh' "
        "onclick='refreshStatus()'>刷新状态</button><button type='button' class='btn btn-primary' id='btn-scan' onclick='doScan()'>"
        "扫描附近网络</button></div></div>"
        "<div class='card-b'>"
        "<div class='status' id='st'><span class='chip'>状态加载中…</span></div>"
        "<p class='scan-h' id='s'></p>"
        "<div class='ap-list' id='u' hidden></div>"
        "<div class='field'><label for='ssid'>网络名称 (SSID)</label><input class='inp' id='ssid' autocomplete='off' "
        "placeholder='选择上方列表或手动输入'/></div>"
        "<div class='field'><label for='pw'>密码</label><input class='inp' id='pw' type='password' autocomplete='off' "
        "placeholder='开放网络可留空'/></div>"
        "<div class='row'><button type='button' class='btn btn-primary' id='btn-save' onclick='doSave()'>保存并重启设备</button></div>"
        "<p class='hint'>保存后设备会使用新凭据连接路由器并重启；若通过 SoftAP 配网，重启后可能需重新连接热点。</p>"
        "<pre class='log' id='wo' hidden></pre>"
        "<div class='danger-zone'>"
        "<p class='dz-title'>断开并清除已保存的路由器</p>"
        "<p class='dz-desc'>将断开当前 STA、清除已保存的 Wi‑Fi 账号并重启。用于换路由器或恢复出厂网络设置。</p>"
        "<button type='button' class='btn btn-danger' id='btn-disconnect' onclick='openDisconnectModal()'>清除已存 Wi‑Fi 并重启…</button>"
        "</div></div></section>"
        "<section class='card' id='card-ledfx'>"
        "<div class='card-h'><h2>WS2812 灯效</h2></div>"
        "<div class='card-b'>"
        "<p class='hint' style='margin-top:0'>点击场景会先停止当前灯效再播放所选效果（网页预览不受优先级影响）。<span id='fx-ver' style='opacity:.6'> UI fx2</span></p>"
        "<div class='fx-grid' id='fx-grid'>"
        "<button type='button' class='btn btn-ghost fx-btn fx-busy' data-fx='working'>彩虹工作</button>"
        "<button type='button' class='btn btn-ghost fx-btn fx-busy' data-fx='pairing'>配网</button>"
        "<button type='button' class='btn btn-ghost fx-btn fx-busy' data-fx='config'>配置紫灯</button>"
        "<button type='button' class='btn btn-ghost fx-btn fx-busy' data-fx='trigger'>触发</button>"
        "<button type='button' class='btn btn-ghost fx-btn fx-busy' data-fx='success'>成功</button>"
        "<button type='button' class='btn btn-ghost fx-btn fx-busy' data-fx='net_offline'>断网</button>"
        "<button type='button' class='btn btn-ghost fx-btn fx-busy' data-fx='net_online'>回网</button>"
        "<button type='button' class='btn btn-ghost fx-btn fx-busy' data-fx='error'>异常</button>"
        "<button type='button' class='btn btn-ghost fx-btn fx-busy' data-fx='alarm'>报警</button>"
        "<button type='button' class='btn btn-ghost fx-btn fx-busy' data-fx='charging'>充电</button>"
        "<button type='button' class='btn btn-ghost fx-btn fx-busy' data-fx='low_battery'>低电量</button>"
        "<button type='button' class='btn btn-ghost fx-btn fx-busy' data-fx='bootup'>上电自检</button>"
        "<button type='button' class='btn btn-danger fx-busy' id='btn-fx-stop'>停止全部</button>"
        "</div>"
        "<pre class='log' id='fxo' hidden></pre></div></section>"
        "<section class='card' id='card-cmd'>"
        "<div class='card-h'><h2>命令行</h2></div>"
        "<div class='card-b'><div class='cmd-row'>"
        "<input class='inp' id='l' type='text' value='version' placeholder='输入命令，例如 version'/>"
        "<button type='button' class='btn btn-primary' id='btn-run' onclick='run()'>执行</button></div>"
        "<pre class='log' id='o' hidden></pre></div></section>"
        "<section class='card' id='card-bmp'>"
        "<div class='card-h'><h2>BMP 上传</h2></div>"
        "<div class='card-b'>"
        "<p class='hint' style='margin-top:0'>选择 BI_RGB 无压缩 24/32 位 BMP；每次上传保存为 SD 根目录下唯一短文件名（如 <code>W01A2B3C.BMP</code>，FAT 8.3）。可勾选上传后在 LCD 全屏显示。</p>"
        "<div class='field'><label for='bf'>图片文件</label><input class='inp' id='bf' type='file' accept='.bmp,image/bmp'/></div>"
        "<label class='row' style='cursor:pointer'><input type='checkbox' id='lcdisp'/> <span>上传后在 LCD 显示</span></label>"
        "<div class='row' style='margin-top:12px'>"
        "<button type='button' class='btn btn-primary' id='btn-up-bmp' onclick='uploadBmp()'>上传</button></div>"
        "<pre class='log' id='bo' hidden></pre></div></section>"
        "<section class='card' id='card-gal'><div class='card-h'><h2>SD 图库</h2><div class='row'>"
        "<button type='button' class='btn btn-ghost' id='btn-gal-refresh' onclick='loadGallery()'>刷新列表</button>"
        "<button type='button' class='btn btn-ghost' id='btn-gal-clear-boot' onclick='clearGalBoot()'>清除开机默认</button></div></div>"
        "<div class='card-b'><p class='hint' style='margin-top:0'>根目录下 .bmp / .bin（与 LCD 图库相同，最多 24 个）。"
        "<strong>LCD 显示</strong>立即全屏出图；<strong>设为开机默认</strong>写入 NVS，下次上电优先显示该文件（须仍在卡上）。</p>"
        "<div id='gal-h' class='scan-h'>加载中…</div><div class='gal-grid' id='gal-grid'></div>"
        "<pre class='log' id='gl' hidden></pre></div></section></div>"
        "<div class='modal-wrap' id='modal-disconnect' role='dialog' aria-modal='true'>"
        "<div class='modal'><h3>确认清除 Wi‑Fi？</h3>"
        "<p>将断开 STA、删除已保存的路由器账号并立即重启。此页面会暂时无法访问。</p>"
        "<div class='modal-actions'>"
        "<button type='button' class='btn btn-ghost' onclick='closeDisconnectModal()'>取消</button>"
        "<button type='button' class='btn btn-danger' id='modal-confirm-dc' onclick='confirmStaDisconnect()'>确认清除并重启</button>"
        "</div></div></div>"
        "<script>"
        "function esc(t){return String(t===undefined||t===null?'':t).replace(/&/g,'&amp;').replace(/</g,'&lt;');}"
        "function setBusy(b){['btn-refresh','btn-scan','btn-save','btn-run','btn-up-bmp','btn-gal-refresh','btn-gal-clear-boot','btn-disconnect','modal-confirm-dc'].forEach(function(id){"
        "var el=document.getElementById(id);if(el)el.disabled=b;});"
        "document.querySelectorAll('.fx-busy').forEach(function(el){el.disabled=b;});}"
        "function showLog(id,txt,show){var el=document.getElementById(id);if(!el)return;if(!txt){el.hidden=true;el.textContent='';return;}"
        "el.hidden=!show;el.textContent=txt;}"
        "function rssiPct(r){if(typeof r!=='number')return 15;return Math.max(8,Math.min(100,2*(r+100)));}"
        "async function refreshStatus(){"
        "var st=document.getElementById('st');"
        "try{var r=await fetch('/api/wifi/status');var j=await r.json();"
        "if(!j.ok){st.innerHTML=\"<span class='chip warn'><strong>错误</strong>无法读取状态</span>\";return;}"
        "var ip=j.ip||'—';var ok=(j.sta_has_ip===true)||(j.softap===true);"
        "var mode=j.mode||'—';"
        "var chips=\"<span class='chip'><strong>模式</strong>\"+esc(mode)+\"</span>\""
        "+\"<span class='chip'><strong>IP</strong>\"+esc(ip)+\"</span>\""
        "+\"<span class='chip \"+(ok?'ok':'warn')+\"'><strong>网络</strong>\"+(ok?'已连接':'未就绪')+\"</span>\";"
        "var b=j.battery;"
        "if(b&&b.valid){"
        "chips+=\"<span class='chip'><strong>电量</strong>\"+esc(String(b.percent))+\"%（\"+esc(String(b.mv))+\" mV）</span>\";"
        "if(b.charging){chips+=\"<span class='chip ok'><strong>状态</strong>充电中</span>\";}"
        "}else if(b&&!b.valid){chips+=\"<span class='chip warn'><strong>电量</strong>—</span>\";}"
        "st.innerHTML=chips;"
        "}catch(e){st.innerHTML=\"<span class='chip warn'><strong>异常</strong>\"+esc(String(e))+\"</span>\";}"
        "}"
        "async function doScan(){"
        "var s=document.getElementById('s'),list=document.getElementById('u');"
        "showLog('wo','',false);setBusy(true);s.textContent='正在扫描…';list.innerHTML='';list.hidden=false;"
        "try{"
        "await fetch('/api/wifi/scan',{method:'POST'});"
        "for(var i=0;i<40;i++){"
        "var r=await fetch('/api/wifi/scan_result');var j=await r.json();"
        "if(j.pending){await new Promise(function(x){setTimeout(x,250);});continue;}"
        "s.textContent=(j.aps&&j.aps.length)?('找到 '+j.aps.length+' 个网络，点击一行填入 SSID'):'未发现网络';"
        "(j.aps||[]).forEach(function(a){"
        "var row=document.createElement('div');row.className='ap-item';row.setAttribute('role','button');"
        "var nm=document.createElement('span');nm.className='ap-name';nm.textContent=String(a.ssid);"
        "var meta=document.createElement('span');meta.className='ap-meta';"
        "var sig=document.createElement('span');sig.className='sig';"
        "var sigi=document.createElement('i');sigi.style.width=rssiPct(a.rssi)+'%';sig.appendChild(sigi);"
        "meta.appendChild(sig);meta.appendChild(document.createTextNode(' ch'+a.ch+' '+a.rssi+' dBm'));"
        "row.appendChild(nm);row.appendChild(meta);"
        "row.onclick=function(){document.getElementById('ssid').value=a.ssid;row.style.outline='2px solid var(--accent)';"
        "setTimeout(function(){row.style.outline='';},400);};"
        "list.appendChild(row);});"
        "break;}"
        "}catch(e){showLog('wo',String(e),true);s.textContent='';}"
        "finally{setBusy(false);}"
        "}"
        "async function doSave(){"
        "showLog('wo','提交中…',true);setBusy(true);"
        "try{"
        "var body=JSON.stringify({ssid:document.getElementById('ssid').value,password:document.getElementById('pw').value});"
        "var r=await fetch('/api/wifi/save',{method:'POST',headers:{'Content-Type':'application/json'},body:body});"
        "showLog('wo',r.status+' '+await r.text(),true);"
        "}catch(e){showLog('wo',String(e),true);}"
        "finally{setBusy(false);}"
        "}"
        "function openDisconnectModal(){document.getElementById('modal-disconnect').classList.add('on');}"
        "function closeDisconnectModal(){document.getElementById('modal-disconnect').classList.remove('on');}"
        "async function confirmStaDisconnect(){"
        "closeDisconnectModal();showLog('wo','正在断开并重启…',true);setBusy(true);"
        "try{"
        "var r=await fetch('/api/wifi/sta_disconnect',{method:'POST'});"
        "showLog('wo',await r.text(),true);refreshStatus();"
        "}catch(e){showLog('wo',String(e),true);}"
        "finally{setBusy(false);}"
        "}"
        "async function postCmdLine(line,logId){"
        "var lid=logId||'o';"
        "showLog(lid,'执行中…',true);setBusy(true);"
        "try{"
        "var r=await fetch('/api/cmd',{method:'POST',headers:{'Content-Type':'application/json'},"
        "body:JSON.stringify({line:line})});"
        "showLog(lid,r.status+' '+await r.text(),true);"
        "}catch(e){showLog(lid,String(e),true);}"
        "finally{setBusy(false);}"
        "}"
        "function setFxActive(btn){"
        "document.querySelectorAll('.fx-btn').forEach(function(el){el.classList.remove('on');});"
        "if(btn){btn.classList.add('on');}"
        "}"
        "function clearFxActive(){"
        "document.querySelectorAll('.fx-btn').forEach(function(el){el.classList.remove('on');});"
        "}"
        "async function runLedFx(scene,btn){"
        "setFxActive(btn);"
        "await postCmdLine('ledfx switch '+scene,'fxo');"
        "}"
        "async function stopAllLedFx(){clearFxActive();await postCmdLine('ledfx cancel all','fxo');}"
        "function initFxGrid(){"
        "var grid=document.getElementById('fx-grid');"
        "if(!grid)return;"
        "grid.addEventListener('click',function(ev){"
        "var stopBtn=ev.target.closest('#btn-fx-stop');"
        "if(stopBtn&&!stopBtn.disabled){stopAllLedFx();return;}"
        "var btn=ev.target.closest('.fx-btn');"
        "if(!btn||btn.disabled)return;"
        "var scene=btn.getAttribute('data-fx');"
        "if(scene){runLedFx(scene,btn);}"
        "});"
        "}"
        "async function run(){await postCmdLine(document.getElementById('l').value,'o');}"
        "async function uploadBmp(){"
        "var inp=document.getElementById('bf');var f=inp&&inp.files&&inp.files[0];"
        "if(!f){showLog('bo','请先选择 .bmp 文件',true);return;}"
        "var q=document.getElementById('lcdisp')&&document.getElementById('lcdisp').checked?'?display=1':'';"
        "var fd=new FormData();fd.append('file',f);"
        "showLog('bo','上传中…',true);setBusy(true);"
        "try{"
        "var r=await fetch('/api/upload/bmp'+q,{method:'POST',body:fd});"
        "var t=await r.text();"
        "showLog('bo',r.status+' '+t,true);"
        "if(r.ok){try{var j=JSON.parse(t);if(j.ok)loadGallery();}catch(x){}}"
        "}catch(e){showLog('bo',String(e),true);}"
        "finally{setBusy(false);}"
        "}"
        "function galFmtBytes(n){if(typeof n!=='number')return'';if(n>=1048576)return(n/1048576).toFixed(1)+' MB';"
        "if(n>=1024)return(n/1024).toFixed(1)+' KB';return n+' B';}"
        "var sGalBoot='';"
        "async function loadGallery(){"
        "var g=document.getElementById('gal-grid'),h=document.getElementById('gal-h');"
        "if(!g||!h)return;showLog('gl','',false);h.textContent='加载中…';g.innerHTML='';"
        "try{"
        "var ra=await Promise.all([fetch('/api/gallery/list'),fetch('/api/gallery/prefs')]);"
        "var j=await ra[0].json();var pj=await ra[1].json();"
        "sGalBoot=(pj&&pj.ok&&pj.boot_name)?String(pj.boot_name):'';"
        "if(!j.ok){h.textContent='';showLog('gl',JSON.stringify(j),true);return;}"
        "var fs=j.files||[];"
        "h.textContent=fs.length?('共 '+fs.length+' 个文件'+(sGalBoot?('；开机默认：'+sGalBoot):'；未设开机默认（上电首张）')):('暂无 .bmp / .bin');"
        "fs.forEach(function(f){"
        "var d=document.createElement('div');d.className='gal-item';"
        "if(f.kind==='bmp'){var im=document.createElement('img');im.alt=f.name;im.loading='lazy';"
        "im.src='/api/gallery/bmp?name='+encodeURIComponent(f.name);d.appendChild(im);}"
        "else{var ph=document.createElement('div');ph.className='gal-ph';ph.textContent='BIN';d.appendChild(ph);}"
        "var nm=document.createElement('div');nm.className='nm';nm.textContent=f.name;d.appendChild(nm);"
        "var by=document.createElement('div');by.className='gal-by';by.textContent=galFmtBytes(f.bytes);d.appendChild(by);"
        "var act=document.createElement('div');act.className='gal-actions';"
        "var bLcd=document.createElement('button');bLcd.type='button';bLcd.className='btn btn-primary';bLcd.textContent='LCD 显示';"
        "bLcd.onclick=function(){showGalOnLcd(f.name);};act.appendChild(bLcd);"
        "var bBoot=document.createElement('button');bBoot.type='button';bBoot.className='btn btn-ghost';bBoot.textContent='设为开机默认';"
        "bBoot.onclick=function(){setGalBootDefault(f.name);};act.appendChild(bBoot);"
        "if(sGalBoot===f.name){var tg=document.createElement('div');tg.className='gal-tag';tg.textContent='当前开机默认';act.appendChild(tg);}"
        "d.appendChild(act);"
        "var del=document.createElement('button');del.type='button';del.className='btn btn-danger';"
        "del.style.cssText='margin-top:8px;width:100%;padding:7px;font-size:.78rem';del.textContent='删除';"
        "del.onclick=function(){delGalFile(f.name);};d.appendChild(del);g.appendChild(d);});"
        "}catch(e){h.textContent='';showLog('gl',String(e),true);}"
        "}"
        "async function showGalOnLcd(name){"
        "showLog('gl','',false);setBusy(true);"
        "try{var r=await fetch('/api/gallery/show?name='+encodeURIComponent(name),{method:'POST'});"
        "var t=await r.text();showLog('gl',r.status+' '+t,true);}"
        "catch(e){showLog('gl',String(e),true);}"
        "finally{setBusy(false);}"
        "}"
        "async function setGalBootDefault(name){"
        "showLog('gl','',false);setBusy(true);"
        "try{var r=await fetch('/api/gallery/prefs?boot_name='+encodeURIComponent(name),{method:'POST'});"
        "var t=await r.text();showLog('gl',r.status+' '+t,true);await loadGallery();}"
        "catch(e){showLog('gl',String(e),true);}"
        "finally{setBusy(false);}"
        "}"
        "async function clearGalBoot(){"
        "if(!confirm('清除开机默认图？下次上电将显示排序后的第一张。'))return;"
        "showLog('gl','',false);setBusy(true);"
        "try{var r=await fetch('/api/gallery/prefs?boot_name=',{method:'POST'});"
        "var t=await r.text();showLog('gl',r.status+' '+t,true);await loadGallery();}"
        "catch(e){showLog('gl',String(e),true);}"
        "finally{setBusy(false);}"
        "}"
        "async function delGalFile(name){"
        "if(!confirm('确定删除「'+name+'」？不可恢复。'))return;"
        "showLog('gl','',false);setBusy(true);"
        "try{var r=await fetch('/api/gallery/delete?name='+encodeURIComponent(name),{method:'POST'});"
        "var t=await r.text();showLog('gl',r.status+' '+t,true);await loadGallery();}"
        "catch(e){showLog('gl',String(e),true);}"
        "finally{setBusy(false);}"
        "}"
        "document.getElementById('modal-disconnect').addEventListener('click',function(ev){"
        "if(ev.target.id==='modal-disconnect')closeDisconnectModal();});"
        "initFxGrid();refreshStatus();loadGallery();setInterval(refreshStatus,2500);"
        "</script></body></html>";

    (void)httpd_resp_set_type(req, "text/html; charset=utf-8");
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t health_get_handler(httpd_req_t *req)
{
    static const char json[] = "{\"ok\":true}";

    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static bool json_skip_ws(const char **pp)
{
    const char *p = *pp;

    if (p == NULL) {
        return false;
    }
    while ((*p != '\0') && (isspace((unsigned char)*p) != 0)) {
        p++;
    }
    *pp = p;
    return (*p != '\0');
}

static bool json_extract_line_field(const char *body, char *out_line, size_t out_cap)
{
    static const char keypat[] = "\"line\"";
    const char       *found = strstr(body, keypat);
    const char       *colon;
    const char       *p;
    size_t            o = 0U;

    if ((found == NULL) || (out_cap == 0U)) {
        return false;
    }
    colon = strchr(found + sizeof(keypat) - 1U, ':');
    if (colon == NULL) {
        return false;
    }
    p = colon + 1U;
    if (!json_skip_ws(&p)) {
        return false;
    }
    if (*p != '"') {
        return false;
    }
    p++;
    while ((*p != '\0') && (*p != '"')) {
        if ((*p == '\\') && (p[1] != '\0')) {
            p++;
        }
        if (o + 1U >= out_cap) {
            return false;
        }
        out_line[o++] = *p++;
    }
    if (*p != '"') {
        return false;
    }
    out_line[o] = '\0';
    return (*p == '"');
}

static size_t json_escape_to_buf(const char *src, char *dst, size_t dst_cap)
{
    size_t j = 0U;

    if ((src == NULL) || (dst == NULL) || (dst_cap == 0U)) {
        return 0U;
    }
    for (size_t i = 0U; (src[i] != '\0') && (j + 1U < dst_cap); i++) {
        const unsigned char c = (unsigned char)src[i];

        if ((c == '"') || (c == '\\')) {
            if (j + 2U >= dst_cap) {
                break;
            }
            dst[j++] = '\\';
            dst[j++] = (char)c;
        } else if (c < 0x20U) {
            int w = snprintf((char *)(dst + j), dst_cap - j, "\\u%04x", (unsigned int)c);
            if ((w <= 0) || ((size_t)w >= dst_cap - j)) {
                break;
            }
            j += (size_t)w;
        } else {
            dst[j++] = (char)c;
        }
    }
    if (j < dst_cap) {
        dst[j] = '\0';
    } else {
        dst[dst_cap - 1U] = '\0';
    }
    return j;
}

/** 读 POST body：依 `req->content_len` 循环 recv（与 `web_ctrl_wifi_api` 一致）。 */
static esp_err_t web_http_read_post_body(httpd_req_t *req, char *body, size_t body_cap, size_t *out_len)
{
    size_t body_len;
    size_t total;
    int    rlen;

    if ((out_len == NULL) || (body_cap <= 1U)) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_len = 0U;
    body_len = (size_t)req->content_len;
    if (body_len == 0U) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (body_len >= body_cap) {
        return ESP_ERR_NO_MEM;
    }
    total = 0U;
    while (total < body_len) {
        rlen = httpd_req_recv(req, body + total, body_len - total);
        if (rlen < 0) {
            ESP_LOGW(TAG, "cmd post recv err %d", rlen);
            return ESP_FAIL;
        }
        if (rlen == 0) {
            return ESP_FAIL;
        }
        total += (size_t)rlen;
    }
    body[body_len] = '\0';
    *out_len = body_len;
    return ESP_OK;
}

static esp_err_t cmd_post_handler(httpd_req_t *req)
{
    char              body[WEB_CMD_BODY_MAX + 1U];
    char              line[CMD_LINE_MAX];
    char              reply[WEB_CTRL_CMD_REPLY_MAX];
    char              json[WEB_CMD_JSON_OUT];
    size_t            body_len = 0U;
    size_t            out_len  = 0U;
    esp_err_t         rbody;
    esp_err_t         ex;
    const int         tmo_ms = CONFIG_WEB_CTRL_CMD_SYNC_TIMEOUT_MS;

    rbody = web_http_read_post_body(req, body, sizeof(body), &body_len);
    if (rbody == ESP_ERR_INVALID_SIZE) {
        (void)httpd_resp_set_status(req, "411 Length Required");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"need body (content_len 0)\"}", HTTPD_RESP_USE_STRLEN);
    }
    if (rbody == ESP_ERR_NO_MEM) {
        (void)httpd_resp_set_status(req, "413 Payload Too Large");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"body too large\"}", HTTPD_RESP_USE_STRLEN);
    }
    if (rbody != ESP_OK) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"recv\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (!json_extract_line_field(body, line, sizeof(line))) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"need JSON {\\\"line\\\":\\\"...\\\"}\"}",
                               HTTPD_RESP_USE_STRLEN);
    }

    ex = web_ctrl_cmd_execute_sync(line, reply, sizeof(reply), &out_len, tmo_ms);
    if (ex == ESP_ERR_INVALID_STATE) {
        (void)httpd_resp_set_status(req, "503 Service Unavailable");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"busy\"}", HTTPD_RESP_USE_STRLEN);
    }
    if (ex == ESP_ERR_TIMEOUT) {
        (void)httpd_resp_set_status(req, "504 Gateway Timeout");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"timeout\"}", HTTPD_RESP_USE_STRLEN);
    }
    if (ex != ESP_OK) {
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"exec\"}", HTTPD_RESP_USE_STRLEN);
    }

    {
        static const char prefix[] = "{\"ok\":true,\"reply\":\"";
        const size_t      plen      = sizeof(prefix) - 1U;
        const size_t      tail_room = (sizeof(json) > plen + 3U) ? (sizeof(json) - plen - 3U) : 0U;
        size_t            j;

        if (tail_room < 1U) {
            (void)httpd_resp_set_status(req, "500 Internal Server Error");
            (void)httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req, "{\"ok\":false,\"error\":\"buffer\"}", HTTPD_RESP_USE_STRLEN);
        }
        (void)memcpy(json, prefix, plen);
        j = json_escape_to_buf(reply, json + plen, tail_room + 1U);
        if (j > tail_room) {
            j = tail_room;
        }
        json[plen + j]       = '"';
        json[plen + j + 1U] = '}';
        json[plen + j + 2U] = '\0';
    }

    (void)httpd_resp_set_status(req, "200 OK");
    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

esp_err_t web_server_start(uint16_t port)
{
    if (s_server != NULL) {
        ESP_LOGW(TAG, "HTTP server already running");
        return ESP_ERR_INVALID_STATE;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = (port == 0U) ? 80U : port;
    /* 默认 8 槽：`web_server` 3 个 + `web_ctrl_wifi_api` 7 个会溢出，须加大。 */
    config.max_uri_handlers = 24U;
    /* 默认栈 4096：`wifi_scan_result_get_handler` 等单帧 JSON 约 4KB，会栈溢出破坏 httpd 会话表。 */
    config.stack_size = 12288U;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    const httpd_uri_t uri_root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t uri_health = {
        .uri = "/api/health",
        .method = HTTP_GET,
        .handler = health_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t uri_cmd = {
        .uri = "/api/cmd",
        .method = HTTP_POST,
        .handler = cmd_post_handler,
        .user_ctx = NULL,
    };

    err = httpd_register_uri_handler(s_server, &uri_root);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register / failed: %s", esp_err_to_name(err));
        (void)httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    err = httpd_register_uri_handler(s_server, &uri_health);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register /api/health failed: %s", esp_err_to_name(err));
        (void)httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    err = httpd_register_uri_handler(s_server, &uri_cmd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register /api/cmd failed: %s", esp_err_to_name(err));
        (void)httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    err = web_bmp_upload_register(s_server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register /api/upload/bmp failed: %s", esp_err_to_name(err));
        (void)httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    ESP_LOGI(TAG, "HTTP listening on port %u", (unsigned int)config.server_port);
    return ESP_OK;
}

httpd_handle_t web_server_get_handle(void)
{
    return s_server;
}

esp_err_t web_server_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }

    esp_err_t err = httpd_stop(s_server);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "httpd_stop: %s", esp_err_to_name(err));
    }
    s_server = NULL;
    return ESP_OK;
}

bool web_server_is_running(void)
{
    return s_server != NULL;
}
