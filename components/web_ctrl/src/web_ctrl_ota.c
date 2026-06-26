/**
 * @file web_ctrl_ota.c
 * @brief HTTP OTA：维护页与 api/ota REST（二进制上传 + apply/abort）。
 */

#include "web_ctrl_ota.h"

#include "sdkconfig.h"

#if CONFIG_WEB_CTRL_OTA

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "lwip/sockets.h"

#include "net_wifi.h"
#include "ota.h"

static const char *TAG = "web_ctrl_ota";

#ifndef CONFIG_WEB_CTRL_OTA_UPLOAD_MAX
#define WEB_CTRL_OTA_UPLOAD_MAX_BYTES (8388608U)
#else
#define WEB_CTRL_OTA_UPLOAD_MAX_BYTES ((size_t)CONFIG_WEB_CTRL_OTA_UPLOAD_MAX)
#endif

#ifndef CONFIG_WEB_CTRL_OTA_DEFAULT_PRODUCT
#define WEB_CTRL_OTA_PRODUCT "project"
#else
#define WEB_CTRL_OTA_PRODUCT CONFIG_WEB_CTRL_OTA_DEFAULT_PRODUCT
#endif

#define OTA_HTTP_RECV_CHUNK (4096U)
#define OTA_HEADER_PEEK     (4096U)

#if CONFIG_WEB_CTRL_OTA_CLOUD_PULL
#define OTA_PAGE_CLOUD_PULL_HTML \
    "<hr><p class=hint><strong>云端拉包</strong>（STA 联网）：填写 manifest.json 完整 URL（本机测试可用 http://），" \
    "设备将按 manifest 校验版本与 SHA256 后写入对侧槽。</p>" \
    "<input type=url id=mu placeholder=http://192.168.x.x:8080/manifest.json style=width:100%><br>" \
    "<label><input type=checkbox id=aa> 拉取完成后自动 apply 并重启</label><br>" \
    "<button type=button id=pl disabled>开始拉包</button>"
#define OTA_PAGE_CLOUD_PULL_JS \
    "function syncPull(st){const pl=document.getElementById('pl');" \
    "if(pl)pl.disabled=pullBusy||uploadBusy||applyBusy||(st==='writing')||(st==='pulling');}" \
    "const oldSync=syncButtons;syncButtons=function(st){oldSync(st);syncPull(st);};" \
    "document.getElementById('pl').onclick=async()=>{" \
    "const url=document.getElementById('mu').value.trim();" \
    "if(!url||pullBusy)return;" \
    "if(!/^https?:\\/\\//i.test(url)){L('URL 须以 http:// 或 https:// 开头');return;}" \
    "pullBusy=true;syncPull('pulling');L('拉包 '+url+'…');" \
    "try{const r=await fetch('/api/ota/pull',{method:'POST'," \
    "headers:{'Content-Type':'application/json'}," \
    "body:JSON.stringify({manifest_url:url,product:'" WEB_CTRL_OTA_PRODUCT "'," \
    "apply:document.getElementById('aa').checked})});" \
    "const txt=await r.text();L('HTTP '+r.status+' '+txt);" \
    "if(r.status!==200){pullBusy=false;refresh();return;}" \
    "let lastLog=0;" \
    "while(pullBusy){await new Promise(res=>setTimeout(res,800));" \
    "try{const sr=await fetch('/api/ota/status');const sj=await sr.json();" \
    "if(!sj.ok)continue;" \
    "if(sj.written>0&&sj.total>0){fill.style.width=Math.min(100,Math.round(100*sj.written/sj.total))+'%';}" \
    "const pct=(sj.total>0)?Math.round(100*sj.written/sj.total):0;" \
    "const now=Date.now();" \
    "if(sj.state==='pulling'||sj.state==='writing'){" \
    "if(now-lastLog>1200){L('下载中 '+sj.written+'/'+sj.total+' ('+pct+'%)');lastLog=now;}" \
    "syncPull(sj.state);continue;}" \
    "if(sj.state==='ready'){L('拉包完成，待切换版本 '+(sj.pending_ver||'?')+' → 点「确认重启」');pullBusy=false;break;}" \
    "L('拉包结束：state='+sj.state+'（若仍为 idle 可能失败，见串口 ota_pull 日志）');pullBusy=false;break;" \
    "}catch(e){L('轮询异常: '+e);}}" \
    "refresh();}catch(e){L(String(e));pullBusy=false;refresh();}};"
