// HLE host threads (gpu/shim/core/libraries/kernel/threads.h): AvPlayer's join pattern, where a
// decoder joins itself as it exits while the demuxer and Stop() join it too.
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include <unistd.h>
#include "core/libraries/kernel/threads.h"

extern "C" void runtime_thread_attach_host(const char*) {}

using Libraries::Kernel::Thread;

namespace {
// A short random pause, so the callers meet in every order.
void Jitter(unsigned& seed) {
    seed = seed * 1103515245u + 12345u;
    const unsigned n = (seed >> 16) % 64;
    for (unsigned i = 0; i < n * 50; ++i) {
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }
    if (n % 7 == 0) std::this_thread::yield();
}
} // namespace

int main() {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
#endif
    // A Thread that never ran: every call is a no-op.
    {
        Thread idle;
        assert(!idle.Joinable());
        idle.Join();
        idle.Stop();
    }

    // Stop() reaches a thread waiting for its stop token, and Run() works again after it.
    {
        Thread t;
        for (int round = 0; round < 3; ++round) {
            std::atomic<bool> exited{false};
            t.Run([&](std::stop_token stop) {
                while (!stop.stop_requested()) std::this_thread::yield();
                exited = true;
            });
            assert(t.Joinable());
            t.Stop();
            assert(exited && !t.Joinable());
        }
    }

    // AvPlayerSource: each decoder joins itself on exit, the demuxer joins both decoders and then
    // itself, and the game thread stops all three. No caller other than the thread itself may
    // return before that thread's function has, and no join may fail: the old Join() raced and
    // printed "HLE thread: join failed" on about half the rounds. The checks inside the threads
    // count into `bad`, since stderr is captured meanwhile.
    constexpr int kRounds = 1000;
    std::atomic<int> bad{0};
    std::FILE* captured = std::tmpfile();
    assert(captured);
    std::fflush(stderr);
    const int saved_stderr = dup(fileno(stderr));
    dup2(fileno(captured), fileno(stderr));
    for (int round = 0; round < kRounds; ++round) {
        unsigned seed = 0x9e3779b9u * (round + 1);
        std::atomic<bool> video_exited{false}, audio_exited{false}, demuxer_exited{false};
        std::atomic<bool> go{false};
        {
            // Destroyed in reverse order: the demuxer (which uses both decoders) first.
            Thread video, audio, demuxer;
            video.Run([&, s = seed ^ 1u](std::stop_token) mutable {
                while (!go) std::this_thread::yield();
                Jitter(s);
                video_exited = true;
                video.Join();
            });
            audio.Run([&, s = seed ^ 2u](std::stop_token) mutable {
                while (!go) std::this_thread::yield();
                Jitter(s);
                audio_exited = true;
                audio.Join();
            });
            demuxer.Run([&, s = seed ^ 3u](std::stop_token) mutable {
                while (!go) std::this_thread::yield();
                Jitter(s);
                video.Join();
                bad += !video_exited;
                audio.Join();
                bad += !audio_exited;
                demuxer_exited = true;
                demuxer.Join();
            });
            go = true;
            Jitter(seed);
            // Odd rounds leave it to the destructors, which Stop() too.
            if (round % 2 == 0) {
                video.Stop();
                bad += !video_exited || video.Joinable();
                audio.Stop();
                bad += !audio_exited || audio.Joinable();
                demuxer.Stop();
                bad += !demuxer_exited || demuxer.Joinable();
            }
        }
        bad += !video_exited || !audio_exited || !demuxer_exited;
    }
    std::fflush(stderr);
    dup2(saved_stderr, fileno(stderr));
    const long logged = std::ftell(captured);
    if (logged > 0) {
        char line[256];
        std::rewind(captured);
        if (std::fgets(line, sizeof line, captured)) std::fprintf(stderr, "first: %s", line);
    }
    std::fclose(captured);
    if (bad != 0 || logged != 0) {
        std::fprintf(stderr, "%d failed checks, %ld bytes of thread errors\n", bad.load(), logged);
    }
    assert(bad == 0 && logged == 0);

    std::printf("PASS: HLE thread join (%d rounds)\n", kRounds);
}
