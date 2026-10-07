#pragma once
#include <cstdint>

namespace appearance {
enum class Phase { Starting, Unsupported, Ready, Pending, Prepared, MissingItems, HookFailed };
struct Status {
    Phase phase = Phase::Starting;
    uint32_t hero = 0, expected = 0, matched = 0, attempts = 0, views = 0;
    uint64_t revision = 0, rebuilds = 0, gathers = 0, lastSeenMs = 0, wearableLists = 0;
    uint32_t prepareMs = 0, commitMs = 0;
    uint32_t registered = 0, known = 0, unavailable = 0, resyncs = 0;
    // Animation choices the server made for its own model, and how many were
    // translated onto the replacement model the outfit draws instead.
    uint32_t animationsSeen = 0, animationsTranslated = 0;
    // Alternate forms (a persona's dragon) drawn in place of the server's classic one.
    uint32_t forms = 0;
    char lastAnimation[128]{};
    uint32_t profileMoved = 0;
    bool profileExact = false;
    char profileDetail[96]{};
    char baseModel[264]{}, selectedModel[264]{}, renderModel[264]{};
};
void InitializeNative();
void TickNativeDiagnostics();
Status NativeStatus();
const char* PhaseText(Phase phase);
}