#else
#define OTA_PAGE_CLOUD_PULL_HTML ""
#define OTA_PAGE_CLOUD_PULL_JS   ""
#endif

static const char s_ota_page_html[] =
    "<!DOCTYPE html><html lang=zh-CN><head><meta charset=utf-8><meta name=viewport "
    "content=\"width=device-width,initial-scale=1\"><title>固件 OTA</title>"
    "<style>body{font-family:system-ui,sans-serif;margin:1rem;max-width:640px}"
    "button{margin:.25rem .5rem .25rem 0;padding:8px 14px}button:disabled{opacity:.45}"
    "#bar{height:10px;background:#ddd;border-radius:4px;margin:12px 0}"
    "#fill{height:100%;background:#4caf50;width:0;border-radius:4px;transition:width .2s}"
    ".hint{color:#555;font-size:.9rem}.err{color:#c62828}.okmsg{color:#2e7d32}"
    "#log{white-space:pre-wrap;background:#f5f5f5;padding:10px;font-size:.85rem;max-height:220px;overflow:auto}"
    "</style></head><body>"
    "<h1>固件 OTA</h1>"
    "<p class=hint>上传 <strong>release</strong> 应用镜像（推荐 "
    "<code>project_x.y.z_YYYYMMDD.bin</code>）到<strong>对侧槽</strong>，完成后点「确认重启」。"
    "选文件见 <code>firmware/&lt;ver&gt;/manifest.json</code> → "
    "<code>products.project.ota.image</code>。新固件版本须<strong>高于</strong>当前运行版本"
    "（<code>idf -Project project release &lt;ver&gt;</code>）。</p>"
    "<p id=info>加载中…</p><p id=hint></p>"
    "<input type=file id=f accept=.bin,application/octet-stream><br>"
    "<button id=up disabled>上传</button>"
    "<button id=ab disabled>放弃</button>"
    "<button id=ap disabled>确认重启</button>"
    "<div id=bar><div id=fill></div></div>"
    OTA_PAGE_CLOUD_PULL_HTML
    "<pre id=log></pre>"
    "<script>"
    "const MAGIC=0xABCD5432,info=document.getElementById('info'),hint=document.getElementById('hint'),"
    "log=document.getElementById('log'),fill=document.getElementById('fill'),"
    "up=document.getElementById('up'),ab=document.getElementById('ab'),ap=document.getElementById('ap'),"
    "fi=document.getElementById('f');"
    "let runVer='',uploadBusy=false,applyBusy=false,pickedVer='',pullBusy=false;"
    "function L(m){log.textContent+=m+'\\n';log.scrollTop=log.scrollHeight;}"
    "function verParts(v){const p=String(v||'').trim().split('.').map(x=>parseInt(x,10)||0);"
    "while(p.length<3)p.push(0);return p;}"
    "function verGt(a,b){const A=verParts(a),B=verParts(b);"
    "for(let i=0;i<3;i++){if(A[i]!==B[i])return A[i]>B[i];}return false;}"
    "function peekBinVer(file){return new Promise((res,rej)=>{"
    "const fr=new FileReader();fr.onerror=()=>rej(fr.error);"
    "fr.onload=()=>{const buf=fr.result,dv=new DataView(buf);let ver=null;"
    "for(let i=0;i+256<=buf.byteLength;i+=4){"
    "if(dv.getUint32(i,true)===MAGIC){ver=new TextDecoder().decode(new Uint8Array(buf,i+0x10,32))"
    ".replace(/\\0.*/,'');break;}}res(ver);};"
    "fr.readAsArrayBuffer(file.slice(0,4096));});}"
    "function syncButtons(st){"
    "const w=(st==='writing'||st==='pulling'),rd=(st==='ready'),id=(st==='idle');"
    "up.disabled=uploadBusy||!fi.files.length||w;"
    "ab.disabled=uploadBusy||applyBusy||id;ap.disabled=uploadBusy||applyBusy||!rd;}"
    "async function refresh(){try{"
    "const r=await fetch('/api/ota/status');const j=await r.json();"
    "if(!j.ok){info.textContent='状态读取失败';info.className='err';return;}"
    "runVer=j.run_ver||'';"
    "info.className='';"
    "info.textContent='运行槽 '+j.run+' 版本 '+runVer+' → 写入 '+j.target+' | 状态 '+j.state"
    "+(j.pending_ver?(' 待切换版本 '+j.pending_ver):'');"
    "+((j.state==='pulling'||j.state==='writing')&&j.total>0?(' '+j.written+'/'+j.total+' 字节'):'');"
    "if(j.written>0&&j.total>0){fill.style.width=Math.min(100,Math.round(100*j.written/j.total))+'%';}"
    "else if(!uploadBusy&&!pullBusy){fill.style.width='0';}"
    "syncButtons(j.state);"
    "}catch(e){info.textContent='状态异常: '+e;info.className='err';}}"
    "setInterval(refresh,1500);refresh();"
    "fi.onchange=async()=>{pickedVer='';hint.textContent='';hint.className='hint';"
    "if(!fi.files.length){up.disabled=true;return;}"
    "try{const v=await peekBinVer(fi.files[0]);pickedVer=v||'';"
    "if(!pickedVer){hint.textContent='无法从文件中识别版本（仍尝试上传）';hint.className='err';}"
    "else if(runVer&&!verGt(pickedVer,runVer)){"
    "hint.textContent='文件版本 '+pickedVer+' 不高于当前 '+runVer+'，上传会被拒绝';hint.className='err';}"
    "else{hint.textContent='文件版本 '+pickedVer+'（可上传）';hint.className='okmsg';}"
    "}catch(e){hint.textContent='读取文件头失败: '+e;hint.className='err';}"
    "syncButtons('idle');up.disabled=uploadBusy||!fi.files.length;};"
    "up.onclick=async()=>{const file=fi.files[0];if(!file||uploadBusy)return;"
    "if(runVer&&pickedVer&&!verGt(pickedVer,runVer)){"
    "L('拒绝: 版本 '+pickedVer+' 须高于 '+runVer);return;}"
    "uploadBusy=true;syncButtons('writing');L('上传 '+file.name+' '+file.size+' 字节…');"
    "try{await fetch('/api/ota/abort',{method:'POST'});}catch(e){L('abort 警告: '+e);}"
    "await new Promise((resolve)=>{"
    "const xhr=new XMLHttpRequest();xhr.open('POST','/api/ota/upload');"
    "xhr.setRequestHeader('Content-Type','application/octet-stream');"
    "xhr.upload.onprogress=e=>{if(e.lengthComputable){"
    "fill.style.width=Math.round(100*e.loaded/e.total)+'%';}};"
    "xhr.onload=()=>{"
    "L('HTTP '+xhr.status+' '+xhr.responseText);"
    "if(xhr.status===409){L('提示: 请使用更高版本的 release 包（idf -Project project release &lt;ver&gt;）');}"
    "uploadBusy=false;refresh();resolve();};"
    "xhr.onerror=()=>{L('网络错误（连接中断或超时，大文件请耐心等待）');"
    "uploadBusy=false;refresh();resolve();};"
    "xhr.ontimeout=()=>{L('客户端超时');uploadBusy=false;refresh();resolve();};"
    "xhr.timeout=600000;xhr.send(file);});};"
    "ab.onclick=async()=>{if(uploadBusy)return;"
    "try{const r=await fetch('/api/ota/abort',{method:'POST'});L(await r.text());refresh();}"
    "catch(e){L(String(e));}};"
    "ap.onclick=async()=>{if(uploadBusy||applyBusy)return;"
    "try{const sr=await fetch('/api/ota/status');const sj=await sr.json();"
    "if(!sj.ok||sj.state!=='ready'){L('无法 apply：当前状态 '+(sj.state||'?')+'，请重新上传');refresh();return;}}"
    "catch(e){L('状态读取失败: '+e);return;}"
    "applyBusy=true;syncButtons('ready');L('确认重启…');"
    "try{const r=await fetch('/api/ota/apply',{method:'POST'});L(await r.text());refresh();}"
    "catch(e){L(String(e));applyBusy=false;refresh();}};"
    OTA_PAGE_CLOUD_PULL_JS
    "</script></body></html>";

