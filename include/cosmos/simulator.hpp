#pragma once

#include "cosmos/faults.hpp"
#include "cosmos/memory.hpp"
#include "cosmos/random.hpp"
#include <optional>
#include <utility>

namespace cosmos {

// Default fault-stream seed when a Simulator is built without an explicit one, so runs are
// reproducible out of the box. ASCII "Cosmos1".
inline constexpr uint64_t kDefaultUniverseSeed = 0x436F736D6F7331ULL;

// Placeholder injector: the slot exists and stays empty until P1 delivers the real engine.
struct NoInjector {};

// The injector is a template parameter because std::optional instantiates traits on its element,
// so the slot needs a complete type and the real FaultInjector does not exist until P1. Only the
// Simulator alias below is wired to the __wrap_* functions: every instantiation gets its own
// current_sim_, so another one is reachable through direct calls but never through a wrapped
// syscall.
template <typename Injector> class BasicSimulator {
  public:
    // The seed feeds the per-class fault sub-streams; the Memory stream drives OOM injection.
    explicit BasicSimulator(uint64_t seed = kDefaultUniverseSeed)
        : fault_rng_(fault_class_seed(seed, FaultClass::Memory)) {}

    ~BasicSimulator() {
        if (current_sim_ == this) {
            current_sim_ = nullptr;
        }
    }

    // Copying would leave two objects claiming the same thread_local slot, and destroying either
    // one would clear it while the other is still current.
    BasicSimulator(const BasicSimulator&) = delete;
    BasicSimulator& operator=(const BasicSimulator&) = delete;

    static BasicSimulator* current() { return current_sim_; }

    static bool has_current() { return current_sim_ != nullptr; }

    static void set_current(BasicSimulator* sim) { current_sim_ = sim; }

    TrackedHeap& heap() { return heap_; }
    const TrackedHeap& heap() const { return heap_; }

    FaultProfile& faults() { return faults_; }
    const FaultProfile& faults() const { return faults_; }

    void set_faults(const FaultProfile& f) { faults_ = f; }

    // The Memory fault sub-stream. Fault decisions that sample (see FaultProfile) draw from it;
    // endpoint probabilities must not.
    Rng& fault_rng() { return fault_rng_; }

    bool has_injector() const { return injector_.has_value(); }

    // Returns nullptr when the slot is empty. A pointer rather than a checked reference because
    // this header compiles into the caller: an assert would vanish under NDEBUG and leave a
    // disengaged optional being dereferenced, and throwing inside __wrap_malloc is not an option.
    Injector* injector_or_null() { return injector_ ? &*injector_ : nullptr; }
    const Injector* injector_or_null() const { return injector_ ? &*injector_ : nullptr; }

    template <typename... Args> Injector& emplace_injector(Args&&... args) {
        return injector_.emplace(std::forward<Args>(args)...);
    }

    void clear_injector() { injector_.reset(); }

  private:
    inline static thread_local BasicSimulator* current_sim_{nullptr};
    TrackedHeap heap_{};
    FaultProfile faults_{};
    Rng fault_rng_;
    std::optional<Injector> injector_{};
};

using Simulator = BasicSimulator<NoInjector>;

} // namespace cosmos
