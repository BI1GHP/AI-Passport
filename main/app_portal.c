// main/app_portal.c —— HTTP 配置门户:配置表单、图片上传、录音下载。
//
// 路由:
//   GET  /                  配置表单(内嵌 JS,fetch /api/config 填充;图片经 canvas 缩放后上传)
//   GET  /api/config        当前配置 JSON
//   POST /api/save          表单保存(application/x-www-form-urlencoded)
//   POST /api/upload?name=logo|photo|qr_card|qr_qsl   原始 JPEG 正文,存 FAT
//   GET  /api/rec/list      录音文件 JSON 列表
//   GET  /rec/<file>        录音文件下载
#include "app_portal.h"

#include "app_config.h"
#include "app_net.h"
#include "app_store.h"
#include "app_weather.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "app_portal";

#define UPLOAD_MAX_BYTES (400 * 1024) // 相片/二维码上限(浏览器已缩放)
#define RECV_BUF 2048

static httpd_handle_t s_server;

// ---------------------------------------------------------------------------
// 表单页(中文)。每一项设置带独立保存按钮(后端 /api/save 支持部分字段更新)。
// ---------------------------------------------------------------------------
static const char INDEX_HTML[] =
"<!doctype html><html lang=zh><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>电子工牌设置</title><style>"
"body{font-family:sans-serif;background:#f2f4f8;margin:0;padding:12px;color:#222}"
"h1{font-size:18px}h2{font-size:15px;margin:14px 0 6px}"
".card{background:#fff;border-radius:10px;padding:10px 12px;margin-bottom:10px;"
"box-shadow:0 1px 3px rgba(0,0,0,.08)}"
".item{margin:10px 0}"
"label{display:block;font-size:13px;margin-bottom:2px;color:#555}"
"input,select{flex:1;box-sizing:border-box;padding:7px;border:1px solid #ccc;"
"border-radius:6px;font-size:14px;min-width:0}"
".row{display:flex;gap:6px;align-items:center}"
"button{background:#2f6fed;color:#fff;border:0;border-radius:8px;padding:8px 14px;"
"font-size:13px;white-space:nowrap}"
"button.up{margin-top:6px}"
".st{color:#0a7d2c;font-size:12px;min-width:44px}"
".rec{font-size:13px;margin:4px 0}"
"</style></head><body>"
"<h1>电子工牌设置</h1>"

"<div class=card><h2>工牌信息</h2>"
"<div class=item><label>姓名</label><div class=row><input id=name>"
"<button onclick=\"return sv('name')\">保存</button><span class=st id=m_name></span></div></div>"
"<div class=item><label>昵称</label><div class=row><input id=nickname>"
"<button onclick=\"return sv('nickname')\">保存</button><span class=st id=m_nickname></span></div></div>"
"<div class=item><label>公司</label><div class=row><input id=company>"
"<button onclick=\"return sv('company')\">保存</button><span class=st id=m_company></span></div></div>"
"<div class=item><label>部门</label><div class=row><input id=department>"
"<button onclick=\"return sv('department')\">保存</button><span class=st id=m_department></span></div></div>"
"<div class=item><label>呼号</label><div class=row><input id=callsign>"
"<button onclick=\"return sv('callsign')\">保存</button><span class=st id=m_callsign></span></div></div>"
"<div class=item><label>天气城市</label><div class=row>"
"<select id=city_sel>"
"<option value=>请选择城市</option>"
"<option value=39.9042,116.4074>北京</option>"
"<option value=31.2304,121.4737>上海</option>"
"<option value=23.1291,113.2644>广州</option>"
"<option value=22.5431,114.0579>深圳</option>"
"<option value=30.5728,104.0668>成都</option>"
"<option value=29.4316,106.9123>重庆</option>"
"<option value=30.2741,120.1551>杭州</option>"
"<option value=32.0603,118.7969>南京</option>"
"<option value=30.5928,114.3055>武汉</option>"
"<option value=34.3416,108.9398>西安</option>"
"<option value=31.2990,120.5853>苏州</option>"
"<option value=34.7466,113.6254>郑州</option>"
"<option value=28.2282,112.9388>长沙</option>"
"<option value=41.8057,123.4315>沈阳</option>"
"<option value=39.3434,117.3616>天津</option>"
"<option value=36.0671,120.3826>青岛</option>"
"<option value=24.4798,118.0894>厦门</option>"
"<option value=26.0745,119.2965>福州</option>"
"<option value=31.8206,117.2272>合肥</option>"
"<option value=25.0389,102.7183>昆明</option>"
"<option value=45.8038,126.5350>哈尔滨</option>"
"<option value=22.3193,114.1694>香港</option>"
"<option value=25.0330,121.5654>台北</option>"
"<option value=22.1987,113.5439>澳门</option>"
"</select>"
"<button onclick=\"return svCity()\">保存</button><span class=st id=m_city></span></div></div>"
"</div>"

