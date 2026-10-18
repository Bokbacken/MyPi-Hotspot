#include "profiles.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "nvs.h"

static const char *TAG = "net";

// Same subnets as MyPi-Companion; 192.168.5.0/24 is reserved for Ethernet LAN.
const uint8_t AP_SUBNET_BASE[AP_SUBNET_COUNT][3] = {
    {192, 168, 4}, {192, 168, 42}, {10, 11, 0}, {172, 16, 4},
};

EXT_RAM_BSS_ATTR static sta_profile_t s_sta[STA_PROFILE_MAX];   // v0.9.0: 1.6 KB, not time-critical -> PSRAM
static ap_profile_t s_ap[AP_PROFILE_MAX];
static SemaphoreHandle_t s_lock;

#define NS_STA "staprof"
#define NS_AP  "approf"

static void lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
}
static void unlock(void) { xSemaphoreGive(s_lock); }

static void get_str(nvs_handle_t h, const char *key, char *out, size_t len)
{
    size_t l = len;
    if (nvs_get_str(h, key, out, &l) != ESP_OK) {
        out[0] = 0;
    }
}

static uint8_t get_u8(nvs_handle_t h, const char *key, uint8_t def)
{
    uint8_t v;
    return nvs_get_u8(h, key, &v) == ESP_OK ? v : def;
}

void profiles_load(void)
{
    lock();
    memset(s_sta, 0, sizeof(s_sta));
    memset(s_ap, 0, sizeof(s_ap));
    nvs_handle_t h;
    char k[12];
    if (nvs_open(NS_STA, NVS_READONLY, &h) == ESP_OK) {
        for (int i = 0; i < STA_PROFILE_MAX; i++) {
            sta_profile_t *p = &s_sta[i];
            snprintf(k, sizeof(k), "s%dv", i);
            p->valid = get_u8(h, k, 0);
            if (!p->valid) continue;
            snprintf(k, sizeof(k), "s%dssid", i); get_str(h, k, p->ssid, sizeof(p->ssid));
            snprintf(k, sizeof(k), "s%dpass", i); get_str(h, k, p->pass, sizeof(p->pass));
            snprintf(k, sizeof(k), "s%ddhcp", i); p->dhcp = get_u8(h, k, 1);
            snprintf(k, sizeof(k), "s%dip", i);   get_str(h, k, p->ip, sizeof(p->ip));
            snprintf(k, sizeof(k), "s%dsn", i);   get_str(h, k, p->mask, sizeof(p->mask));
            snprintf(k, sizeof(k), "s%dgw", i);   get_str(h, k, p->gw, sizeof(p->gw));
            snprintf(k, sizeof(k), "s%ddns", i);  get_str(h, k, p->dns, sizeof(p->dns));
        }
        nvs_close(h);
    }
    if (nvs_open(NS_AP, NVS_READONLY, &h) == ESP_OK) {
        for (int i = 0; i < AP_PROFILE_MAX; i++) {
            ap_profile_t *p = &s_ap[i];
            snprintf(k, sizeof(k), "a%dv", i);
            p->valid = get_u8(h, k, 0);
            if (!p->valid) continue;
            snprintf(k, sizeof(k), "a%dssid", i); get_str(h, k, p->ssid, sizeof(p->ssid));
            snprintf(k, sizeof(k), "a%dpass", i); get_str(h, k, p->pass, sizeof(p->pass));
            snprintf(k, sizeof(k), "a%dsub", i);  p->subnet = get_u8(h, k, 0) % AP_SUBNET_COUNT;
            snprintf(k, sizeof(k), "a%dnat", i);  p->nat = get_u8(h, k, 1);
            snprintf(k, sizeof(k), "a%dboot", i); p->on_boot = get_u8(h, k, 0);
        }
        nvs_close(h);
    }
    unlock();
    ESP_LOGI(TAG, "Profiles loaded: %d STA, AP boot slot %d", sta_profile_count(), ap_profile_boot_slot());
}

bool sta_profile_get(int slot, sta_profile_t *out)
{
    if (slot < 0 || slot >= STA_PROFILE_MAX) return false;
    lock();
    *out = s_sta[slot];
    unlock();
    return true;
}

bool ap_profile_get(int slot, ap_profile_t *out)
{
    if (slot < 0 || slot >= AP_PROFILE_MAX) return false;
    lock();
    *out = s_ap[slot];
    unlock();
    return true;
}

static bool valid_ip(const char *s)
{
    struct in_addr a;
    return s && s[0] && inet_aton(s, &a);
}

