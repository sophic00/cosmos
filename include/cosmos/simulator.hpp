#pragma once

#include "cosmos/fault_injector.hpp"
#include "cosmos/faults.hpp"
#include "cosmos/finding.hpp"
#include "cosmos/memory.hpp"
#include "cosmos/random.hpp"
#include "cosmos/time.hpp"
#include "cosmos/trace.hpp"
#include <expected>
#include <optional>
#include <utility>
#include <vector>

namespace cosmos {

// Default fault-stream seed when a Simulator is built without an explicit one, so runs are
// reproducible out of the box. ASCII "Cosmos1".
inline constexpr uint64_t kDefaultUniverseSeed = 0x436F736D6F7331ULL;

// An injector carrying its own validating factory governs its construction, and the raw slot
// setters below are withdrawn for it: emplacing one directly would reopen the wrong-seed and
// second-install paths install_faults exists to close.
template <typename I>
concept HasFactory = requires { I::create; };

// Every instantiation gets its own current_sim_, so only the Simulator alias below is reachable
// through a wrapped syscall.
template <typename Injector> class BasicSimulator {
  public:
    // The seed is the universe seed: user-visible randomness derives via the User stream
    // (Rule 1), and the fault engine installed later derives its sub-streams from the Fault
    // domain (install_faults).
    explicit BasicSimulator(uint64_t seed = kDefaultUniverseSeed)
        : seed_(seed), user_rng_(stream_seed(seed, StreamDomain::User)) {
        // The universe's event stream starts at its seed, and every clock advance from here on
        // is hashed into trace_ (fault fires are hashed by the injector's ledger; see
        // trace_hash()).
        trace_.absorb_tag(static_cast<uint8_t>(TraceEvent::UniverseSeed));
        trace_.absorb_u64(seed_);
        clock_.attach_trace(&trace_);
    }

    // The universe seed this Simulator was built with. It is the repro key printed alongside
    // every campaign finding: same binary + same seed = the same execution (state-exploration.md
    // §8), so anything that varies per run must derive from this value, not from elsewhere.
    uint64_t seed() const { return seed_; }

    // The universe's trace hash (state-exploration.md §4, interim encoding): an incremental
    // FNV-1a over the canonical event stream. Component digests are maintained as events occur
    // — clock advances and the seed in trace_, fault fires in the injector's ledger — and only
    // the fold of the two happens at query time. This is what Campaign --verify compares between
    // an original run and its re-run: equal digests prove the executions were identical.
    uint64_t trace_hash() const {
        // The ledger component exists only when a fault engine is installed; a disengaged slot
        // contributes zero. if constexpr keeps the ledger() name out of factory-less
        // instantiations entirely.
        const uint64_t fault_digest = injector_ && (requires(const Injector& i) {
                                          i.ledger().trace_hash();
                                      }) ? injector_->ledger().trace_hash() : 0;
        return combine_trace_hashes(trace_.digest(), fault_digest);
    }

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

    // Low-level install. Prefer Scope — it restores the previous context on scope exit, which
    // manual set_current()/set_current(nullptr) pairs cannot (and leak when an early return
    // skips the reset). Kept available for the existing wrapper tests; the campaign loop and
    // its verify pass must go through Scope.
    static void set_current(BasicSimulator* sim) { current_sim_ = sim; }

    // RAII context guard: installs this universe as the thread's current, and restores the
    // previous pointer on scope exit — restore, not clear, so nested scopes unwind in LIFO
    // order. This is the documented sole context-management path for harness code: the campaign
    // loop installs one Scope per universe, and the verify pass runs sim then sim2 through
    // consecutive Scopes on the same worker thread (state-exploration.md §4, "never manual
    // set_current()").
    class Scope {
      public:
        explicit Scope(BasicSimulator& sim) : previous_(current_sim_) { current_sim_ = &sim; }
        ~Scope() { current_sim_ = previous_; }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        Scope(Scope&&) = delete;
        Scope& operator=(Scope&&) = delete;

      private:
        BasicSimulator* previous_;
    };

    TrackedHeap& heap() { return heap_; }
    const TrackedHeap& heap() const { return heap_; }

    VirtualClock& clock() { return clock_; }
    const VirtualClock& clock() const { return clock_; }

    Time now() const { return clock_.now(); }
    void advance_time(Duration d) { clock_.advance(d); }

    [[nodiscard]] std::expected<void, ConfigProblem> install_faults(FaultConfig cfg,
                                                                    uint32_t node_count = 1) {
        // A second install would fork stream positions, counters and the ledger against one run.
        if (injector_.has_value()) {
            return std::unexpected(
                ConfigProblem{ConfigError::InjectorAlreadyInstalled, std::nullopt});
        }
        // Seeding from the universe seed instead of the Fault domain would collide five of six
        // class sub-streams with the domain streams.
        auto made = Injector::create(std::move(cfg), stream_seed(seed_, StreamDomain::Fault),
                                     node_count, clock_);
        if (!made.has_value()) return std::unexpected(made.error());
        injector_.emplace(std::move(*made));
        return {};
    }

    // User-visible randomness (getrandom/random values). Never draws for fault decisions (Rule 1).
    Rng& user_rng() { return user_rng_; }
    const Rng& user_rng() const { return user_rng_; }

    // Records a harness finding (assert.hpp always()/COSMOS_ALWAYS, and later --verify
    // determinism violations). Contract: called after the wrapped call has returned — never
    // from inside a __wrap_* path — so the std::string materialization in Failure is safe.
    // Findings accumulate per universe here; the campaign layer merges worker shards (§4).
    void record_finding(const Failure& failure) { findings_.push_back(failure); }

    const std::vector<Failure>& findings() const { return findings_; }

    // Runs the universe until it is quiescent (state-exploration.md §4). Placeholder contract:
    // with no fiber scheduler yet, the workload function returning IS quiescence, so this is a
    // no-op — the campaign worker loop calls it unconditionally, which is the point: when the
    // scheduler lands (design.md §10), this grows into the full three-condition definition —
    // (1) ReadyQueue empty: no task is runnable; (2) virtual event queue empty: no timer
    // wakeups or I/O completions pending at any future virtual time; (3) no packets in flight —
    // and the campaign code does not change.
    void run_until_quiescence() {}

    bool has_injector() const { return injector_.has_value(); }

    // Returns nullptr when the slot is empty. A pointer rather than a checked reference because
    // this header compiles into the caller: an assert would vanish under NDEBUG and leave a
    // disengaged optional being dereferenced, and throwing inside __wrap_malloc is not an option.
    Injector* injector_or_null() { return injector_ ? &*injector_ : nullptr; }
    const Injector* injector_or_null() const { return injector_ ? &*injector_ : nullptr; }

    template <typename... Args>
        requires(!HasFactory<Injector>)
    Injector& emplace_injector(Args&&... args) {
        return injector_.emplace(std::forward<Args>(args)...);
    }

    void clear_injector()
        requires(!HasFactory<Injector>)
    {
        injector_.reset();
    }

  private:
    // thread_local on purpose: a universe belongs to one OS thread. Caveat until the fiber
    // scheduler lands and pthread_create is genuinely wrapped: threads spawned through the
    // current passthrough get their own empty slot on their own OS thread, so wrapped calls
    // made there fall through to the real host clock/heap instead of this universe. Keep
    // simulation workloads single-threaded until then, or real and virtual state will mix.
    inline static thread_local BasicSimulator* current_sim_{nullptr};
    uint64_t seed_;
    TrackedHeap heap_{};
    Rng user_rng_;
    TraceHash trace_;
    VirtualClock clock_{};
    // Declared after clock_ on purpose: the injector borrows it, and destruction is reverse order.
    std::vector<Failure> findings_;
    std::optional<Injector> injector_{};
};

using Simulator = BasicSimulator<BasicFaultInjector<VirtualClock>>;

} // namespace cosmos