static bool ota_http_write_allowed(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);
    struct sockaddr_storage peer;
    socklen_t               slen = (socklen_t)sizeof(peer);

    if (!net_wifi_is_softap_mode()) {
        return true;
    }
    if (fd < 0) {
        return false;
    }
    if (getpeername(fd, (struct sockaddr *)&peer, &slen) != 0) {
        return false;
    }
    if (peer.ss_family == AF_INET) {
        const struct sockaddr_in *in4 = (const struct sockaddr_in *)&peer;

        return net_wifi_softap_peer_ipv4_on_ap_subnet(in4->sin_addr.s_addr);
    }
    return false;
}

static esp_err_t ota_send_json_err(httpd_req_t *req, const char *http_status, const char *err_key, const char *detail)
{
    char body[256];

    (void)httpd_resp_set_status(req, http_status);
    (void)httpd_resp_set_type(req, "application/json");
    if ((detail != NULL) && (detail[0] != '\0')) {
        (void)snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\",\"detail\":\"%s\"}", err_key, detail);
    } else {
        (void)snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", err_key);
    }
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static const char *ota_state_str(ota_session_state_e st)
{
    switch (st) {
    case OTA_SESSION_WRITING:
        return "writing";
    case OTA_SESSION_READY:
        return "ready";
    case OTA_SESSION_PULLING:
        return "pulling";
    default:
        return "idle";
    }
}

