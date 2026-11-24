// theatrhythm: host threads that may call guest code (AvPlayer allocator callbacks).
// Each thread gets a guest TCB (GS base, TLS) from the C runtime before running.
#pragma once
#include <atomic>
#include <chrono>
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
        Stop();
        std::scoped_lock lock{mutex};
        finished = std::make_shared<std::atomic<bool>>(false);
        thread = std::jthread([func = std::move(func), done = finished](std::stop_token stop) {
            runtime_thread_attach_host("theatrhythm:hle");
            func(stop);
            done->store(true, std::memory_order_release);
        });
        id = thread.get_id();
        stop_source = thread.get_stop_source();
    }
    // A thread may stop its own Thread object (AvPlayer does); it detaches instead of joining.
    // theatrhythm: AvPlayer joins a decoder from the decoder itself, the demuxer and Stop() at
    // once, and join() racing detach() on one std::thread is undefined. The handle is taken out
    // under the lock, so exactly one caller joins or detaches it; the others wait (at most 5 s) for
    // the thread's function to return, so nothing it uses is freed under it. These are called from
    // guest code, which a C++ exception cannot unwind: a failed join is logged and the thread
    // detached instead.
    void Join() {
        std::jthread local;
        std::shared_ptr<std::atomic<bool>> done;
        bool self;
        {
            std::scoped_lock lock{mutex};
            local = std::move(thread);
            done = finished;
            self = id == std::this_thread::get_id();
        }
        if (self) {
            if (local.joinable()) local.detach();
            return;
        }
        if (!local.joinable()) {
            WaitFinished(done);
            return;
        }
        try {
            local.join();
        } catch (const std::system_error& error) {
            // Windows (libc++ on Win32 threads): the join can fail although the thread is fine.
            std::fprintf(stderr, "HLE thread: join failed (%s); waiting for it to finish\n", error.what());
            WaitFinished(done);
            if (local.joinable()) local.detach();
        }
    }
    bool Joinable() const {
        std::scoped_lock lock{mutex};
        return thread.joinable();
    }
    void Stop() {
        {
            std::scoped_lock lock{mutex};
            // The stop source outlives the handle: a thread another caller is joining still sees it.
            if (thread.joinable() || (finished && !finished->load(std::memory_order_acquire))) {
                stop_source.request_stop();
            }
        }
        Join();
    }

private:
    static void WaitFinished(const std::shared_ptr<std::atomic<bool>>& done) {
        for (int waited = 0; done && !done->load(std::memory_order_acquire); ++waited) {
            if (waited == 5000) {
                std::fprintf(stderr, "HLE thread: still running after 5 s; letting it go\n");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    mutable std::mutex mutex;
    std::jthread thread;
    std::shared_ptr<std::atomic<bool>> finished;
    std::thread::id id;
    std::stop_source stop_source{std::nostopstate};
};
} // namespace Libraries::Kernel
