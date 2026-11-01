// Exercise the actual updater with deterministic ESP-IDF transport/flash mocks,
// linked to real Mbed TLS 3.6.6 for manifest signature and image hash validation.
#include <fstream>
#include <iterator>
#include "../ota-updater.cpp"
static std::vector<uint8_t> read(const std::string& path) {
 std::ifstream file(path,std::ios::binary);
 assert(file.good());
 return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}
int main(int argc,char** argv) {
 assert(argc==3);
 const std::string fixture=argv[1],scenario=argv[2];
 const std::string url=NEON_OTA_MANIFEST_URL;
 responses[url].bytes=read(fixture+"/release.ota");

 incomingImage=read(fixture+"/firmware.bin");
 if(scenario=="rotation-next")responses[url].bytes=read(fixture+"/release-next.ota");
 if(scenario=="wrong-key")responses[url].bytes=read(fixture+"/wrong-key.ota");
 if(scenario=="bad-signature")responses[url].bytes.back()^=1;
 if(scenario=="bad-manifest")responses[url].bytes[5]^=1;
 if(scenario=="oversize-signature")responses[url].bytes.push_back(0);
 if(scenario=="http-redirect")responses[url].status=302;
 if(scenario=="github-manifest") {
  const auto final=responses[url];
  const std::string tagged="https://github.com/amansanoj/neon-os/releases/download/v2.0.1/release.ota";
  const std::string asset="https://release-assets.githubusercontent.com/test/release.ota?sig="+std::string(1200,'a');
  responses[url]={}; responses[url].status=302;
  responses[url].location="/amansanoj/neon-os/releases/download/v2.0.1/release.ota";
  responses[tagged].status=302; responses[tagged].location=asset;
  responses[asset]=final;
 }
 if(scenario=="redirect-downgrade") { responses[url].status=302; responses[url].location="http://example.com/release.ota"; }
 if(scenario=="redirect-loop") { responses[url].status=302; responses[url].location=url; }
 if(scenario=="redirect-relative-host") { responses[url].status=302; responses[url].location="//example.com/release.ota"; }
 if(scenario=="redirect-too-long") { responses[url].status=302; responses[url].location="https://example.com/"+std::string(4096,'a'); }
 if(scenario=="truncated-manifest")responses[url].complete=false;
 if(scenario=="trial-healthy" || scenario=="trial-fails" || scenario=="trial-journal-fails") {
  otaStateResult=ESP_OK; otaState=ESP_OTA_IMG_PENDING_VERIFY;
  Preferences::values["pending"]=NEON_FIRMWARE_SEQUENCE;
 }
 if(scenario=="missing-trust-journal") { otaStateResult=ESP_OK; otaState=ESP_OTA_IMG_PENDING_VERIFY; }
 if(scenario=="quarantine")Preferences::values["pending"]=NEON_FIRMWARE_SEQUENCE+1;
 if(scenario=="missing-trust-journal") {
  try { NeonOta::begin(); assert(false); } catch(const Restart&) {}
  assert(rolledBack); return 0;
 }
 assert(NeonOta::begin()); // Fresh USB boot has no OTA state entry.
 if(scenario=="quarantine") {
  assert(NeonOta::failed==NEON_FIRMWARE_SEQUENCE+1);
  assert(!Preferences::values.count("pending")); return 0;
 }
 if(scenario=="trial-healthy") {
  NeonOta::endpointVerified.store(true);
  NeonOta::tick(0,true); mockNow=60000;
  NeonOta::workerHeartbeat.store(mockNow); NeonOta::tick(mockNow,true);
  assert(validatedImage && NeonOta::validated.load());
  assert(!Preferences::values.count("pending")); return 0;
 }
 if(scenario=="trial-journal-fails") {
  NeonOta::endpointVerified.store(true); NeonOta::tick(0,true);
  failWrite=true; mockNow=60000; NeonOta::workerHeartbeat.store(mockNow);
  NeonOta::tick(mockNow,true); assert(!validatedImage);
  mockNow=300001; NeonOta::workerHeartbeat.store(mockNow);
  try { NeonOta::tick(mockNow,true); assert(false); } catch(const Restart&) {}
  assert(rolledBack); return 0;
 }
 if(scenario=="trial-fails") {
  mockNow=300001; NeonOta::workerHeartbeat.store(mockNow);
  try { NeonOta::tick(mockNow,true); assert(false); } catch(const Restart&) {}
  assert(rolledBack && !validatedImage); return 0;
 }
 OtaPolicy::Manifest release;
 const bool validManifest=NeonOta::manifest(release);
 assert(liveClients==0);
 if(scenario=="redirect-loop")assert(totalClients<=5);
 if(scenario=="redirect-loop" || scenario=="redirect-downgrade" ||
     scenario=="redirect-relative-host" || scenario=="redirect-too-long" ||
     scenario=="wrong-key" || scenario=="bad-signature" || scenario=="bad-manifest" ||
     scenario=="oversize-signature" || scenario=="http-redirect" || scenario=="truncated-manifest") {
  assert(!validManifest && !bootSelected && !NeonOta::endpointVerified.load()); return 0;
 }
 assert(validManifest);
 assert(OtaPolicy::eligible(release,NEON_FIRMWARE_SEQUENCE,0,0));
 if(scenario=="bad-image")incomingImage[300]^=1;
 if(scenario=="truncated-image") { incomingImage.resize(incomingImage.size()-8); imageComplete=false; }
 if(scenario=="journal-fails")failWrite=true;
 if(scenario=="finish-fails")finishFails=true;
 if(scenario=="low-heap")mockFreeHeap=40000;
 if(scenario=="oversize-image")release.size=targetPartition.size+1;
 if(scenario=="same-partition")targetPartition.address=runningPartition.address;
 responses[release.url].bytes=incomingImage;
 responses[release.url].complete=imageComplete;
 if(scenario=="image-redirect")responses[release.url].status=302;
 if(scenario=="github-image") {
  const auto final=responses[release.url];
  const std::string asset="https://release-assets.githubusercontent.com/test/firmware.bin?sig="+std::string(1200,'a');
  responses[release.url]={}; responses[release.url].status=302; responses[release.url].location=asset;
  responses[asset]=final;
 }
 if(scenario=="flash-write-fails")failFlashWrite=true;
 if(scenario=="boot-select-fails")failBootSelect=true;
 if(scenario=="transfer-timeout")readDelay=100001;
 const bool installed=NeonOta::install(release);
 assert(liveClients==0 && liveSessions==0);
 if(scenario=="success" || scenario=="rotation-next" || scenario=="github-manifest" || scenario=="github-image") {
  assert(installed && bootSelected && NeonOta::rebootRequested());
  assert(Preferences::values["pending"]==release.sequence);
  assert(flashImage==incomingImage);
 } else {
  assert(!installed && !bootSelected && !NeonOta::rebootRequested());
  assert(!Preferences::values.count("pending"));
 }
}
