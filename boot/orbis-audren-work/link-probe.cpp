#include "nxrt/log.h"
#include "ps4-pcm-stream.h"

namespace nxrt {
bool log_enabled(LogLevel) { return false; }
void logf(LogLevel, const char *, const char *, ...) {}
}

int main() {
    Ps4PcmStream *stream = ps4_pcm_create(nullptr, nullptr);
    if (!stream) {
        return 1;
    }
    int32_t result = ps4_pcm_start(stream);
    ps4_pcm_destroy(stream);
    return result < 0;
}
