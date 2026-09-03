#pragma once

#include "cosmos/time.hpp"

namespace cosmos {

// Sample implementation to get virtual time flowing; the full implementation is pending. B's
// runtime branch replaces this whole header at P6 with the real clock — one that advances only on
// quiescence and drives the event queue — and the injector picks it up unchanged, because it only
// ever reads now().
class VirtualClock {
  public:
    Time now() const { return now_; }
    void advance(Duration d) { now_ += d; }
    void set(Time t) { now_ = t; }

  private:
    Time now_{Time::zero()};
};

} // namespace cosmos
