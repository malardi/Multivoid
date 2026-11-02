// coop/dev/rearm.cpp -- see coop/dev/rearm.h.

#include "coop/dev/rearm.h"

namespace coop::dev::rearm {
namespace {
std::atomic<uint32_t> g_epoch{0};
}  // namespace

uint32_t Epoch() { return g_epoch.load(std::memory_order_acquire); }

void Bump() { g_epoch.fetch_add(1, std::memory_order_acq_rel); }

}  // namespace coop::dev::rearm
