// SPDX-License-Identifier: GPL-2.0-or-later
#include "mt_label_wait.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include "../mtgpu.h"

namespace MtLabelWait {
namespace {
std::mutex mutex;
std::condition_variable written;
std::atomic<unsigned> waiting{0};
/// The label the waiter sleeps on (one waiter: the render thread). With more than one waiting,
/// every write wakes them all.
std::atomic<std::uintptr_t> watched{0};
/// Writes that reached the watched label (tells a write that woke no one from a timely one).
std::atomic<unsigned long long> label_writes{0};
std::atomic<unsigned long long> waits{0}, wakeups{0}, missed{0};

/// The game's own test: the label's low 32 bits at or past the target, as a wrapping count.
bool Reached(const volatile void* label, std::uint32_t target) {
    return static_cast<std::int32_t>(*static_cast<const volatile std::uint32_t*>(label) - target) >= 0;
}
} // namespace

void Written(const void* address, std::size_t bytes) {
    // Pairs with the waiter's increment: either it reads the new label or this sees it waiting,
    // and the label it set before.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const unsigned count = waiting.load(std::memory_order_acquire);
    const bool label = watched.load(std::memory_order_relaxed) - reinterpret_cast<std::uintptr_t>(address) < bytes;
    if (label) {
        label_writes.fetch_add(1, std::memory_order_relaxed);
    }
    if (count == 0 || (count == 1 && !label)) {
        return;
    }
    // A waiter between its test and its wait holds the lock: the notify comes after that wait.
    { std::scoped_lock lock{mutex}; }
    written.notify_all();
}
} // namespace MtLabelWait

extern "C" void mtgpu_wait_label(const volatile void* label, uint32_t target) {
    using namespace MtLabelWait;
    waits.fetch_add(1, std::memory_order_relaxed);
    const auto address = reinterpret_cast<std::uintptr_t>(label);
    std::unique_lock lock{mutex};
    watched.store(address, std::memory_order_relaxed);
    waiting.fetch_add(1, std::memory_order_seq_cst);
    while (!Reached(label, target)) {
        const unsigned long long writes = label_writes.load(std::memory_order_relaxed);
        // A write that does not call Written() still ends the wait, up to 2 ms late; counted.
        const bool timeout = written.wait_for(lock, std::chrono::milliseconds(2)) == std::cv_status::timeout;
        wakeups.fetch_add(1, std::memory_order_relaxed);
        if (timeout && Reached(label, target) && label_writes.load(std::memory_order_relaxed) == writes) {
            missed.fetch_add(1, std::memory_order_relaxed);
        }
        // Another waiter may have left its own label here (then every write wakes both).
        watched.store(address, std::memory_order_relaxed);
    }
    waiting.fetch_sub(1, std::memory_order_relaxed);
}

extern "C" void mtgpu_label_wait_counts(unsigned long long* waits, unsigned long long* wakeups,
                                        unsigned long long* missed) {
    *waits = MtLabelWait::waits.load(std::memory_order_relaxed);
    *wakeups = MtLabelWait::wakeups.load(std::memory_order_relaxed);
    *missed = MtLabelWait::missed.load(std::memory_order_relaxed);
}
