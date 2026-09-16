// Minimal no-op implementation of PhysBAM logging symbols used by level-set
// paths. This avoids pulling the full PhysBAM logging stack into headless
// parity binaries.

#include <PhysBAM_Tools/Log/LOG.h>
#include <iostream>

namespace PhysBAM {

namespace LOG_NULL {
log_null_class cout;
log_null_class cerr;
} // namespace LOG_NULL

namespace LOG {

LOG_CLASS::LOG_CLASS(const bool suppress_cout_input,
                     const bool suppress_cerr_input,
                     const bool suppress_timing_input,
                     const int verbosity_level_input,
                     const bool /*cache_initial_output*/,
                     const int threadid_input)
    : timer_singleton(nullptr)
    , timer_id(0)
    , suppress_cout(suppress_cout_input)
    , suppress_cerr(suppress_cerr_input)
    , threadid(threadid_input)
    , suppress_timing(suppress_timing_input)
    , log_file(nullptr)
    , verbosity_level(verbosity_level_input)
    , log_file_temporary(false)
    , xml(false)
    , root(nullptr)
    , current_entry(nullptr)
{}

LOG_CLASS::~LOG_CLASS() {}

void LOG_CLASS::Push_Scope(const std::string&, const std::string&, const int) {}
void LOG_CLASS::Pop_Scope(const int) {}
void LOG_CLASS::Time_Helper(const std::string&, const int) {}
void LOG_CLASS::Copy_Log_To_File(const std::string&, const bool, const int) {}

SCOPE::~SCOPE()
{
    if(active) Pop();
}

namespace {
LOG_CLASS g_log_instance(true, true, true, (1 << 30), false, 1);
}

LOG_CLASS* Instance(const int) { return &g_log_instance; }
std::ostream& cout_Helper() { return std::cout; }
std::ostream& cerr_Helper() { return std::cerr; }

void Initialize_Logging(const bool, const bool, const int, const bool, const int) {}
void Finish_Logging(const int) {}
void Stop_Time(const int) {}
void filecout(const std::string&, const int) {}
bool Check_Log_Initialized() { return true; }
void Stat_Helper(const std::string&, const std::stringstream&, const int) {}
void Reset(const int) {}
void Dump_Log(const int) {}

} // namespace LOG
} // namespace PhysBAM