static esp_err_t ota_page_get_handler(httpd_req_t *req)
{
    (void)httpd_resp_set_type(req, "text/html; charset=utf-8");
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, s_ota_page_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ota_status_get_handler(httpd_req_t *req)
{
    ota_status_t st;
    char         json[512];
    status_t     err;

    err = ota_get_status(&st);
    if (err != ESP_OK) {
        return ota_send_json_err(req, "500 Internal Server Error", "status", esp_err_to_name(err));
    }

    (void)snprintf(json, sizeof(json),
                   "{\"ok\":true,\"state\":\"%s\",\"run\":\"%s\",\"target\":\"%s\",\"run_ver\":\"%s\","
                   "\"pending_ver\":\"%s\",\"written\":%u,\"total\":%u}",
                   ota_state_str(st.state), st.run_label, st.target_label, st.run_version, st.pending_version,
                   (unsigned)st.written, (unsigned)st.expected_size);
    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ota_abort_post_handler(httpd_req_t *req)
{
    status_t err;

    if (!ota_http_write_allowed(req)) {
        return ota_send_json_err(req, "403 Forbidden", "forbidden", NULL);
    }

    err = ota_upload_abort();
    if (err != ESP_OK) {
        return ota_send_json_err(req, "500 Internal Server Error", "abort", esp_err_to_name(err));
    }

    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ota_apply_post_handler(httpd_req_t *req)
{
    status_t err;

    if (!ota_http_write_allowed(req)) {
        return ota_send_json_err(req, "403 Forbidden", "forbidden", NULL);
    }

    err = ota_apply();
    if (err == ESP_ERR_INVALID_STATE) {
        ota_status_t st;
        char         detail[64];

        if (ota_get_status(&st) == ESP_OK) {
            (void)snprintf(detail, sizeof(detail), "session not ready (state=%s)", ota_state_str(st.state));
            return ota_send_json_err(req, "409 Conflict", "apply", detail);
        }
    }
    return ota_send_json_err(req, "500 Internal Server Error", "apply", esp_err_to_name(err));
}

static esp_err_t ota_upload_post_handler(httpd_req_t *req)
{
    size_t     total;
    size_t     got;
    status_t   err;
    uint8_t   *chunk = NULL;
    uint8_t    head[OTA_HEADER_PEEK];
    int        rlen;

    if (!ota_http_write_allowed(req)) {
        return ota_send_json_err(req, "403 Forbidden", "forbidden", NULL);
    }

    if (req->content_len <= 0) {
        return ota_send_json_err(req, "411 Length Required", "need_content_length",
                                 "browser must send Content-Length");
    }

    total = (size_t)req->content_len;
    if (total > WEB_CTRL_OTA_UPLOAD_MAX_BYTES) {
        return ota_send_json_err(req, "413 Payload Too Large", "too_large", NULL);
    }

    ESP_LOGI(TAG, "upload start len=%u", (unsigned)total);

    chunk = (uint8_t *)malloc(OTA_HTTP_RECV_CHUNK);
    if (chunk == NULL) {
        return ota_send_json_err(req, "507 Insufficient Storage", "no_mem", NULL);
    }

    (void)ota_upload_abort();

    got = 0U;
    while (got < OTA_HEADER_PEEK && got < total) {
        size_t want = total - got;

        if (want > OTA_HEADER_PEEK - got) {
            want = OTA_HEADER_PEEK - got;
        }
        rlen = httpd_req_recv(req, (char *)(head + got), want);
        if (rlen == HTTPD_SOCK_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "recv timeout at header offset %u", (unsigned)got);
            (void)ota_upload_abort();
            free(chunk);
            return ota_send_json_err(req, "408 Request Timeout", "recv_timeout", NULL);
        }
        if (rlen < 0) {
            ESP_LOGW(TAG, "recv err %d at header", rlen);
            (void)ota_upload_abort();
            free(chunk);
            return ota_send_json_err(req, "400 Bad Request", "recv", NULL);
        }
        if (rlen == 0) {
            break;
        }
        got += (size_t)rlen;
    }

    if (got < 256U) {
        free(chunk);
        return ota_send_json_err(req, "400 Bad Request", "short_body", NULL);
    }

    err = ota_upload_begin(total, head, got);
    if (err != ESP_OK) {
        free(chunk);
        if (err == ESP_ERR_INVALID_VERSION) {
            return ota_send_json_err(req, "409 Conflict", "version",
                                     "new version must be greater than running firmware");
        }
        if (err == ESP_ERR_INVALID_STATE) {
            return ota_send_json_err(req, "503 Service Unavailable", "busy", NULL);
        }
        return ota_send_json_err(req, "400 Bad Request", "begin", esp_err_to_name(err));
    }

    while (got < total) {
        size_t want = total - got;

        if (want > OTA_HTTP_RECV_CHUNK) {
            want = OTA_HTTP_RECV_CHUNK;
        }
        rlen = httpd_req_recv(req, (char *)chunk, want);
        if (rlen == HTTPD_SOCK_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "recv timeout at %u/%u", (unsigned)got, (unsigned)total);
            (void)ota_upload_abort();
            free(chunk);
            return ota_send_json_err(req, "408 Request Timeout", "recv_timeout", NULL);
        }
        if (rlen < 0) {
            (void)ota_upload_abort();
            free(chunk);
            return ota_send_json_err(req, "400 Bad Request", "recv", NULL);
        }
        if (rlen == 0) {
            (void)ota_upload_abort();
            free(chunk);
            return ota_send_json_err(req, "400 Bad Request", "truncated", NULL);
        }
        err = ota_upload_write(chunk, (size_t)rlen);
        if (err != ESP_OK) {
            (void)ota_upload_abort();
            free(chunk);
            return ota_send_json_err(req, "500 Internal Server Error", "write", esp_err_to_name(err));
        }
        got += (size_t)rlen;
    }

    free(chunk);

    err = ota_upload_end();
    if (err != ESP_OK) {
        return ota_send_json_err(req, "500 Internal Server Error", "end", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "upload complete %u bytes", (unsigned)total);
    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true,\"state\":\"ready\"}", HTTPD_RESP_USE_STRLEN);
}

#if CONFIG_WEB_CTRL_OTA_CLOUD_PULL

static esp_err_t ota_http_read_post_body(httpd_req_t *req, char *body, size_t body_cap, size_t *out_len)
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
            return ESP_FAIL;
        }
        if (rlen == 0) {
            return ESP_FAIL;
        }
        total += (size_t)rlen;
    }
    *out_len = total;
    return ESP_OK;
}

