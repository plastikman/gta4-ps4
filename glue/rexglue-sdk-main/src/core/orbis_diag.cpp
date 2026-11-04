#include <rex/platform.h>
#include <rex/ps5_diag.h>

#if REX_PLATFORM_PS4
namespace rex::ps5diag {
std::atomic<uint64_t> counters[kCount]{};
}  // namespace rex::ps5diag
#endif
