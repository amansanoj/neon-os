#pragma once
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#define CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 1
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_NOT_FOUND=7;
constexpr int ESP_ERR_HTTPS_OTA_IN_PROGRESS=99, HTTP_TRANSPORT_OVER_SSL=2;
constexpr int MALLOC_CAP_8BIT=1,WL_CONNECTED=1,pdPASS=1;
constexpr int ESP_OTA_IMG_UNDEFINED=0,ESP_OTA_IMG_PENDING_VERIFY=1;
using esp_ota_img_states_t=int;
using TaskHandle_t=void*;
inline uint32_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 8192; }
struct Restart {};
static uint32_t mockNow=0;
inline uint32_t millis() { return mockNow; }
inline void vTaskDelay(uint32_t ticks) { mockNow+=ticks; }
inline uint32_t pdMS_TO_TICKS(uint32_t ms) { return ms; }
inline int xTaskCreatePinnedToCore(void(*)(void*),const char*,int,void*,int,void**,int) { return pdPASS; }
inline uint32_t esp_random() { return 123; }
inline void configTime(int,int,const char*,const char*) {}
struct { template<class... T> void printf(const char*,T...) {} void println(const char*) {} } Serial;
struct { int status() { return WL_CONNECTED; } } WiFi;
struct { void restart() { throw Restart{}; } } ESP;
static bool failWrite=false,failFlashWrite=false,failBootSelect=false;
static uint32_t readDelay=0;
class Preferences {
 public:
  static inline std::map<std::string,uint32_t> values;
  bool begin(const char*,bool) { return true; }
  uint32_t getUInt(const char* key,uint32_t fallback=0) { return values.count(key)?values[key]:fallback; }
  size_t putUInt(const char* key,uint32_t value) {
    if(failWrite)return 0;
    values[key]=value; return 4;
  }
  bool remove(const char* key) { return values.erase(key)!=0; }
};
static size_t mockFreeHeap=100000;
inline size_t heap_caps_get_free_size(int) { return mockFreeHeap; }
inline size_t heap_caps_get_largest_free_block(int) { return mockFreeHeap; }
inline esp_err_t esp_crt_bundle_attach(void*) { return 0; }
constexpr int HTTP_EVENT_ON_HEADER=1;
struct esp_http_client_event_t {
 int event_id; const char* header_key; const char* header_value; void* user_data;
};
struct esp_http_client_config_t {
 const char* url=nullptr;
 esp_err_t(*event_handler)(esp_http_client_event_t*)=nullptr;
 void* user_data=nullptr;
 esp_err_t(*crt_bundle_attach)(void*)=nullptr;
 int timeout_ms=0; bool disable_auto_redirect=false;
 int transport_type=0,buffer_size=0,buffer_size_tx=0; bool keep_alive_enable=false;
};
struct HttpResponse { std::vector<uint8_t> bytes; int status=200; bool complete=true; std::string location; };
struct HttpClient { HttpResponse response; size_t pos=0;
 esp_err_t(*handler)(esp_http_client_event_t*)=nullptr; void* user_data=nullptr; };
using esp_http_client_handle_t=HttpClient*;
static std::map<std::string,HttpResponse> responses;
static int liveClients=0,totalClients=0;
inline HttpClient* esp_http_client_init(const esp_http_client_config_t* config) {
 assert(config->crt_bundle_attach && config->disable_auto_redirect);
 assert(config->transport_type==HTTP_TRANSPORT_OVER_SSL && config->timeout_ms==10000);
 ++liveClients; ++totalClients;
 return new HttpClient{responses[config->url],0,config->event_handler,config->user_data};
}
inline int esp_http_client_cleanup(HttpClient* client) { delete client; --liveClients; return 0; }
inline int esp_http_client_open(HttpClient*,int) { return ESP_OK; }
inline int esp_http_client_set_header(HttpClient*,const char*,const char*) { return ESP_OK; }
inline int esp_http_client_set_user_data(HttpClient* c,void* data) { c->user_data=data; return ESP_OK; }
inline int64_t esp_http_client_fetch_headers(HttpClient* c) {
 if(!c->response.location.empty() && c->handler) {
  esp_http_client_event_t event{HTTP_EVENT_ON_HEADER,"Location",c->response.location.c_str(),c->user_data};
  c->handler(&event);
 }
 return c->response.bytes.size();
}
inline int esp_http_client_get_status_code(HttpClient* c) { return c->response.status; }
inline int esp_http_client_read(HttpClient* c,char* dst,int room) {
 mockNow+=readDelay;
 size_t n=std::min<size_t>(room,c->response.bytes.size()-c->pos);
 if(n)memcpy(dst,c->response.bytes.data()+c->pos,n);
 c->pos+=n; return n;
}
inline bool esp_http_client_is_chunked_response(HttpClient*) { return false; }
inline bool esp_http_client_is_complete_data_received(HttpClient* c) { return c->response.complete; }
struct esp_partition_t { uint32_t address=0,size=0; };
static esp_partition_t runningPartition{0x10000,0x1F0000},targetPartition{0x200000,0x1F0000};
static std::vector<uint8_t> incomingImage,flashImage;
static int otaStateResult=ESP_ERR_NOT_FOUND,otaState=ESP_OTA_IMG_UNDEFINED;
static bool validatedImage=false,rolledBack=false,bootSelected=false;
static bool imageComplete=true,finishFails=false;
static int liveSessions=0,aborts=0;
inline const esp_partition_t* esp_ota_get_running_partition() { return &runningPartition; }
inline const esp_partition_t* esp_ota_get_next_update_partition(void*) { return &targetPartition; }
inline int esp_ota_get_state_partition(const esp_partition_t*,int* state) { *state=otaState; return otaStateResult; }
inline int esp_ota_mark_app_valid_cancel_rollback() { validatedImage=true; return ESP_OK; }
inline int esp_ota_mark_app_invalid_rollback_and_reboot() { rolledBack=true; throw Restart{}; }
inline int esp_partition_read(const esp_partition_t* p,size_t offset,void* output,size_t n) {
 assert(p==&targetPartition);
 if(offset+n>flashImage.size())return ESP_FAIL;
 memcpy(output,flashImage.data()+offset,n); return ESP_OK;
}
using esp_ota_handle_t=uint32_t;
inline int esp_ota_begin(const esp_partition_t* target,size_t size,esp_ota_handle_t* handle) {
 assert(target==&targetPartition && target!=&runningPartition && size<=target->size);
 *handle=1; ++liveSessions; flashImage.clear(); return ESP_OK;
}
inline int esp_ota_write(esp_ota_handle_t,const void* data,size_t n) {
 if(failFlashWrite)return ESP_FAIL;
 const auto* bytes=static_cast<const uint8_t*>(data);
 flashImage.insert(flashImage.end(),bytes,bytes+n); return ESP_OK;
}
inline int esp_ota_abort(esp_ota_handle_t) { --liveSessions; ++aborts; return ESP_OK; }
inline int esp_ota_end(esp_ota_handle_t) { --liveSessions; return finishFails?ESP_FAIL:ESP_OK; }
inline int esp_ota_set_boot_partition(const esp_partition_t* target) {
 assert(target==&targetPartition); if(failBootSelect)return ESP_FAIL;
 bootSelected=true; return ESP_OK;
}