static bool ota_json_skip_ws(const char **pp)
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

static bool ota_json_extract_quoted(const char *body, const char *key, char *out, size_t out_cap)
{
    const char *found = strstr(body, key);
    const char *colon;
    const char *p;
    size_t      o = 0U;

    if ((found == NULL) || (out_cap == 0U)) {
        return false;
    }
    colon = strchr(found + strlen(key), ':');
    if (colon == NULL) {
        return false;
    }
    p = colon + 1U;
    if (!ota_json_skip_ws(&p) || (*p != '"')) {
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
        out[o++] = *p++;
    }
    if (*p != '"') {
        return false;
    }
    out[o] = '\0';
    return true;
}

static bool ota_json_extract_bool(const char *body, const char *key, bool *out)
{
    const char *found = strstr(body, key);
    const char *colon;
    const char *p;

    if ((found == NULL) || (out == NULL)) {
        return false;
    }
    colon = strchr(found + strlen(key), ':');
    if (colon == NULL) {
        return false;
    }
    p = colon + 1U;
    if (!ota_json_skip_ws(&p)) {
        return false;
    }
    if (strncmp(p, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool ota_pull_allowed(httpd_req_t *req)
{
    (void)req;
    if (!net_wifi_sta_has_ipv4()) {
        return false;
    }
    return ota_http_write_allowed(req);
}

static esp_err_t ota_pull_post_handler(httpd_req_t *req)
{
    char              body[512];
    size_t            body_len = 0U;
    ota_pull_request_t pull;
    status_t          err;
    esp_err_t         herr;

    if (!ota_pull_allowed(req)) {
        if (!net_wifi_sta_has_ipv4()) {
            return ota_send_json_err(req, "503 Service Unavailable", "sta_required",
                                     "STA must have IPv4 to pull from cloud");
        }
        return ota_send_json_err(req, "403 Forbidden", "forbidden", NULL);
    }

    if (req->content_len <= 0) {
        return ota_send_json_err(req, "411 Length Required", "need_body", NULL);
    }
    if ((size_t)req->content_len >= sizeof(body)) {
        return ota_send_json_err(req, "413 Payload Too Large", "body_too_large", NULL);
    }

    herr = ota_http_read_post_body(req, body, sizeof(body), &body_len);
    if (herr != ESP_OK) {
        return ota_send_json_err(req, "400 Bad Request", "read_body", NULL);
    }
    body[body_len] = '\0';

    (void)memset(&pull, 0, sizeof(pull));
    if (!ota_json_extract_quoted(body, "\"manifest_url\"", pull.manifest_url, sizeof(pull.manifest_url))) {
        return ota_send_json_err(req, "400 Bad Request", "manifest_url", "manifest_url required");
    }
    if (ota_json_extract_quoted(body, "\"product\"", pull.product, sizeof(pull.product))) {
        /* optional */
    } else {
        (void)snprintf(pull.product, sizeof(pull.product), "%s", WEB_CTRL_OTA_PRODUCT);
    }
    (void)ota_json_extract_bool(body, "\"apply\"", &pull.apply_after_pull);

    ESP_LOGI(TAG, "pull request url=%s apply=%d", pull.manifest_url, (int)pull.apply_after_pull);

    err = ota_pull_start(&pull);
    if (err == ESP_ERR_INVALID_STATE) {
        return ota_send_json_err(req, "409 Conflict", "busy", "pull already in progress");
    }
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return ota_send_json_err(req, "503 Service Unavailable", "not_enabled", NULL);
    }
    if (err != ESP_OK) {
        return ota_send_json_err(req, "500 Internal Server Error", "pull", esp_err_to_name(err));
    }

    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true,\"state\":\"pulling\"}", HTTPD_RESP_USE_STRLEN);
}

#endif /* CONFIG_WEB_CTRL_OTA_CLOUD_PULL */

esp_err_t web_ctrl_ota_register(httpd_handle_t server)
{
    const httpd_uri_t uri_page = {
        .uri = "/ota",
        .method = HTTP_GET,
        .handler = ota_page_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t uri_status = {
        .uri = "/api/ota/status",
        .method = HTTP_GET,
        .handler = ota_status_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t uri_upload = {
        .uri = "/api/ota/upload",
        .method = HTTP_POST,
        .handler = ota_upload_post_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t uri_abort = {
        .uri = "/api/ota/abort",
        .method = HTTP_POST,
        .handler = ota_abort_post_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t uri_apply = {
        .uri = "/api/ota/apply",
        .method = HTTP_POST,
        .handler = ota_apply_post_handler,
        .user_ctx = NULL,
    };
    esp_err_t err;

    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = httpd_register_uri_handler(server, &uri_page);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register /ota failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &uri_status);
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_register_uri_handler(server, &uri_upload);
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_register_uri_handler(server, &uri_abort);
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_register_uri_handler(server, &uri_apply);
    if (err != ESP_OK) {
        return err;
    }
#if CONFIG_WEB_CTRL_OTA_CLOUD_PULL
    {
        const httpd_uri_t uri_pull = {
            .uri = "/api/ota/pull",
            .method = HTTP_POST,
            .handler = ota_pull_post_handler,
            .user_ctx = NULL,
        };
        err = httpd_register_uri_handler(server, &uri_pull);
        if (err != ESP_OK) {
            return err;
        }
    }
#endif

    ESP_LOGI(TAG, "OTA HTTP routes registered");
    return ESP_OK;
}

#else /* CONFIG_WEB_CTRL_OTA */

esp_err_t web_ctrl_ota_register(httpd_handle_t server)
{
    (void)server;
    return ESP_OK;
}

#endif /* CONFIG_WEB_CTRL_OTA */
