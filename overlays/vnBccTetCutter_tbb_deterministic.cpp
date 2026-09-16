// Deterministic overlay for vnBccTetCutter_tbb.cpp.
//
// Defining _DEBUG before including the original source activates its serial
// fallback paths, bypassing tbb::parallel_for and the tbb::concurrent_*
// containers whose iteration order is nondeterministic on macOS/libc++
// (std::unordered_* ordering is platform-dependent too). Without this,
// identical replays produce run-to-run topology variation after cuts.
// The original source file itself is not modified.

#ifndef _DEBUG
#define _DEBUG
#define SKINFLAPS_CUTTER_OVERLAY_DEFINED_DEBUG
#endif

#include "vnBccTetCutter_tbb.cpp"

#ifdef SKINFLAPS_CUTTER_OVERLAY_DEFINED_DEBUG
#undef _DEBUG
#undef SKINFLAPS_CUTTER_OVERLAY_DEFINED_DEBUG
#endif
