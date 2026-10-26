// Execute the real reconnect worker with a simulated clock and BLE transport.
#include <Arduino.h>
#include <vector>
#include <type_traits>
#include "config.h"
#include "colors.h"
constexpr uint8_t BLE_ADDR_PUBLIC=0, BLE_ADDR_RANDOM=1;
constexpr int pdTRUE=1;
static uint32_t clockNow=0;
static int succeedsOn=-1;
static int rssiReads=0;
static std::vector<uint8_t> attempts;
static std::vector<uint32_t> attemptedAt;
struct Done {};
uint32_t millis() { return clockNow; }
uint32_t pdMS_TO_TICKS(uint32_t ms) { return ms; }
template<class A,class B> auto min(A a,B b) {
 using T=std::common_type_t<A,B>;
 return std::min(static_cast<T>(a),static_cast<T>(b));
}
struct NimBLEAddress {
 uint8_t type;
 NimBLEAddress(const std::string&,uint8_t t):type(t) {}
};
struct NimBLEUUID { explicit NimBLEUUID(uint16_t) {} };
struct NimBLERemoteCharacteristic {
 bool canWrite() { return true; }
 bool canWriteNoResponse() { return true; }
 bool writeValue(const uint8_t*,size_t,bool) { return true; }
} characteristic;
struct RemoteService {
 NimBLERemoteCharacteristic* getCharacteristic(NimBLEUUID) { return &characteristic; }
} service;
struct ConnInfo {
 uint16_t getConnInterval() const { return 24; }
 uint16_t getConnLatency() const { return 0; }
 uint16_t getConnTimeout() const { return 256; }
};
struct NimBLEClient {
 bool connected=false;
 bool isConnected() { return connected; }
 void disconnect() { connected=false; }
 bool connect(NimBLEAddress address,bool,bool,bool) {
  if(attempts.size()==6) throw Done{};
  const int index=attempts.size();
  attempts.push_back(address.type);
  attemptedAt.push_back(clockNow);
  connected=index==succeedsOn;
  return connected;
 }
 int getLastError() { return 13; }
 int getRssi() { ++rssiReads; return -65; }
 ConnInfo getConnInfo() { return {}; }
 RemoteService* getService(NimBLEUUID) { return &service; }
} client;
struct NimBLEClientCallbacks {
 virtual void onDisconnect(NimBLEClient*,int) {}
};
struct NimBLEUtils {
 static const char* returnCodeToString(int) { return "simulated error"; }
};
struct { unsigned getFreeHeap() { return 70000; } } ESP;
static NimBLEClient* bleClient=&client;
static NimBLERemoteCharacteristic* bleCharacteristic=nullptr;
static bool writeResponse=false;
static std::atomic<bool> bleDropped{false};
static void* stateQueue=nullptr;
static bool due(uint32_t now,uint32_t deadline) {
 return static_cast<int32_t>(now-deadline)>=0;
}
int xQueueReceive(void*,LightState*,uint32_t wait) {
 clockNow+=wait;
 // Lose the established link after the initial + periodic RSSI samples.
 if(clockNow>=6000) client.connected=false;
 return 0;
}
#include "ble-bridge.h"
int main(int argc,char** argv) {
 assert(argc==2);
 const std::string scenario=argv[1];
 if(scenario=="public") succeedsOn=0;
 else if(scenario=="random") succeedsOn=1;
 else assert(scenario=="discover");
 try { bleWorker(nullptr); } catch(const Done&) {}
 assert(attempts.size()==6);
 for(unsigned i=0;i<attempts.size();++i) {
  const uint8_t expected=succeedsOn<0?i%2:(i<=static_cast<unsigned>(succeedsOn)?i%2:succeedsOn);
  assert(attempts[i]==expected);
 }
 if(succeedsOn<0) {
  for(unsigned i=1;i<attemptedAt.size();++i) {
   const uint32_t expected=std::min<uint32_t>(750U<<(i-1),5000U);
   assert(attemptedAt[i]-attemptedAt[i-1]==expected);
  }
 }
 assert(succeedsOn<0?rssiReads==0:rssiReads>=2);
 static_cast<NimBLEClientCallbacks&>(events).onDisconnect(&client,520);
 assert(bleDropped.load() && !bleConnected.load());
}
