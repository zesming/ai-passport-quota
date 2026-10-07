#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/time.h>
typedef int esp_err_t;
enum { ESP_OK=0, ESP_FAIL=1, MALLOC_CAP_8BIT=1, WIFI_MODE_STA=1, WIFI_MODE_APSTA=2, WIFI_IF_STA=0, WIFI_IF_AP=1, WIFI_AUTH_OPEN=0, WIFI_AUTH_WPA2_PSK=3, WIFI_REASON_AUTH_FAIL=202, WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT=15 };
typedef void *QueueHandle_t;
typedef unsigned UBaseType_t;
typedef struct { unsigned char ssid[32], password[64]; struct {bool capable, required;} pmf_cfg; struct {int authmode;} threshold; } wifi_sta_config_t;
typedef struct { unsigned char ssid[32], password[64]; unsigned ssid_len,channel,max_connection; int authmode; } wifi_ap_config_t;
typedef union { wifi_sta_config_t sta; wifi_ap_config_t ap; } wifi_config_t;
typedef struct {unsigned char ssid[33];} wifi_ap_record_t;
typedef struct {uint32_t addr;} esp_ip4_addr_t;
typedef struct {esp_ip4_addr_t ip;} esp_netif_ip_info_t;
typedef struct {int unused;} esp_netif_t;
#define CONFIG_LWIP_SNTP_MAX_SERVERS 3
typedef struct {bool start; size_t num_of_servers; const char *servers[CONFIG_LWIP_SNTP_MAX_SERVERS];} esp_sntp_config_t;
#define ESP_SNTP_SERVER_LIST(...) { __VA_ARGS__ }
#define ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(count,list) ((esp_sntp_config_t){.start=true,.num_of_servers=(count),.servers=list})
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(ip) 192u,168u,1u,2u
#define ESP_LOGI(tag,fmt,...) do {(void)(tag);if(false)printf(fmt,__VA_ARGS__);}while(0)
int64_t esp_timer_get_time(void);
uint32_t esp_random(void);
size_t esp_get_free_heap_size(void);
size_t esp_get_minimum_free_heap_size(void);
size_t heap_caps_get_largest_free_block(int);
UBaseType_t uxTaskGetStackHighWaterMark(void*);
esp_netif_t *esp_netif_create_default_wifi_ap(void);
esp_netif_t *esp_netif_get_handle_from_ifkey(const char*);
esp_err_t esp_netif_get_ip_info(esp_netif_t*,esp_netif_ip_info_t*);
esp_err_t esp_wifi_set_mode(int);
esp_err_t esp_wifi_set_config(int,const wifi_config_t*);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t*);
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t*);
esp_err_t esp_netif_sntp_sync_wait(unsigned);
void esp_netif_sntp_deinit(void);
