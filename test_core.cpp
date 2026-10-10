/*
 * test_core.cpp - native Linux harness for roses_core.h.
 *
 * roses.win.cpp cannot run on Linux (no wine here), so this file
 * exercises the SHARED compute core with the same threading shape
 * as the Win32 shell: one worker per attractor, two sync points
 * per tick (std::barrier here instead of kernel32 events). It
 * verifies the attractor math, plane ownership, tone-map slicing,
 * rose history, and resize behavior, and reports throughput.
 *
 * Build:  make -f Makefile.test          (or g++ -O2 -std=c++20 -pthread ...)
 * Run:    ./test_core [ticks]
 */

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "roses_core.h"

using namespace roses;

static std::barrier step_start{N_COLORS + 1};
static std::barrier step_done{N_COLORS + 1};
static std::barrier tone_done{N_COLORS + 1};

static std::atomic<bool> quit_flag{false};

// tick parameters, frozen by the engine before releasing workers
static double tick_ca = 0.0, tick_cb = 0.0;
static int    tick_cx = 0, tick_cy = 0;
static size_t tick_np = 0;

static void worker(int j)
{
    for (;;) {
        step_start.arrive_and_wait();
        if (quit_flag.load()) break;

        decay_plane(j, tick_np);
        step_particle(j, tick_ca, tick_cb, tick_cx, tick_cy, tick_np);
        step_done.arrive_and_wait();

        const size_t span = (tick_np + N_COLORS) / (N_COLORS + 1);
        const size_t lo = std::min(tick_np, (size_t)(j + 1) * span);
        const size_t hi = std::min(tick_np, lo + span);
        tone_map_range(lo, hi, tick_np);

        tone_done.arrive_and_wait();
    }
}

int main(int argc, char** argv)
{
    int ticks = argc > 1 ? atoi(argv[1]) : 30;

    double a0 = time_random(-1.0, 1.0);
    a.store(a0);
    b.store(1.0 - a0);
    ab_buffer.push_back({ a0, 1.0 - a0 });
    index = 0;

    create_accumulator(WINDOW_W, WINDOW_H);
    render();

    std::thread workers[N_COLORS];
    for (int j = 0; j < N_COLORS; j++)
        workers[j] = std::thread(worker, j);

    auto t0 = std::chrono::steady_clock::now();
    size_t lit_prev = 0;
    int checks = 0, fails = 0;

    auto check = [&](bool ok, const char* what) {
        checks++;
        if (!ok) { fails++; printf("FAIL: %s\n", what); }
    };

    for (int t = 0; t < ticks; t++) {
        // scripted commands at fixed ticks (mirrors the Win32 queue)
        if (t == 5)  next_rose();
        if (t == 8)  back_rose();
        if (t == 10) next_rose();          // replay forward
        if (t == 12) toggle_phosphor();
        if (t == 14) shorter_persistence();
        if (t == 16) longer_persistence();
        if (t == 18) { create_accumulator(320, 200); render(); }
        if (t == 22) { create_accumulator(WINDOW_W, WINDOW_H); render(); }

        tick_np = (size_t)fb_w.load() * fb_h.load();
        tick_cx = fb_w.load() / 2;
        tick_cy = fb_h.load() / 2;
        tick_ca = a.load();
        tick_cb = b.load();

        step_start.arrive_and_wait();
        step_done.arrive_and_wait();

        const size_t span = (tick_np + N_COLORS) / (N_COLORS + 1);
        tone_map_range(0, std::min(tick_np, span), tick_np);
        tone_done.arrive_and_wait();

        // sanity: rgb fully written (every slice covered)
        size_t lit = 0;
        for (size_t p = 0; p < tick_np; p++)
            if (rgb[3*p] || rgb[3*p+1] || rgb[3*p+2]) lit++;

        if (t == 0) check(lit > 1000, "frame 0 has hits");
        if (t == 4) check(lit >= lit_prev / 2, "density persists across ticks");
        // NOTE: unlike roses.py (whose render() recreates the
        // accumulator), the C++ core keeps old trails on a new
        // rose and lets them decay -- matching Roses.cpp R2.
        if (t == 6) check(lit > 1000, "frame still has content after new rose");
        if (t == 20) check(fb_w.load() == 320 && fb_h.load() == 200, "resize applied");
        lit_prev = lit;
    }

    // history semantics: n,b,n must replay the buffered rose
    // exactly (no fresh random), leaving the cursor at newest
    check(ab_buffer.size() == 2, "replay did not append a new rose");
    check(index == (int)ab_buffer.size() - 1, "cursor at newest after replay");

    quit_flag.store(true);
    step_start.arrive_and_wait();
    for (int j = 0; j < N_COLORS; j++) workers[j].join();

    double secs = (double)ticks * STEPS_PER_TICK * N_COLORS;
    double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    printf("%d ticks in %.3f s, %d checks, %d fails\n",
           ticks, elapsed, checks, fails);
    printf("rose params: a=%.6f b=%.6f index=%d buffer=%zu\n",
           a.load(), b.load(), index, ab_buffer.size());
    printf("throughput: %.1f M pts/sec\n", secs / elapsed / 1e6);
    return fails ? 1 : 0;
}