"<div class=card><h2>图片上传</h2>"
"<div class=item><label>开机 Logo(120x120,开机画面显示)</label>"
"<input type=file id=fl accept='image/*'>"
"<button class=up onclick=\"return up('fl','logo')\">上传 Logo</button>"
"<span class=st id=m_logo></span></div>"
"<div class=item><label>证件照(100x134,首页工牌)</label>"
"<input type=file id=fp accept='image/*'>"
"<button class=up onclick=\"return up('fp','photo')\">上传证件照</button>"
"<span class=st id=m_photo></span></div>"
"<div class=item><label>电子名片二维码(240x240)</label>"
"<input type=file id=fc accept='image/*'>"
"<button class=up onclick=\"return up('fc','qr_card')\">上传名片二维码</button>"
"<span class=st id=m_qr_card></span></div>"
"<div class=item><label>电子 QSL 二维码(240x240)</label>"
"<input type=file id=fq accept='image/*'>"
"<button class=up onclick=\"return up('fq','qr_qsl')\">上传 QSL 二维码</button>"
"<span class=st id=m_qr_qsl></span></div></div>"

"<div class=card><h2>邮箱</h2>"
"<div class=item><label>服务商</label><div class=row>"
"<select id=mail_provider><option value=qq>QQ 邮箱</option>"
"<option value=163>163 邮箱</option><option value=gmail>Gmail</option>"
"<option value=outlook>Outlook</option></select>"
"<button onclick=\"return sv('mail_provider')\">保存</button><span class=st id=m_mail_provider></span></div></div>"
"<div class=item><label>邮箱地址</label><div class=row><input id=mail_user>"
"<button onclick=\"return sv('mail_user')\">保存</button><span class=st id=m_mail_user></span></div></div>"
"<div class=item><label>授权码/密码</label><div class=row><input id=mail_pass type=password>"
"<button onclick=\"return sv('mail_pass')\">保存</button><span class=st id=m_mail_pass></span></div></div>"
"<div style='font-size:12px;color:#888;margin-top:6px'>保存后在设备菜单「邮箱」查看新邮件</div></div>"

"<div class=card><h2>设备</h2>"
"<div class=item><label>皮肤主题</label><div class=row>"
"<select id=theme><option value=0>金属</option><option value=1>可爱</option>"
"<option value=2>商务</option></select>"
"<button onclick=\"return sv('theme')\">保存</button><span class=st id=m_theme></span></div></div>"
"<div class=item><label>播放音量(0-100)</label><div class=row>"
"<input id=volume type=number min=0 max=100>"
"<button onclick=\"return sv('volume')\">保存</button><span class=st id=m_volume></span></div></div>"
"</div>"

"<div class=card><h2>小智AI</h2>"
"<div class=item><label>小智AI服务器 OTA 地址(留空用官方)</label><div class=row>"
"<input id=xzurl placeholder='https://api.tenclass.net/xz/ota/'>"
"<button onclick=\"return sv('xzurl')\">保存</button><span class=st id=m_xzurl></span></div></div>"
"</div>"

"<div class=card><h2>录音文件</h2><span class=st id=m_rec></span><div id=recs>加载中…</div></div>"
"<div style='text-align:center;color:#999;font-size:12px;margin:14px 0'>"
"电子工牌 V1.01 · 制作者 BI1GHP</div>"

