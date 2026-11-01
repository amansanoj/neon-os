#include <cassert>
#include <cstring>
#include <string>
#include "ota-policy.h"
#include "runtime-policy.h"
static std::string release() {
 return "NEON-OTA-1\nneon-esp32-wroom-4m\nstable\n2000001\n2.0.1\n4096\n"+
     std::string(64,'a')+"\nhttps://updates.example.com/neon/2000001.bin\n";
}
static bool parse(const std::string& s) { OtaPolicy::Manifest m; return OtaPolicy::parse(s.data(),s.size(),m); }
int main() {
 auto raw=release(); OtaPolicy::Manifest m;
 assert(OtaPolicy::parse(raw.data(),raw.size(),m));
 assert(m.sequence==2000001 && m.size==4096 && !strcmp(m.version,"2.0.1"));
 assert(OtaPolicy::eligible(m,2000000,2000000,0));
 assert(!OtaPolicy::eligible(m,2000001,0,0));
 assert(!OtaPolicy::eligible(m,2000000,2000002,0));
 assert(!OtaPolicy::eligible(m,2000000,0,2000001));
 for(const char* url : {"http://host/x","https://user@host/x","https://host:443/x",
     "https:///x","https://host/x#fragment","https://host/with space","https://host/\"bad"})
  assert(!OtaPolicy::httpsUrl(url));
 assert(OtaPolicy::httpsUrl("https://updates.example.com/v2/firmware.bin"));
 const std::string asset="https://release-assets.githubusercontent.com/asset?sig="+std::string(1200,'a');
 assert(!OtaPolicy::httpsUrl(asset.c_str())); // Still cap URLs inside signed metadata.
 assert(OtaPolicy::httpsUrl(asset.c_str(),4096)); // Temporary CDN redirects may be longer.
 assert(!parse(raw+"extra\n")); assert(!parse(raw.substr(0,raw.size()-1)));
 auto embedded=raw; embedded[10]=0; assert(!parse(embedded));
 for(const auto& replacement : {std::string("neon-esp32-s3-4m"),std::string("neon-esp32-wroom-8m")}) {
  auto bad=raw; bad.replace(bad.find(NEON_HARDWARE_ID),strlen(NEON_HARDWARE_ID),replacement);
  assert(!parse(bad));
 }
 for(const char* seq : {"0","-1","+1","01","4294967296","1.0"}) {
  auto bad=raw; bad.replace(bad.find("2000001"),7,seq); assert(!parse(bad));
 }
 for(const char* size : {"0","2031617","4294967296","-1"}) {
  auto bad=raw; bad.replace(bad.find("4096"),4,size); assert(!parse(bad));
 }
 auto wrongChannel=raw; wrongChannel.replace(wrongChannel.find("stable"),6,"preview");
 assert(!parse(wrongChannel));
 // Truncation at every byte is rejected, including in long URL/hash fields.
 for(size_t i=0;i<raw.size();++i)assert(!parse(raw.substr(0,i)));
 assert(!parse(std::string(2000,'x')));
 assert(Runtime::elapsed(0x80000010U,0,70)); // An idle timestamp older than 25 days.
 assert(Runtime::elapsed(10,UINT32_MAX-10,20));
 assert(!Runtime::elapsed(10,UINT32_MAX-10,22));
 Runtime::DeferredWrite writes;
 writes.changed(100); assert(!writes.due(5099)); assert(writes.due(5100));
 writes.committed(5100);
 writes.changed(5200); assert(!writes.due(60000)); assert(writes.due(65100));
 writes.committed(65100); assert(!writes.due(100000));
 Runtime::DeferredWrite rollover;
 rollover.changed(UINT32_MAX-2000); assert(!rollover.due(2998)); assert(rollover.due(3000));
}
