#pragma once

#include "cosmos/ledger.hpp"
#include <algorithm>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>

namespace cosmos {

namespace detail {

// Grouped by hand rather than through the stream's locale: the checkpoint is byte-identical output
// for the same seed, and a locale is ambient state the run does not control.
inline std::string grouped(uint64_t value) {
    std::string digits = std::to_string(value);
    for (size_t pos = digits.size(); pos > 3;) {
        pos -= 3;
        digits.insert(pos, ",");
    }
    return digits;
}

// Sub-millisecond fires are common once a real clock runs, and two of them 999us apart must not
// print as the same instant in the artifact whose only job is being read.
inline std::string format_time(Time at) {
    // Split on the magnitude, not the signed value: a negative remainder renders with a minus sign,
    // and the pad width below would underflow rather than print it. Negating via (n + 1) keeps
    // Time::min() in range.
    const bool negative = at.ns < 0;
    const uint64_t magnitude =
        negative ? static_cast<uint64_t>(-(at.ns + 1)) + 1 : static_cast<uint64_t>(at.ns);
    const uint64_t whole_ms = magnitude / 1'000'000;
    const uint64_t remainder_us = (magnitude % 1'000'000) / 1'000;

    std::string text = std::to_string(whole_ms);
    if (remainder_us != 0) {
        const std::string fraction = std::to_string(remainder_us);
        text += "." + std::string(3 - fraction.size(), '0') + fraction;
    }
    return negative ? "t=-" + text + "ms" : "t=" + text + "ms";
}

} // namespace detail

// §11.1's layout. Two deliberate departures from its sample line, both because the injector must
// not invent what it does not know: the wrapper owns the kind-to-errno mapping (§8.2), so the tail
// names the outcome rather than "→ ENOMEM"; and the run header needs a seed and an oracle verdict
// that only the P2 harness has.
template <typename Injector> void print_ledger(std::ostream& out, const Injector& injector) {
    const FaultLedger& ledger = injector.ledger();

    // The time column is sized to its own contents, so one long timestamp cannot shift every other
    // row's columns the way a fixed width would.
    size_t time_width = 9;
    for (const LedgerEntry& entry : ledger) {
        time_width = std::max(time_width, detail::format_time(entry.at).size() + 2);
    }

    std::ostringstream body;
    body << std::left;
    body << "Fault ledger:\n";
    for (const LedgerEntry& entry : ledger) {
        body << "  " << std::setw(static_cast<int>(time_width)) << detail::format_time(entry.at)
             << std::setw(8) << name_of(entry.fault_class) << " " << std::setw(20)
             << ("site=" + std::string(name_of(entry.site))) << " FIRED    drew=" << std::setw(4)
             << (entry.drew ? "yes" : "no") << " eligible #" << entry.eligible_index << " -> "
             << name_of(entry.outcome) << "\n";
    }
    if (ledger.size() == 0) body << "  (no faults fired)\n";
    if (ledger.dropped() > 0) {
        body << "  ... and " << detail::grouped(ledger.dropped()) << " more not recorded\n";
    }

    body << "\nPer-site counters: ";
    bool first = true;
    for (SiteId site : kAllSites) {
        const uint64_t eligible = injector.eligible_calls(site);
        if (eligible == 0) continue;
        body << (first ? " " : " \u00b7 ") << name_of(site)
             << " eligible=" << detail::grouped(eligible);
        if (injector.injections(site) > 0) {
            body << " fired=" << detail::grouped(injector.injections(site));
        }
        first = false;
    }
    if (first) body << " (none exercised)";
    body << "\n";

    // Formatted into a local stream so the caller's fill and adjustment flags come back unchanged;
    // std::left is sticky, and a harness printing after this should not inherit it.
    out << body.str();
}

} // namespace cosmos