"<script>"
"function stm(k,t){const m=document.getElementById('m_'+k);"
"if(m){m.textContent=t;setTimeout(()=>{if(m.textContent===t)m.textContent=''},3000)}}"
"function sv(k){const e=document.getElementById(k);if(!e)return false;"
"const p=new URLSearchParams();p.append(k,e.value);"
"fetch('/api/save',{method:'POST',body:p})"
".then(r=>r.text()).then(t=>stm(k,t)).catch(()=>stm(k,'保存失败'));return false}"
"function svCity(){const s=document.getElementById('city_sel');if(!s.value)return false;"
"const q=s.value.split(',');const p=new URLSearchParams();"
"p.append('city',s.options[s.selectedIndex].text);"
"p.append('city_lat',q[0]);p.append('city_lon',q[1]);"
"fetch('/api/save',{method:'POST',body:p})"
".then(r=>r.text()).then(t=>stm('city',t)).catch(()=>stm('city','保存失败'));return false}"
"fetch('/api/config').then(r=>r.json()).then(c=>{"
"for(const k in c){const e=document.getElementById(k);if(e)e.value=c[k]}"
"if(c.city_lat&&c.city_lon){const s=document.getElementById('city_sel');"
"for(let i=0;i<s.options.length;i++){if(s.options[i].value===c.city_lat+','+c.city_lon){s.selectedIndex=i;break}}}});"
"function up(id,name){const fi=document.getElementById(id);"
"if(!fi.files[0])return false;const sizes={photo:[100,134],logo:[120,120]};"
"const sz=sizes[name]||[240,240];const w=sz[0],h=sz[1];"
"const img=new Image();img.onload=()=>{const cv=document.createElement('canvas');"
"cv.width=w;cv.height=h;const x=cv.getContext('2d');"
"const s=Math.max(w/img.width,h/img.height);"
"const dw=img.width*s,dh=img.height*s;"
"x.drawImage(img,(w-dw)/2,(h-dh)/2,dw,dh);"
"cv.toBlob(b=>fetch('/api/upload?name='+name,{method:'POST',"
"headers:{'Content-Type':'image/jpeg'},body:b})"
".then(r=>r.text()).then(t=>stm(name,t)).catch(()=>stm(name,'上传失败')),'image/jpeg',.88);};"
"img.src=URL.createObjectURL(fi.files[0]);return false}"
"function loadRecs(){fetch('/api/rec/list').then(r=>r.json()).then(a=>{"
"const d=document.getElementById('recs');"
"d.innerHTML=a.length?a.map(x=>`<div class=rec><a href='/rec/${x.n}'>${x.n}</a>"
" (${(x.s/1024).toFixed(1)} KB) <button onclick=\"return del('${x.n}')\">删除</button></div>`).join(''):'暂无录音';}).catch(()=>{})}"
"function del(n){if(!confirm('删除录音 '+n+' ?'))return false;"
"fetch('/api/rec/del?name='+encodeURIComponent(n),{method:'POST'})"
".then(r=>r.text()).then(t=>{stm('rec',t);loadRecs()}).catch(()=>stm('rec','删除失败'));return false}"
"loadRecs();"
"</script></body></html>";

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------
static void send_text(httpd_req_t *req, const char *text) {
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, text);
}

// 读取整个请求正文(Content-Length 或 chunked),成功返回已读字节数(<=cap)。
static int read_body(httpd_req_t *req, char *buf, int cap) {
    int total = 0;
    while (total < cap) {
        int n = httpd_req_recv(req, buf + total, cap - total);
        if (n <= 0) {
            if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
            return -1;
        }
        total += n;
        if (req->content_len > 0 && total >= req->content_len) break;
        if (n == 0) break;
    }
    return total;
}

