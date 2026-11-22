#pragma once

#define UXAIR_DEV_APP_ID "com.github.depius.uxair.dev"

// The development package has its own launcher identity, AirPlay name, and
// app-local state directory. Keep the ACB fallback ID in step with appinfo.json.
#ifdef UXAIR_DEV_BUILD
#define UXAIR_APP_ID UXAIR_DEV_APP_ID
#define UXAIR_DISPLAY_NAME "UxAir Dev"
#else
#define UXAIR_APP_ID "com.github.depius.uxair"
#define UXAIR_DISPLAY_NAME "UxAir"
#endif
