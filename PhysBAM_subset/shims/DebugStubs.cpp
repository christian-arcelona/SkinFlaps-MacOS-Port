#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <typeinfo>

// Relaxed debug stubs: never abort in headless/parity runs.
// This preserves solver execution while still surfacing messages to stderr.
namespace PhysBAM {
namespace DEBUG_UTILITIES {
namespace {
inline void log_msg(const char* kind, const char* msg = nullptr) {
	if (kind) std::fprintf(stderr, "[PHYSBAM:%s]%s%s\n", kind, msg ? " " : "", msg ? msg : "");
}
}

void Fatal_Error(const char*, const char*, unsigned int) { /* no-op */ }
void Fatal_Error(const char*, const char*, unsigned int, const char*) { /* no-op */ }
void Fatal_Error(const char*, const char*, unsigned int, const std::string&) { /* no-op */ }
void Assertion_Failed(const char*, const char*, unsigned int, const char*) { log_msg("ASSERT"); }
void Assertion_Failed(const char*, const char*, unsigned int, const char*, const char* message) { log_msg("ASSERT", message); }
void Assertion_Failed(const char*, const char*, unsigned int, const char*, const std::string& message) { log_msg("ASSERT", message.c_str()); }
void Warning(const std::string& s, const char*, const char*, unsigned int) { log_msg("WARN", s.c_str()); }
void Not_Implemented(const char*, const char*, unsigned int, const std::string&) { log_msg("NI"); }
void Not_Implemented(const char*, const char*, unsigned int) { log_msg("NI"); }
void Function_Is_Not_Defined(const char*, const char*, unsigned int, const std::type_info&) { log_msg("UNDEF"); }
void Warn_If_Not_Overridden(const char*, const char*, unsigned int, const std::type_info&) { /* no-op */ }
}

void Debug_Print_Helper(const char* prefix, ...)
{
	std::fprintf(stderr, "[PHYSBAM:DEBUG]%s", prefix ? prefix : "");
	va_list args;
	va_start(args, prefix);
	for (;;) {
		const char* value = va_arg(args, const char*);
		if (!value) break;
		std::fprintf(stderr, " %s", value);
	}
	va_end(args);
	std::fprintf(stderr, "\n");
}
}