// application/x-www-form-urlencoded 解码(in-place)。
static void url_decode(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            int v;
            sscanf(r + 1, "%2x", &v);
            *w++ = (char)v;
            r += 3;
        } else if (*r == '+') {
            *w++ = ' ';
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

// 从表单正文取值(解码在副本上进行,避免原地解码引入内嵌 '\0' 截断后续解析);返回是否有该键。
static bool form_get(char *body, const char *key, char *out, size_t out_len) {
    size_t klen = strlen(key);
    char *p = body;
    while (p && *p) {
        char *amp = strchr(p, '&');
        if (amp) *amp = '\0';
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            size_t seglen = strlen(p + klen + 1);
            char *tmp = malloc(seglen + 1);
            if (tmp) {
                memcpy(tmp, p + klen + 1, seglen + 1);
                url_decode(tmp);
                strlcpy(out, tmp, out_len);
                free(tmp);
            }
            if (amp) *amp = '&';
            return true;
        }
        if (amp) *amp = '&';
        p = amp ? amp + 1 : NULL;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 路由处理
// ---------------------------------------------------------------------------
static esp_err_t h_index(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store"); // 禁缓存,防止旧 JS 表单失效
    httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t h_config_get(httpd_req_t *req) {
    app_config_t *c = app_config();
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "name", c->name);
    cJSON_AddStringToObject(j, "nickname", c->nickname);
    cJSON_AddStringToObject(j, "company", c->company);
    cJSON_AddStringToObject(j, "department", c->department);
    cJSON_AddStringToObject(j, "callsign", c->callsign);
    cJSON_AddStringToObject(j, "city", c->city);
    cJSON_AddNumberToObject(j, "city_lat", c->lat);
    cJSON_AddNumberToObject(j, "city_lon", c->lon);
    cJSON_AddNumberToObject(j, "theme", c->theme);
    cJSON_AddStringToObject(j, "mail_provider", c->mail_provider);
    cJSON_AddStringToObject(j, "mail_user", c->mail_user);
    cJSON_AddStringToObject(j, "mail_pass", c->mail_pass);
    cJSON_AddStringToObject(j, "xzurl", c->xz_server);
    cJSON_AddNumberToObject(j, "volume", c->volume);
    char *text = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, text ? text : "{}");
    cJSON_free(text);
    return ESP_OK;
}

static esp_err_t h_save(httpd_req_t *req) {
    char *body = malloc(RECV_BUF);
    if (!body) return ESP_ERR_NO_MEM;
    int n = read_body(req, body, RECV_BUF - 1);
    if (n < 0) {
        free(body);
        send_text(req, "读取失败");
        return ESP_FAIL;
    }
    body[n] = '\0';

    app_config_t *c = app_config();
    char v[APP_CFG_PASS_LEN];
    if (form_get(body, "name", v, sizeof(v)))          strlcpy(c->name, v, sizeof(c->name));
    if (form_get(body, "nickname", v, sizeof(v)))      strlcpy(c->nickname, v, sizeof(c->nickname));
    if (form_get(body, "company", v, sizeof(v)))       strlcpy(c->company, v, sizeof(c->company));
    if (form_get(body, "department", v, sizeof(v)))    strlcpy(c->department, v, sizeof(c->department));
    if (form_get(body, "callsign", v, sizeof(v)))      strlcpy(c->callsign, v, sizeof(c->callsign));
    if (form_get(body, "city", v, sizeof(v)))          strlcpy(c->city, v, sizeof(c->city));
    { double d; if (form_get(body, "city_lat", v, sizeof(v)) && (d = atof(v))) c->lat = d; }
    { double d; if (form_get(body, "city_lon", v, sizeof(v)) && (d = atof(v))) c->lon = d; }
    if (form_get(body, "theme", v, sizeof(v))) {
        int t = atoi(v);
        c->theme = (t >= 0 && t <= 2) ? (uint8_t)t : 0;
    }
    if (form_get(body, "mail_provider", v, sizeof(v))) strlcpy(c->mail_provider, v, sizeof(c->mail_provider));
    if (form_get(body, "mail_user", v, sizeof(v)))     strlcpy(c->mail_user, v, sizeof(c->mail_user));
    if (form_get(body, "mail_pass", v, sizeof(v)))     strlcpy(c->mail_pass, v, sizeof(c->mail_pass));
    if (form_get(body, "xzurl", v, sizeof(v))) strlcpy(c->xz_server, v, sizeof(c->xz_server));
    if (form_get(body, "volume", v, sizeof(v))) {
        int vol = atoi(v);
        c->volume = (vol >= 0 && vol <= 100) ? (uint8_t)vol : 70;
    }
    free(body);
    esp_err_t e = app_config_save();
    ESP_LOGI(TAG, "门户保存配置: %s", esp_err_to_name(e));
    if (e == ESP_OK) app_weather_request_refresh(); // 城市可能已变,立即重取天气
    send_text(req, e == ESP_OK ? "已保存,设备端立即生效" : "保存失败");
    return e == ESP_OK ? ESP_OK : ESP_FAIL;
}

// POST /api/upload?name=logo|photo|qr_card|qr_qsl,正文为缩放后的 JPEG。
static esp_err_t h_upload(httpd_req_t *req) {
    char query[64] = { 0 };
    char name[16] = { 0 };
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK) {
        send_text(req, "缺少 name 参数");
        return ESP_FAIL;
    }
    const char *rel = NULL;
    if (strcmp(name, "logo") == 0)         rel = "logo.jpg";
    else if (strcmp(name, "photo") == 0)   rel = "photo.jpg";
    else if (strcmp(name, "qr_card") == 0) rel = "qr_card.jpg";
    else if (strcmp(name, "qr_qsl") == 0)  rel = "qr_qsl.jpg";
    else {
        send_text(req, "未知图片类型");
        return ESP_FAIL;
    }
    if (!app_store_ready()) {
        send_text(req, "存储不可用");
        return ESP_FAIL;
    }
    if (req->content_len > UPLOAD_MAX_BYTES) {
        send_text(req, "图片过大(上限 400KB)");
        return ESP_FAIL;
    }

    char path[64];
    app_store_path(path, sizeof(path), rel);
    FILE *f = fopen(path, "wb");
    if (!f) {
        send_text(req, "文件打开失败");
        return ESP_FAIL;
    }
    char *buf = malloc(RECV_BUF);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    int total = 0;
    bool ok = true;
    while (total < req->content_len) {
        int want = req->content_len - total;
        if (want > RECV_BUF) want = RECV_BUF;
        int n = httpd_req_recv(req, buf, want);
        if (n <= 0) {
            if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ok = false;
            break;
        }
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) {
            ok = false;
            break;
        }
        total += n;
    }
    free(buf);
    fclose(f);
    if (!ok) {
        remove(path);
        send_text(req, "上传中断,请重试");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "已保存 %s(%d 字节)", path, total);
    send_text(req, "上传成功,返回设备页面即可看到");
    return ESP_OK;
}

