#pragma once
#include <cstdint>
namespace NeonOta {
// Called after networking has been initialized; never blocks the control loop.
bool begin();
// The main loop owns validation, Preferences for light state, and reboot timing.
void tick(uint32_t now, bool applicationHealthy);
bool rebootRequested();
uint32_t stackHeadroom();
const char* identity();
}
