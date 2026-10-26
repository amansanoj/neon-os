#pragma once
#include <NimBLEDevice.h>
#include <NimBLEUtils.h>
#include <atomic>
#include <string>
static bool sendFrame(const uint8_t (&frame)[9],const char *what){
 if(!bleClient || !bleClient->isConnected() || !bleCharacteristic)return false;
 bool ok=bleCharacteristic->writeValue(frame,9,writeResponse);
 
 Serial.printf("[BLE] %s: %s\n",what,ok?"write queued":"WRITE FAILED");
 return ok;
}
static bool sendPower(bool on){
 uint8_t f[9]={0x7E,0x04,0x04,(uint8_t)(on?0xF0:0),0,(uint8_t)(on?1:0),0xFF,0,0xEF};
 return sendFrame(f,on?"POWER ON":"POWER OFF");
}
static bool sendColor(const LightState&s){
 RGB c=s.rgb;
 uint8_t f[9]={0x7E,0x07,0x05,0x03,c.r,c.g,c.b,0x10,0xEF};
 Serial.printf("[BLE] RGB %u,%u,%u\n",c.r,c.g,c.b);
 return sendFrame(f,"COLOR");
}
static bool sendBrightness(uint8_t value){
 uint8_t f[9]={0x7E,0x04,0x01,(uint8_t)min((int)value,100),0x01,0xFF,0xFF,0,0xEF};
 Serial.printf("[BLE] Brightness %u%%\n",value);
 return sendFrame(f,"NATIVE DIM");
}
class BleEvents : public NimBLEClientCallbacks {
 void onDisconnect(NimBLEClient*,int reason) override {
  bleDropped.store(true);bleConnected.store(false);
  Serial.printf("[BLE] Disconnected reason=%d (0x%X): %s\n",
      reason,static_cast<unsigned>(reason),NimBLEUtils::returnCodeToString(reason));
 }
};
static BleEvents events;
static void logBleLink() {
 if(!bleClient->isConnected())return;
 const int rssi=bleClient->getRssi();
 // NimBLE returns zero if the RSSI read fails; do not report it as a strong link.
 if(rssi!=0)Serial.printf("[BLE] Link RSSI=%d dBm\n",rssi);
 else Serial.println("[BLE] Link RSSI unavailable");
}
static bool connectBle(uint8_t type){
 bleCharacteristic=nullptr;
 if(bleClient->isConnected())bleClient->disconnect();
 NimBLEAddress address(std::string(C::MAC),type);
 Serial.printf("[BLE] Connecting type=%u\n",type);
 if(!bleClient->connect(address,true,false,false)){
  const int error=bleClient->getLastError();
  Serial.printf("[BLE] Connect failed type=%u error=%d (0x%X): %s\n",
      type,error,static_cast<unsigned>(error),NimBLEUtils::returnCodeToString(error));
  return false;
 }
 auto *svc=bleClient->getService(NimBLEUUID((uint16_t)0xFFF0));
 if(svc)bleCharacteristic=svc->getCharacteristic(NimBLEUUID((uint16_t)0xFFF3));
 if(!bleCharacteristic || (!bleCharacteristic->canWrite() && !bleCharacteristic->canWriteNoResponse())){
  Serial.println("[BLE] Missing writable FFF0/FFF3");
  bleCharacteristic=nullptr;bleClient->disconnect();return false;
 }
 writeResponse=!bleCharacteristic->canWriteNoResponse();
 bleConnected.store(true);
 Serial.printf("[BLE] Connected, response=%d, free heap=%u\n",writeResponse,ESP.getFreeHeap());
 const auto info=bleClient->getConnInfo();
 Serial.printf("[BLE] Link interval=%.2f ms latency=%u timeout=%u ms\n",
     info.getConnInterval()*1.25,static_cast<unsigned>(info.getConnLatency()),
     static_cast<unsigned>(info.getConnTimeout())*10);
 logBleLink();
 return true;
}
static void bleWorker(void*){
 LightState desired{false,100,{255,255,255},0,0},applied{};
 bool linked=false,powerSynced=false,colorSynced=false,dimSynced=false;
 uint8_t addrType=BLE_ADDR_PUBLIC;
 bool addrTypeKnown=false;
 uint32_t retry=C::RETRY_MIN,nextConnect=0,nextWrite=0,nextLinkLog=0;
 for(;;){
  LightState incoming;
  if(xQueueReceive(stateQueue,&incoming,0)==pdTRUE)desired=incoming;
  uint32_t now=millis();
  if(linked && (bleDropped.exchange(false)||!bleClient->isConnected())){
   linked=false;bleConnected.store(false);bleCharacteristic=nullptr;
   powerSynced=colorSynced=dimSynced=false;
   nextConnect=now+C::RETRY_MIN;
  }
  if(!linked && due(now,nextConnect)){
   bleDropped.store(false);
   linked=connectBle(addrType);
   if(linked){addrTypeKnown=true;retry=C::RETRY_MIN;powerSynced=colorSynced=dimSynced=false;nextWrite=millis();nextLinkLog=millis()+C::BLE_LINK_LOG_MS;}
   else{
    bleConnected.store(false);
    // Discover the address type until the first successful connection, then
    // keep that type: a timeout does not change the peer's identity address.
    if(!addrTypeKnown)addrType=(addrType==BLE_ADDR_PUBLIC)?BLE_ADDR_RANDOM:BLE_ADDR_PUBLIC;
    nextConnect=millis()+retry;
    Serial.printf("[BLE] Reconnect in %lu ms\n",(unsigned long)retry);
    retry=min(retry*2UL,(uint32_t)C::RETRY_MAX);
   }
   continue;
  }
  if(linked && due(now,nextLinkLog)){
   logBleLink();
   nextLinkLog=millis()+C::BLE_LINK_LOG_MS;
  }
  if(linked && due(now,nextWrite)){
   bool attempt=false,ok=true;
   if(!powerSynced || applied.on!=desired.on){
    attempt=true;ok=sendPower(desired.on);
    if(ok){applied.on=desired.on;powerSynced=true;
     if(desired.on)colorSynced=dimSynced=false;}
   }else if(desired.on && (!colorSynced || !sameRgb(applied.rgb,desired.rgb))){
    attempt=true;ok=sendColor(desired);
    if(ok){applied.rgb=desired.rgb;colorSynced=true;dimSynced=false;}
   }else if(desired.on && (!dimSynced || applied.brightness!=desired.brightness)){
    attempt=true;ok=sendBrightness(desired.brightness);
    if(ok){applied.brightness=desired.brightness;dimSynced=true;}
   }
   if(attempt){
    nextWrite=millis()+C::WRITE_GAP;
    if(!ok){bleCharacteristic=nullptr;bleClient->disconnect();linked=false;
     bleConnected.store(false);powerSynced=colorSynced=dimSynced=false;nextConnect=millis()+C::RETRY_MIN;}
    continue;
   }
  }
  uint32_t waitMs=500,clockNow=millis();
  if(!linked && !due(clockNow,nextConnect))waitMs=min(waitMs,nextConnect-clockNow);
  else if(linked && !due(clockNow,nextWrite))waitMs=min(waitMs,nextWrite-clockNow);
  if(xQueueReceive(stateQueue,&incoming,pdMS_TO_TICKS(waitMs))==pdTRUE)desired=incoming;
 }
}