static bool rec_name_valid(const char *n) {
    if (!n[0] || strlen(n) > 48) return false;
    for (const char *p = n; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '_' || *p == '-')) return false;
    }
    return true;
}

static esp_err_t h_rec_list(httpd_req_t *req) {
    char dirpath[64];
    app_store_path(dirpath, sizeof(dirpath), "rec");
    cJSON *arr = cJSON_CreateArray();
    DIR *d = opendir(dirpath);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_type != DT_REG || !rec_name_valid(de->d_name)) continue;
            char full[128];
            snprintf(full, sizeof(full), "%.60s/%.48s", dirpath, de->d_name);
            struct stat st;
            if (stat(full, &st) != 0) continue;
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "n", de->d_name);
            cJSON_AddNumberToObject(o, "s", (double)st.st_size);
            cJSON_AddItemToArray(arr, o);
        }
        closedir(d);
    }
    char *text = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, text ? text : "[]");
    cJSON_free(text);
    return ESP_OK;
}

static esp_err_t h_rec_del(httpd_req_t *req) {
    char query[96] = { 0 };
    char name[56] = { 0 };
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK) {
        send_text(req, "缺少 name 参数");
        return ESP_FAIL;
    }
    if (!rec_name_valid(name)) {
        send_text(req, "文件名非法");
        return ESP_FAIL;
    }
    if (!app_store_ready()) {
        send_text(req, "存储不可用");
        return ESP_FAIL;
    }
    char rel[64];
    snprintf(rel, sizeof(rel), "rec/%.48s", name);
    char path[96];
    app_store_path(path, sizeof(path), rel);
    if (remove(path) != 0) {
        ESP_LOGW(TAG, "门户删除失败 %s", path);
        send_text(req, "删除失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "门户已删除录音 %s", name);
    send_text(req, "已删除");
    return ESP_OK;
}

static esp_err_t h_rec_get(httpd_req_t *req) {
    const char *name = req->uri + strlen("/rec/");
    if (!rec_name_valid(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
        return ESP_FAIL;
    }
    char rel[64];
    snprintf(rel, sizeof(rel), "rec/%.48s", name);
    char path[96];
    app_store_path(path, sizeof(path), rel);
    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "audio/wav");
    char disp[96];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%.48s\"", name);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    char *buf = malloc(RECV_BUF);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t n;
    esp_err_t ret = ESP_OK;
    while ((n = fread(buf, 1, RECV_BUF, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) {
            ret = ESP_FAIL;
            break;
        }
    }
    free(buf);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ret;
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
esp_err_t app_portal_start(void) {
    if (s_server) return ESP_OK;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 12;
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t e = httpd_start(&s_server, &cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务启动失败: %s", esp_err_to_name(e));
        return e;
    }
    const httpd_uri_t routes[] = {
        { .uri = "/",             .method = HTTP_GET,  .handler = h_index },
        { .uri = "/api/config",   .method = HTTP_GET,  .handler = h_config_get },
        { .uri = "/api/save",     .method = HTTP_POST, .handler = h_save },
        { .uri = "/api/upload",   .method = HTTP_POST, .handler = h_upload },
        { .uri = "/api/rec/list", .method = HTTP_GET,  .handler = h_rec_list },
        { .uri = "/api/rec/del",  .method = HTTP_POST, .handler = h_rec_del },
        { .uri = "/rec/*",        .method = HTTP_GET,  .handler = h_rec_get },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        e = httpd_register_uri_handler(s_server, &routes[i]);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "注册路由 %s 失败", routes[i].uri);
            httpd_stop(s_server);
            s_server = NULL;
            return e;
        }
    }
    char ip[20];
    app_net_sta_ip(ip, sizeof(ip));
    ESP_LOGI(TAG, "门户已启动: http://%s/", ip[0] ? ip : "<未联网>");
    return ESP_OK;
}

esp_err_t app_portal_stop(void) {
    if (!s_server) return ESP_OK;
    esp_err_t e = httpd_stop(s_server);
    s_server = NULL;
    return e;
}

bool app_portal_running(void) {
    return s_server != NULL;
}
