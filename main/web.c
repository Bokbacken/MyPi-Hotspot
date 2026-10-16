#include "web.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "diag.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "logbuf.h"
#include "pintest.h"
#include "sdkconfig.h"

static const char *TAG = "web";

static httpd_handle_t s_server;
static volatile bool s_ota_busy;

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------

static const char INDEX_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>MyPi-Hotspot</title><style>"
"body{font-family:system-ui,sans-serif;margin:1em;background:#111;color:#ddd}"
"h1{font-size:1.3em}a{color:#6cf}table{border-collapse:collapse}"
"td{padding:2px 10px 2px 0;vertical-align:top}td:first-child{color:#999}"
"pre{background:#000;color:#9f9;padding:8px;height:45vh;overflow:auto;font-size:12px;white-space:pre-wrap}"
".bad{color:#f66}.ok{color:#6f6}</style></head><body>"
"<h1>MyPi-Hotspot status</h1><p><a href='/update'>Firmware update (OTA)</a> | <a href='/pins'>Wiring diagnostics</a> | "
"<a href='/api/status'>/api/status</a> | <a href='/api/log'>/api/log</a></p>"
"<table id='t'></table><h2 style='font-size:1.1em'>Last touch events</h2><pre id='touch' style='height:auto'></pre>"
"<h2 style='font-size:1.1em'>Log <label style='font-size:.8em'><input type='checkbox' id='follow' checked> follow</label></h2>"
"<pre id='log'></pre><script>"
"function fmtUp(s){let h=Math.floor(s/3600),m=Math.floor(s/60)%60;return h+'h '+m+'m '+(s%60)+'s'}"
"function row(k,v,c){return '<tr><td>'+k+'</td><td'+(c?' class='+c:'')+'>'+v+'</td></tr>'}"
"async function tick(){try{"
"let s=await (await fetch('/api/status')).json();"
"document.getElementById('t').innerHTML="
"row('Firmware',s.firmware)+row('ESP-IDF',s.idf)+row('Chip',s.chip)+row('Reset reason',s.reset_reason)+"
"row('Uptime',fmtUp(s.uptime_s))+row('Partition',s.partition)+"
"row('Heap free',(s.heap_free/1024).toFixed(1)+' KB (min '+(s.heap_min/1024).toFixed(1)+' KB)')+"
"row('PSRAM free',(s.psram_free/1024).toFixed(0)+' KB')+"
"row('Wi-Fi',s.wifi_state+' (SSID '+s.ssid+', retries '+s.wifi_retries+')',s.wifi_state=='connected'?'ok':'bad')+"
"row('IP',s.ip)+row('RSSI',s.rssi+' dBm')+row('ESP-Hosted',s.hosted)+"
"row('Display',s.display_status,s.display_status.startsWith('ok')?'ok':'bad')+"
"row('SPI clock',(s.spi_clock_hz/1e6)+' MHz')+row('LCD ID read',s.lcd_id)+"
"row('Frames flushed',s.frames_flushed+' (errors '+s.flush_errors+')')+"
"row('Touch',s.touch_status,s.touch_status.startsWith('ok')?'ok':'bad')+row('Touch events',s.touch_events)+"
"row('OTA',s.ota_status);"
"document.getElementById('touch').textContent=s.touch_history.join('\\n')||'(none yet)';"
"let l=await (await fetch('/api/log')).text();let p=document.getElementById('log');p.textContent=l;"
"if(document.getElementById('follow').checked)p.scrollTop=p.scrollHeight;"
"}catch(e){document.getElementById('t').innerHTML=row('Error','device unreachable: '+e,'bad')}}"
"tick();setInterval(tick,2000);</script></body></html>";

