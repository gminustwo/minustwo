// minustwo: host threads that may call guest code (AvPlayer allocator callbacks).
// Each thread gets a guest TCB (GS base, TLS) from the C runtime before running.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <system_error>
#include <thread>
#include "common/types.h"

extern "C" void runtime_thread_attach_host(const char* name);

namespace Libraries::Kernel {
class Thread {
public:
    Thread() = default;
    ~Thread() { Stop(); }
    void Run(std::function<void(std::stop_token)>&& func) {
        std::scoped_lock lock{mutex};
        finished = std::make_shared<std::atomic<bool>>(false);
        thread = std::jthread([func = std::move(func), done = finished](std::stop_token stop) {
            runtime_thread_attach_host("minustwo:hle");
            func(stop);
            done->store(true);
        });
        id = thread.get_id();
        running = true;
    }
    // A thread may stop its own Thread object (AvPlayer does); it detaches instead of joining.
    // minustwo: two threads may join the same one (AvPlayer's Stop joins the decoders while the
    // demuxer, stopping, joins them too). Only one of them joins; the other waits for that join.
    // Two pthread_joins of one thread are undefined: glibc fails the second, macOS blocks it
    // for good (the movie skip hung there). These are called from guest code, which a C++
    // exception cannot unwind: a failed join is logged and the thread detached instead.
    void Join() {
        std::unique_lock lock{mutex};
        if (!running) return;
        if (id == std::this_thread::get_id()) {
            // Whoever is joining reaps this thread once it returns; else nobody will.
            if (!joining) {
                thread.detach();
                running = false;
            }
            return;
        }
        if (joining) {
            joined.wait(lock, [this] { return !joining; });
            return;
        }
        joining = true;
        lock.unlock();
        try {
            thread.join();
        } catch (const std::system_error& error) {
            // Windows (libc++ on Win32 threads): the join can fail although the thread is fine.
            // Wait for its function to return, so nothing it uses is freed under it, then let go.
            std::fprintf(stderr, "HLE thread: join failed (%s); waiting for it to finish\n", error.what());
            for (int waited = 0; finished && !finished->load() && waited < 5000; ++waited) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (thread.joinable()) thread.detach();
        }
        lock.lock();
        joining = false;
        running = false;
        joined.notify_all();
    }
    bool Joinable() const {
        std::scoped_lock lock{mutex};
        return running;
    }
    void Stop() {
        {
            std::scoped_lock lock{mutex};
            if (!running) return;
            thread.request_stop();
        }
        Join();
    }

private:
    std::jthread thread;
    std::thread::id id;
    std::shared_ptr<std::atomic<bool>> finished;
    mutable std::mutex mutex;          ///< guards `running`, `joining` and calls on `thread`
    std::condition_variable joined;    ///< a join by another thread finished
    bool running = false;              ///< started and neither joined nor detached
    bool joining = false;              ///< a thread is in thread.join() (outside the lock)
};
} // namespace Libraries::Kernel