static esp_err_t write_sta(nvs_handle_t h, int i, const sta_profile_t *p)
{
    char k[12];
    esp_err_t e = ESP_OK;
    snprintf(k, sizeof(k), "s%dv", i);    e |= nvs_set_u8(h, k, p->valid);
    snprintf(k, sizeof(k), "s%dssid", i); e |= nvs_set_str(h, k, p->ssid);
    snprintf(k, sizeof(k), "s%dpass", i); e |= nvs_set_str(h, k, p->pass);
    snprintf(k, sizeof(k), "s%ddhcp", i); e |= nvs_set_u8(h, k, p->dhcp);
    snprintf(k, sizeof(k), "s%dip", i);   e |= nvs_set_str(h, k, p->ip);
    snprintf(k, sizeof(k), "s%dsn", i);   e |= nvs_set_str(h, k, p->mask);
    snprintf(k, sizeof(k), "s%dgw", i);   e |= nvs_set_str(h, k, p->gw);
    snprintf(k, sizeof(k), "s%ddns", i);  e |= nvs_set_str(h, k, p->dns);
    return e == ESP_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t write_ap(nvs_handle_t h, int i, const ap_profile_t *p)
{
    char k[12];
    esp_err_t e = ESP_OK;
    snprintf(k, sizeof(k), "a%dv", i);    e |= nvs_set_u8(h, k, p->valid);
    snprintf(k, sizeof(k), "a%dssid", i); e |= nvs_set_str(h, k, p->ssid);
    snprintf(k, sizeof(k), "a%dpass", i); e |= nvs_set_str(h, k, p->pass);
    snprintf(k, sizeof(k), "a%dsub", i);  e |= nvs_set_u8(h, k, p->subnet);
    snprintf(k, sizeof(k), "a%dnat", i);  e |= nvs_set_u8(h, k, p->nat);
    snprintf(k, sizeof(k), "a%dboot", i); e |= nvs_set_u8(h, k, p->on_boot);
    return e == ESP_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t sta_profile_save(int slot, const sta_profile_t *in)
{
    if (slot < 0 || slot >= STA_PROFILE_MAX || !in || !in->ssid[0]) return ESP_ERR_INVALID_ARG;
    size_t pl = strlen(in->pass);
    if (pl > 0 && pl < 8) return ESP_ERR_INVALID_ARG;
    if (!in->dhcp && (!valid_ip(in->ip) || !valid_ip(in->mask) || !valid_ip(in->gw))) return ESP_ERR_INVALID_ARG;
    sta_profile_t p = *in;
    p.valid = true;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_STA, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = write_sta(h, slot, &p);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        lock();
        s_sta[slot] = p;
        unlock();
        ESP_LOGI(TAG, "STA profile %d saved: \"%s\" (%s)", slot, p.ssid, p.dhcp ? "DHCP" : p.ip);
    }
    return err;
}

esp_err_t ap_profile_save(int slot, const ap_profile_t *in)
{
    if (slot < 0 || slot >= AP_PROFILE_MAX || !in || !in->ssid[0] || in->subnet >= AP_SUBNET_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t pl = strlen(in->pass);
    if (pl > 0 && pl < 8) return ESP_ERR_INVALID_ARG;
    ap_profile_t p = *in;
    p.valid = true;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_AP, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    lock();
    err = write_ap(h, slot, &p);
    if (err == ESP_OK && p.on_boot) {
        for (int i = 0; i < AP_PROFILE_MAX; i++) {
            if (i != slot && s_ap[i].on_boot) {
                ap_profile_t o = s_ap[i];
                o.on_boot = false;
                if (write_ap(h, i, &o) == ESP_OK) s_ap[i].on_boot = false;
            }
        }
    }
    if (err == ESP_OK) err = nvs_commit(h);
    if (err == ESP_OK) s_ap[slot] = p;
    unlock();
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "AP profile %d saved: \"%s\" subnet %d.%d.%d.0/24 nat=%d boot=%d", slot, p.ssid,
                 AP_SUBNET_BASE[p.subnet][0], AP_SUBNET_BASE[p.subnet][1], AP_SUBNET_BASE[p.subnet][2], p.nat,
                 p.on_boot);
    }
    return err;
}

static esp_err_t clear_valid(const char *ns, char prefix, int slot)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    char k[12];
    snprintf(k, sizeof(k), "%c%dv", prefix, slot);
    err = nvs_set_u8(h, k, 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t sta_profile_delete(int slot)
{
    if (slot < 0 || slot >= STA_PROFILE_MAX) return ESP_ERR_INVALID_ARG;
    esp_err_t err = clear_valid(NS_STA, 's', slot);
    if (err == ESP_OK) {
        lock();
        memset(&s_sta[slot], 0, sizeof(s_sta[slot]));
        unlock();
    }
    return err;
}

esp_err_t ap_profile_delete(int slot)
{
    if (slot < 0 || slot >= AP_PROFILE_MAX) return ESP_ERR_INVALID_ARG;
    esp_err_t err = clear_valid(NS_AP, 'a', slot);
    if (err == ESP_OK) {
        lock();
        memset(&s_ap[slot], 0, sizeof(s_ap[slot]));
        unlock();
    }
    return err;
}

int sta_profile_count(void)
{
    int n = 0;
    lock();
    for (int i = 0; i < STA_PROFILE_MAX; i++) n += s_sta[i].valid;
    unlock();
    return n;
}

int sta_profile_find(const char *ssid)
{
    int r = -1;
    lock();
    for (int i = 0; i < STA_PROFILE_MAX && r < 0; i++) {
        if (s_sta[i].valid && strcmp(s_sta[i].ssid, ssid) == 0) r = i;
    }
    unlock();
    return r;
}

int ap_profile_boot_slot(void)
{
    int r = -1;
    lock();
    for (int i = 0; i < AP_PROFILE_MAX && r < 0; i++) {
        if (s_ap[i].valid && s_ap[i].on_boot) r = i;
    }
    unlock();
    return r;
}
