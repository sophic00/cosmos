#pragma once

#include "cosmos/time.hpp"

// Compatibility shim: VirtualClock now lives in cosmos/time.hpp with the full
// deterministic implementation (clock_gettime/gettimeofday/nanosleep/
// clock_nanosleep). This header is kept so existing injector tests including
// "cosmos/virtual_clock.hpp" keep resolving VirtualClock without duplication.