static const char UPDATE_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>MyPi-Hotspot OTA</title><style>body{font-family:system-ui,sans-serif;margin:1em;background:#111;color:#ddd}"
"a{color:#6cf}progress{width:100%;height:1.5em}</style></head><body>"
"<h1 style='font-size:1.3em'>Firmware update (OTA)</h1>"
"<p>Upload the <b>app image</b> <code>mypi_hotspot.bin</code> (a.k.a. <code>mypi-hotspot-*-ota.bin</code>), "
"<b>not</b> the merged full-flash image.</p>"
"<input type='file' id='f' accept='.bin'> <button onclick='up()'>Upload &amp; flash</button>"
"<p><progress id='p' value='0' max='100'></progress></p><p id='s'></p><p><a href='/'>&larr; Status</a></p>"
"<script>function up(){let f=document.getElementById('f').files[0];let s=document.getElementById('s');"
"if(!f){s.textContent='Choose a file first';return}"
"let x=new XMLHttpRequest();x.open('POST','/update');x.setRequestHeader('Content-Type','application/octet-stream');"
"x.upload.onprogress=e=>{if(e.lengthComputable)document.getElementById('p').value=e.loaded/e.total*100};"
"x.onload=()=>{s.textContent=x.status+': '+x.responseText;if(x.status==200)setTimeout(()=>location.href='/',15000)};"
"x.onerror=()=>s.textContent='Upload failed (connection error)';"
"s.textContent='Uploading '+f.size+' bytes...';x.send(f)}</script></body></html>";

static const char PINS_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>MyPi-Hotspot pins</title><style>body{font-family:system-ui,sans-serif;margin:1em;background:#111;color:#ddd}"
"a{color:#6cf}pre{background:#000;color:#9f9;padding:8px;font-size:12px;white-space:pre-wrap}"
"td{padding:3px 8px}button{margin:0 2px}</style></head><body>"
"<h1 style='font-size:1.3em'>Wiring diagnostics</h1><p><a href='/'>&larr; Status</a></p>"
"<h2 style='font-size:1.1em'>Boot-time wiring check</h2><pre id='r'></pre>"
"<h2 style='font-size:1.1em'>Manual pin control</h2>"
"<p>Set a pin HIGH/LOW and measure it with a multimeter at the LCD module (against GND): HIGH should be ~3.3 V, LOW ~0 V. "
"<b>This detaches the pin from SPI/I2C until reboot.</b></p>"
"<p><b>Logic level check:</b> set SCLK to <i>in</i> and measure SCLK against GND. ~3.3 V is correct; "
"~5 V means the module VCC is on 5 V - move it to 3V3 (the level translator pull-ups otherwise put 5 V on the ESP32-P4 pins).</p>"
"<table id='t'></table>"
"<p><button onclick=\"fetch('/api/reboot',{method:'POST'}).then(()=>document.getElementById('o').textContent='Rebooting...')\">Reboot</button></p>"
"<p id='o'></p><script>"
"fetch('/api/pins').then(r=>r.json()).then(j=>{document.getElementById('r').textContent=j.report;"
"document.getElementById('t').innerHTML=j.names.split(',').map(n=>'<tr><td>'+n+'</td><td>'+"
"['high','low','in','inpd'].map(m=>\"<button onclick=\\\"setp('\"+n+\"','\"+m+\"')\\\">\"+m+'</button>').join('')+"
"\"</td><td id='v_\"+n+\"'></td></tr>\").join('')});"
"function setp(n,m){fetch('/api/pin?name='+n+'&mode='+m).then(r=>r.text()).then(t=>document.getElementById('v_'+n).textContent=t)}"
"</script></body></html>";

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

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

static esp_err_t api_reboot_post(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "rebooting");
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
    free(st);

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
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGW(TAG, "Rebooting into new firmware");
    esp_restart();
}

static esp_err_t ota_fail(httpd_req_t *req, esp_ota_handle_t h, const char *status, const char *msg)
{
    if (h) esp_ota_abort(h);
    ESP_LOGE(TAG, "OTA failed: %s", msg);
    diag_set_ota_status("failed: %s", msg);
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

    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf, bufsize);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
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
        int pct = (int)(received * 100 / req->content_len);
        if (pct / 10 != last_pct / 10) {
            ESP_LOGI(TAG, "OTA %d%% (%u / %u)", pct, (unsigned)received, (unsigned)req->content_len);
            diag_set_ota_status("writing %d%%", pct);
            last_pct = pct;
        }
    }
    free(buf);

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
    httpd_resp_sendstr(req, "OK - image verified, rebooting into the new firmware...");
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

esp_err_t web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.recv_wait_timeout = 15;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 16;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] httpd_start: %s", esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/",           .method = HTTP_GET,  .handler = index_get },
        { .uri = "/api/status", .method = HTTP_GET,  .handler = status_get },
        { .uri = "/api/log",    .method = HTTP_GET,  .handler = log_get },
        { .uri = "/update",     .method = HTTP_GET,  .handler = update_get },
        { .uri = "/update",     .method = HTTP_POST, .handler = update_post },
        { .uri = "/pins",       .method = HTTP_GET,  .handler = pins_get },
        { .uri = "/api/pins",   .method = HTTP_GET,  .handler = api_pins_get },
        { .uri = "/api/pin",    .method = HTTP_GET,  .handler = api_pin_get },
        { .uri = "/api/reboot", .method = HTTP_POST, .handler = api_reboot_post },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
    ESP_LOGI(TAG, "[ OK ] HTTP server on port 80");
    return ESP_OK;
}
