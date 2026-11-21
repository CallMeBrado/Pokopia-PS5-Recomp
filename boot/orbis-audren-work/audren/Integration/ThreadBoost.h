#pragma once

namespace nxrt::audren {
inline void BoostAudioThread() {}
class ScopedAudioLockBoost {};
class TimerResolutionScope {
  public:
    static void Acquire() {}
    static void Release() {}
};
}
