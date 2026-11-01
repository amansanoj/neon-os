#pragma once
// Local configuration is deliberately ignored by Git. Never embed private keys.
#if __has_include("ota-config.local.h") && !defined(NEON_OTA_MANIFEST_URL)
#include "ota-config.local.h"
#endif
#ifndef NEON_OTA_MANIFEST_URL
#define NEON_OTA_MANIFEST_URL "https://github.com/amansanoj/neon-os/releases/latest/download/release.ota"
#endif
#ifndef NEON_OTA_PUBLIC_KEY_PEM
#define NEON_OTA_PUBLIC_KEY_PEM ""
#endif

#ifndef NEON_OTA_NEXT_PUBLIC_KEY_PEM
#define NEON_OTA_NEXT_PUBLIC_KEY_PEM ""
#endif
