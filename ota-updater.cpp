#include "ota-updater.h"
#include "ota-config.h"
#include "ota-policy.h"
#include "runtime-policy.h"
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <atomic>
#include <memory>
#include <cstring>
#include <strings.h>
#include <ctime>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_ota_ops.h>
#include <esp_random.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "NeonOS requires an OTA-rollback-enabled ESP-IDF bootloader"
#endif
#define NEON_STR_IMPL(x) #x
#define NEON_STR(x) NEON_STR_IMPL(x)
namespace NeonOta {
namespace {
// Keep the integer macro free of a U suffix so the artifact identity is portable.
const char releaseIdentity[]="NEON_RELEASE_ID|" NEON_HARDWARE_ID "|"
    NEON_STR(NEON_FIRMWARE_SEQUENCE) "|" NEON_FIRMWARE_VERSION "|" NEON_RELEASE_CHANNEL "|";
constexpr uint32_t CHECK_INTERVAL_MS=15*60*1000, TRANSFER_LIMIT_MS=5*60*1000;
constexpr uint32_t TRIAL_LIMIT_MS=5*60*1000;
std::atomic<uint32_t> workerHeartbeat{0};
std::atomic<bool> endpointVerified{false}, readyToReboot{false};
bool configured=false, pending=false;
std::atomic<bool> validated{false};
uint32_t trialStarted=0, healthySince=0;
bool healthStarted=false,acceptanceAttempted=false;
uint32_t lastAcceptanceAttempt=0;
TaskHandle_t workerHandle=nullptr;
Preferences journal;
uint32_t highest=0, failed=0;

struct RedirectHeaders {
  char location[4097]{};
  bool invalid=false;
};
struct Client {
  esp_http_client_handle_t value=nullptr;
  std::unique_ptr<RedirectHeaders> headers;
  ~Client() { if(value)esp_http_client_cleanup(value); }
};
struct OtaSession {
  esp_ota_handle_t value=0;
  ~OtaSession() { if(value)esp_ota_abort(value); }
};
struct Key {
  mbedtls_pk_context value;
  Key() { mbedtls_pk_init(&value); }
  ~Key() { mbedtls_pk_free(&value); }
};
struct Sha {
  mbedtls_sha256_context value;
  Sha() { mbedtls_sha256_init(&value); }
  ~Sha() { mbedtls_sha256_free(&value); }
};
void beat() { workerHeartbeat.store(millis()); }
esp_http_client_config_t httpConfig(const char* url) {
  esp_http_client_config_t config{};
  config.url=url;
  config.crt_bundle_attach=esp_crt_bundle_attach;
  config.timeout_ms=10000;
  config.disable_auto_redirect=true; // Follow only explicitly checked HTTPS redirects below.
  config.transport_type=HTTP_TRANSPORT_OVER_SSL;
  config.buffer_size=4096; config.buffer_size_tx=1024;
  config.keep_alive_enable=false;
  return config;
}
esp_err_t responseHeader(esp_http_client_event_t* event) {
  if(event->event_id!=HTTP_EVENT_ON_HEADER || !event->user_data ||
      !event->header_key || strcasecmp(event->header_key,"Location"))return ESP_OK;
  auto* headers=static_cast<RedirectHeaders*>(event->user_data);
  if(!event->header_value || headers->location[0] ||
      strlen(event->header_value)>=sizeof(headers->location)) {
    headers->invalid=true; return ESP_FAIL;
  }
  strcpy(headers->location,event->header_value);
  return ESP_OK;
}
bool openDownload(Client& client,const char* initialUrl,int64_t& length) {
  if(!OtaPolicy::httpsUrl(initialUrl))return false;
  client.headers.reset(new(std::nothrow) RedirectHeaders);
  if(!client.headers)return false;
  strcpy(client.headers->location,initialUrl);
  const uint32_t started=millis();
  for(unsigned redirects=0;redirects<=4;++redirects) {
    beat();
    if(Runtime::elapsed(millis(),started,60000))return false;
    char* current=client.headers->location;
    if(!OtaPolicy::httpsUrl(current,4096))return false;
    const char* path=strchr(current+8,'/');
    const size_t originLength=static_cast<size_t>(path-current);
    char origin[264]; // https:// plus a DNS name of at most 253 characters.
    if(originLength>=sizeof(origin))return false;
    memcpy(origin,current,originLength); origin[originLength]=0;
    if(client.value) { esp_http_client_cleanup(client.value); client.value=nullptr; }
    auto config=httpConfig(current);
    config.event_handler=responseHeader;
    config.user_data=client.headers.get();
    // IDF copies URL components during init, before the header buffer is reused.
    client.value=esp_http_client_init(&config);
    current[0]=0; client.headers->invalid=false;
    if(!client.value || esp_http_client_set_header(client.value,"Accept-Encoding","identity")!=ESP_OK ||
        esp_http_client_open(client.value,0)!=ESP_OK)return false;
    length=esp_http_client_fetch_headers(client.value);
    const int status=esp_http_client_get_status_code(client.value);
    if(length<0 || client.headers->invalid)return false;
    if(status==200) {
      // Header callbacks can also occur for chunked trailers. Detach user_data
      // before freeing the redirect buffer, keeping the body read lifetime safe.
      if(esp_http_client_set_user_data(client.value,nullptr)!=ESP_OK)return false;
      client.headers.reset();
      return true;
    }
    if((status!=301 && status!=302 && status!=303 && status!=307 && status!=308) ||
        !*current || redirects==4)return false;
    if(current[0]=='/') {
      if(current[1]=='/')return false;
      const size_t relativeLength=strlen(current);
      if(originLength+relativeLength>4096)return false;
      memmove(current+originLength,current,relativeLength+1);
      memcpy(current,origin,originLength);
    }
    // Absolute targets must remain HTTPS, with no credentials, unsafe bytes,
    // explicit ports or fragments. GitHub's temporary asset URLs stay in RAM.
    if(!OtaPolicy::httpsUrl(current,4096))return false;
  }
  return false;
}
bool fetch(const char* url, uint8_t* buffer, size_t capacity, size_t& length) {
  length=0;
  if(!OtaPolicy::httpsUrl(url))return false;
  Client client;
  int64_t announced=0;
  if(!openDownload(client,url,announced) || announced>static_cast<int64_t>(capacity))return false;
  const uint32_t started=millis();
  for(;;) {
    beat();
    if(Runtime::elapsed(millis(),started,30000))return false;
    uint8_t extra;
    const size_t room=capacity-length;
    const int n=esp_http_client_read(client.value,
        reinterpret_cast<char*>(room?buffer+length:&extra),room?room:1);
    if(n<0 || (!room && n>0))return false;
    if(n==0)return esp_http_client_is_complete_data_received(client.value) && length!=0;
    length+=static_cast<size_t>(n);
    vTaskDelay(1);
  }
}
bool verifyWithKey(const char* pem, const uint8_t (&digest)[32],
    const uint8_t* signature,size_t sigLength) {
  if(sigLength!=OtaPolicy::SIGNATURE_SIZE || !*pem)return false;
  Key key;
  if(mbedtls_pk_parse_public_key(&key.value,reinterpret_cast<const uint8_t*>(pem),strlen(pem)+1) ||
      !mbedtls_pk_can_do(&key.value,MBEDTLS_PK_RSA) || mbedtls_pk_get_bitlen(&key.value)!=3072)
    return false;
  mbedtls_pk_rsassa_pss_options options{MBEDTLS_MD_SHA256,32};
  return mbedtls_pk_verify_ext(MBEDTLS_PK_RSASSA_PSS,&options,&key.value,
      MBEDTLS_MD_SHA256,digest,sizeof(digest),signature,sigLength)==0;
}
bool verifySignature(const uint8_t* data,size_t length,const uint8_t* signature,size_t sigLength) {
  uint8_t digest[32];
  if(mbedtls_sha256(data,length,digest,0))return false;
  return verifyWithKey(NEON_OTA_PUBLIC_KEY_PEM,digest,signature,sigLength) ||
      verifyWithKey(NEON_OTA_NEXT_PUBLIC_KEY_PEM,digest,signature,sigLength);
}
bool manifest(OtaPolicy::Manifest& release) {
  // One atomic CDN object: exact manifest bytes followed by its 384-byte
  // signature. No race between separately cached manifest/signature objects.
  constexpr size_t capacity=OtaPolicy::MAX_MANIFEST+OtaPolicy::SIGNATURE_SIZE;
  std::unique_ptr<uint8_t[]> envelope(new(std::nothrow) uint8_t[capacity]);
  if(!envelope)return false;
  size_t length=0;
  if(!fetch(NEON_OTA_MANIFEST_URL,envelope.get(),capacity,length) ||
      length<=OtaPolicy::SIGNATURE_SIZE)return false;
  const size_t bodyLength=length-OtaPolicy::SIGNATURE_SIZE;
  if(!verifySignature(envelope.get(),bodyLength,envelope.get()+bodyLength,
      OtaPolicy::SIGNATURE_SIZE) ||
      !OtaPolicy::parse(reinterpret_cast<const char*>(envelope.get()),bodyLength,release))return false;
  endpointVerified.store(true);
  return true;
}
bool imageMatches(const esp_partition_t* target,const OtaPolicy::Manifest& release) {
  Sha hash;
  if(mbedtls_sha256_starts(&hash.value,0))return false;
  uint8_t block[1024];
  char marker[160];
  snprintf(marker,sizeof(marker),"NEON_RELEASE_ID|%s|%lu|%s|%s|",NEON_HARDWARE_ID,
      static_cast<unsigned long>(release.sequence),release.version,NEON_RELEASE_CHANNEL);
  const size_t markerLength=strlen(marker);
  // Match across block boundaries. Marker starts with N, which does not recur
  // anywhere else in its fixed prefix except at the start; use a small overlap.
  char overlap[1184]{};
  size_t kept=0;
  bool identityFound=false;
  for(size_t offset=0;offset<release.size;) {
    beat();
    const size_t n=std::min<size_t>(sizeof(block),release.size-offset);
    if(esp_partition_read(target,offset,block,n)!=ESP_OK ||
        mbedtls_sha256_update(&hash.value,block,n))return false;
    memcpy(overlap+kept,block,n);
    const size_t total=kept+n;
    for(size_t i=0;i+markerLength<=total;++i)
      if(!memcmp(overlap+i,marker,markerLength))identityFound=true;
    kept=std::min(markerLength-1,total);
    memmove(overlap,overlap+total-kept,kept);
    offset+=n; vTaskDelay(1);
  }
  uint8_t digest[32]; char actual[65];
  if(mbedtls_sha256_finish(&hash.value,digest))return false;
  for(size_t i=0;i<sizeof(digest);++i)snprintf(actual+i*2,3,"%02x",digest[i]);
  return identityFound && strcmp(actual,release.sha256)==0;
}
bool install(const OtaPolicy::Manifest& release) {
  const esp_partition_t* running=esp_ota_get_running_partition();
  const esp_partition_t* target=esp_ota_get_next_update_partition(nullptr);
  if(!running || !target || running->address==target->address || release.size>target->size)
    return false;
  // OTA should not starve HomeKit/BLE of RAM. Defer instead of crashing on TLS
  // allocation failures. These thresholds also need measurement on the device.
  const size_t freeHeap=heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const size_t largest=heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if(freeHeap<50000 || largest<24576) {
    Serial.printf("[OTA] Deferred: heap=%u largest=%u\n",
        static_cast<unsigned>(freeHeap),static_cast<unsigned>(largest));
    return false;
  }
  // Use the native IDF streaming primitives directly. The convenience HTTPS
  // OTA wrapper performs its own redirect handling, even with auto-redirect
  // disabled. Here every status, byte count, deadline and boot switch is ours.
  Client client;
  int64_t announced=0;
  if(!openDownload(client,release.url,announced) ||
      announced!=static_cast<int64_t>(release.size) ||
      esp_http_client_is_chunked_response(client.value))return false;
  OtaSession session;
  if(esp_ota_begin(target,release.size,&session.value)!=ESP_OK)return false;
  uint8_t block[1024];
  uint32_t received=0;
  const uint32_t started=millis();
  while(received<release.size) {
    beat();
    if(Runtime::elapsed(millis(),started,TRANSFER_LIMIT_MS))return false;
    const size_t room=std::min<size_t>(sizeof(block),release.size-received);
    const int n=esp_http_client_read(client.value,reinterpret_cast<char*>(block),room);
    if(n<=0 || n>static_cast<int>(room) ||
        esp_ota_write(session.value,block,n)!=ESP_OK)return false;
    received+=n;
    vTaskDelay(1);
  }
  if(!esp_http_client_is_complete_data_received(client.value) || !imageMatches(target,release))
    return false;
  // IDF validates the ESP image format/checksum (and secure-boot signature when
  // provisioned) before the boot selector can ever point at this partition.
  const esp_err_t ended=esp_ota_end(session.value);
  session.value=0; // end frees its context on success and failure.
  if(ended!=ESP_OK)return false;
  // Journal before selecting the boot slot. A power loss in this narrow window
  // conservatively quarantines this release; it never overwrites the running app.
  if(journal.putUInt("pending",release.sequence)!=sizeof(uint32_t))return false;
  if(esp_ota_set_boot_partition(target)!=ESP_OK) {
    journal.remove("pending"); return false;
  }
  readyToReboot.store(true);
  return true;
}
void worker(void*) {
  beat();
  // TLS needs a correct clock for certificate expiry checks. HomeSpan owns
  // network provisioning; SNTP runs independently and never blocks the loop.
  configTime(0,0,"time.cloudflare.com","pool.ntp.org");
  uint32_t lastCheck=millis(),interval=10000;
  for(;;) {
    beat();
    if(WiFi.status()==WL_CONNECTED && time(nullptr)>1700000000 &&
        Runtime::elapsed(millis(),lastCheck,interval)) {
      lastCheck=millis();
      // Poll retries back off to the normal interval; jitter avoids a fleet
      // reconnecting at exactly the same instant after an outage.
      interval=std::min<uint32_t>(interval*2,CHECK_INTERVAL_MS)+(esp_random()%1000);
      if(!readyToReboot.load()) {
        OtaPolicy::Manifest release;
        if(manifest(release)) {
          interval=CHECK_INTERVAL_MS+(esp_random()%30000);
          if(validated.load() && OtaPolicy::eligible(release,NEON_FIRMWARE_SEQUENCE,highest,failed)) {
            if(!install(release))Serial.println("[OTA] Update deferred or rejected; current app retained");
          }
        } else Serial.println("[OTA] Release check failed; current app retained");
      }
    }
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}
}  // namespace

const char* identity() { return releaseIdentity; }
bool begin() {
  trialStarted=millis();
  esp_ota_img_states_t state=ESP_OTA_IMG_UNDEFINED;
  const auto* running=esp_ota_get_running_partition();
  if(!running)return false;
  const esp_err_t stateResult=esp_ota_get_state_partition(running,&state);
  // A fresh USB installation can have erased otadata: no journal entry exists
  // yet for app0. This is a valid initial boot, not an OTA initialization fault.
  if(stateResult!=ESP_OK && stateResult!=ESP_ERR_NOT_FOUND)return false;
  pending=state==ESP_OTA_IMG_PENDING_VERIFY;
  configured=OtaPolicy::httpsUrl(NEON_OTA_MANIFEST_URL) && strlen(NEON_OTA_PUBLIC_KEY_PEM)>64;
  if(!journal.begin("neon-ota",false))return false;
  highest=journal.getUInt("highest",0); failed=journal.getUInt("failed",0);
  const uint32_t priorPending=journal.getUInt("pending",0);
  if(priorPending && priorPending!=NEON_FIRMWARE_SEQUENCE) {
    if(journal.putUInt("failed",priorPending)!=sizeof(uint32_t))return false;
    failed=priorPending; journal.remove("pending");
    Serial.println("[OTA] Failed trial release quarantined");
  }
  if(pending && (!configured || priorPending!=NEON_FIRMWARE_SEQUENCE)) {
    Serial.println("[OTA] Missing trust configuration or trial journal; rolling back");
    esp_ota_mark_app_invalid_rollback_and_reboot(); return false;
  }
  if(!configured) {
    Serial.println("[OTA] Disabled: configure the release signing public key before mounting");
    return true;
  }
  Key key;
  if(mbedtls_pk_parse_public_key(&key.value,reinterpret_cast<const uint8_t*>(NEON_OTA_PUBLIC_KEY_PEM),
      strlen(NEON_OTA_PUBLIC_KEY_PEM)+1) || !mbedtls_pk_can_do(&key.value,MBEDTLS_PK_RSA) ||
      mbedtls_pk_get_bitlen(&key.value)!=3072)return false;
  if(*NEON_OTA_NEXT_PUBLIC_KEY_PEM) {
    Key next;
    if(mbedtls_pk_parse_public_key(&next.value,
        reinterpret_cast<const uint8_t*>(NEON_OTA_NEXT_PUBLIC_KEY_PEM),
        strlen(NEON_OTA_NEXT_PUBLIC_KEY_PEM)+1) ||
        !mbedtls_pk_can_do(&next.value,MBEDTLS_PK_RSA) ||
        mbedtls_pk_get_bitlen(&next.value)!=3072)return false;
  }
  beat();
  return xTaskCreatePinnedToCore(worker,"neon-ota",12288,nullptr,1,&workerHandle,1)==pdPASS;
}
void tick(uint32_t now,bool applicationHealthy) {
  if(configured && Runtime::elapsed(now,workerHeartbeat.load(),120000)) {
    Serial.println("[OTA] Worker stalled; restarting"); ESP.restart();
  }
  if(validated.load())return;
  // Enforce the deadline independently of success-path journal/validation
  // errors; otherwise repeated flash failures could leave a trial pending forever.
  if(pending && Runtime::elapsed(now,trialStarted,TRIAL_LIMIT_MS)) {
    Serial.println("[OTA] Trial health/endpoint check failed; rolling back");
    esp_ota_mark_app_invalid_rollback_and_reboot(); return;
  }
  if(applicationHealthy) {
    if(!healthStarted) { healthySince=now; healthStarted=true; }
  } else healthStarted=false;
  const bool healthy=healthStarted && Runtime::elapsed(now,healthySince,60000);
  if(healthy && (!configured || endpointVerified.load())) {
    if(acceptanceAttempted && !Runtime::elapsed(now,lastAcceptanceAttempt,5000))return;
    acceptanceAttempted=true; lastAcceptanceAttempt=now;
    // Commit the version floor before releasing the worker to install another
    // image. The atomic validated flag publishes these writes across tasks.
    const uint32_t floor=std::max<uint32_t>(highest,NEON_FIRMWARE_SEQUENCE);
    if(highest!=floor) {
      if(journal.putUInt("highest",floor)!=sizeof(uint32_t))return;
      highest=floor;
    }
    if(pending && esp_ota_mark_app_valid_cancel_rollback()!=ESP_OK)return;
    journal.remove("pending"); validated.store(true);
    // The release worker waits for this validation before offering updates.
    Serial.println("[OTA] Application health confirmed");
  }
}
bool rebootRequested() { return readyToReboot.load(); }
uint32_t stackHeadroom() {
  return workerHandle?static_cast<uint32_t>(uxTaskGetStackHighWaterMark(workerHandle)):0;
}
}  // namespace NeonOta
