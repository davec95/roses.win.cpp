/*
 * roses_core.h - portable C++20 compute core for the rose attractor.
 *
 * Shared by roses.win.cpp (Win32 GUI shell) and test_core.cpp (native
 * Linux test harness). Pure standard library: no platform headers,
 * no threading, no I/O. The platform shell owns threads, sync, and
 * display; this header owns the math.
 *
 * Attractor (Clifford-style "rose"):
 *     x' = a*x + y + c
 *     c' = 2*b*x'^2 / (1 + x'^2)
 *     y' = a*x' - x + c'
 *
 * Eight particles accumulate hit DENSITY into their own float plane
 * (CRT phosphor model): each tick every plane decays by the
 * persistence factor, new hits add to that particle's plane, each
 * plane is tone-mapped separately, and the 8 colors composite
 * additively -- a dense single-color trail saturates toward that
 * color, never toward white.
 *
 * DENSITY LAYOUT: PLANE-MAJOR, plane j at density[j*w*h ..
 * (j+1)*w*h). Hits accumulate with a direct `plane[idx] += 1.0f`
 * (exact for repeated hits), and each plane is a disjoint memory
 * region so one thread can own one plane with zero locks.
 */

#ifndef ROSES_CORE_H
#define ROSES_CORE_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <random>
#include <utility>
#include <vector>

namespace roses {

// --- window / attractor constants --------------------------------

static const int    WINDOW_W = 1280;
static const int    WINDOW_H = 700;
static const double SCALE    = 9.5;

static const int BUFFER_SIZE = 20;

// --- phosphor settings -------------------------------------------

static const float PHOSPHOR_GAIN = 1.0f;   // density one hit adds
static const float GLOW_K = 50.0f;         // tone-map knee; lower = hotter glow

static const float DECAY_STEPS[] = {0.90f, 0.94f, 0.96f, 0.98f, 0.99f};
static const int   N_DECAY = (int)(sizeof(DECAY_STEPS) / sizeof(DECAY_STEPS[0]));

// EGA palette 8..15, normalized 0..1 for compositing
static const int N_COLORS = 8;
static const float COLOR_NORM[N_COLORS][3] = {
    { 85.0f/255,  85.0f/255,  85.0f/255 },   // #555555
    { 85.0f/255,  85.0f/255, 255.0f/255 },   // #5555ff
    { 85.0f/255, 255.0f/255,  85.0f/255 },   // #55ff55
    { 85.0f/255, 255.0f/255, 255.0f/255 },   // #55ffff
    {255.0f/255,  85.0f/255,  85.0f/255 },   // #ff5555
    {255.0f/255,  85.0f/255, 255.0f/255 },   // #ff55ff
    {255.0f/255, 255.0f/255,  85.0f/255 },   // #ffff55
    {255.0f/255, 255.0f/255, 255.0f/255 },   // #ffffff
};

static const int STEPS_PER_TICK = 20000;

// --- shared state -------------------------------------------------

// density is PLANE-MAJOR: plane j occupies density[j*w*h ..
// (j+1)*w*h). Reallocated by the main thread only, while workers
// are parked at the step-start barrier.
static std::vector<float>   density;
static std::vector<uint8_t> rgb;          // tone-mapped frame, 3 bytes/pixel
// atomics: the engine thread writes these on resize; a GUI
// thread may read them for blit-size checks outside the barrier
// sync. Workers only read them between barriers, where the
// sync already orders the writes.
static std::atomic<int> fb_w{0};          // frozen between barriers
static std::atomic<int> fb_h{0};

struct Particle { double x, y, c; };
// particles[j]: written by main in render() (main-only window),
// then read/written only by worker j during the step phase.
static Particle particles[N_COLORS];

// atomics: cheap insurance for cross-thread visibility; the
// barriers already order the writes
static std::atomic<double> a{0.0};
static std::atomic<double> b{0.0};
static std::atomic<bool>   phosphor{true};
static std::atomic<int>    decay_i{2};

static std::deque<std::pair<double,double>> ab_buffer;  // history (main only)
static int index = 0;                                    // cursor (main only)

// --- random -------------------------------------------------------

/*
 * time_random: uniform(lo, hi) with the current time folded into
 * the seed -- wall clock in nanoseconds XOR 64 bits of OS entropy,
 * so two roses drawn at different instants essentially never share
 * the same (a, b). Main thread only.
 */
static double time_random(double lo, double hi)
{
    static std::random_device rd;

    uint64_t ns = (uint64_t)
        std::chrono::system_clock::now()
            .time_since_epoch().count();

    uint64_t seed = ns ^ ((uint64_t)rd() << 32) ^ (uint64_t)rd();

    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> dist(lo, hi);
    return dist(rng);
}

// --- accumulator / render (main thread, main-only windows) --------

static void create_accumulator(int w, int h)
{
    fb_w.store(w);
    fb_h.store(h);
    density.assign((size_t)N_COLORS * w * h, 0.0f);
    rgb.assign((size_t)w * h * 3, 0);
}

static void render(void)
{
    // reseed the 8 particles along the diagonal, as in the original
    double cb = b.load();
    for (int i = 0; i < N_COLORS; i++) {
        double t = 2.0 * i - 0.51;
        double x = t;
        double y = t;
        double c = 2.0 * cb * x * x / (1.0 + x * x);
        particles[i] = { x, y, c };
    }
}

// --- rose history (sliding-window semantics, same as roses.py) ----
// main thread only, in the main-only window between barriers

static void next_rose(void)
{
    double ca = a.load(), cb = b.load();

    if (index < (int)ab_buffer.size() - 1) {
        // cursor not at newest: replay the buffered rose exactly
        index++;
        ca = ab_buffer[index].first;
        cb = ab_buffer[index].second;
    } else {
        // cursor at newest: THIS is where a new random rose is allowed
        ca = time_random(-1.0, 1.0);
        cb = 1.0 - ca;

        ab_buffer.push_back({ ca, cb });
        if ((int)ab_buffer.size() > BUFFER_SIZE)
            ab_buffer.pop_front();      // window full: drop oldest

        index = (int)ab_buffer.size() - 1;
    }

    a.store(ca);
    b.store(cb);
    render();
}

static void back_rose(void)
{
    if (index > 0) {
        index--;
        a.store(ab_buffer[index].first);
        b.store(ab_buffer[index].second);
    }
    render();
}

static void toggle_phosphor(void)
{
    phosphor.store(!phosphor.load());
    // clear trails: caller guarantees workers are idle
    std::fill(density.begin(), density.end(), 0.0f);
}

static void shorter_persistence(void)
{
    decay_i.store(std::max(0, decay_i.load() - 1));
}

static void longer_persistence(void)
{
    decay_i.store(std::min(N_DECAY - 1, decay_i.load() + 1));
}

// --- per-plane work (each plane owned by exactly one thread) -----

/*
 * Decay plane j by the current persistence factor (or clear it
 * when phosphor is off). Plane j is touched only by its owner.
 */
static inline void decay_plane(int j, size_t NP)
{
    float* plane = density.data() + (size_t)j * NP;

    if (phosphor.load()) {
        const float k = DECAY_STEPS[decay_i.load()];
        for (size_t i = 0; i < NP; i++)
            plane[i] *= k;
    } else {
        std::fill(plane, plane + NP, 0.0f);
    }
}

/*
 * Step particle j STEPS_PER_TICK times, accumulating hits directly
 * into plane j. Particle j and plane j are touched only by the
 * calling thread. ca/cb/cx/cy are frozen tick values supplied by
 * the caller so every plane in a tick uses identical parameters.
 */
static inline void step_particle(int j, double ca, double cb,
                                 int cx, int cy, size_t NP)
{
    float* plane = density.data() + (size_t)j * NP;
    const double sc = SCALE;
    const int w = fb_w.load(), h = fb_h.load();

    Particle p = particles[j];

    for (int s = 0; s < STEPS_PER_TICK; s++) {
        double xp = ca * p.x + p.y + p.c;
        double cp = 2.0 * cb * xp * xp / (1.0 + xp * xp);
        double yp = ca * xp - p.x + cp;

        p.x = xp; p.y = yp; p.c = cp;

        int px = (int)(sc * xp + cx);
        int py = (int)(sc * yp + cy);

        if (px >= 0 && px < w && py >= 0 && py < h)
            plane[(size_t)py * w + px] += PHOSPHOR_GAIN;
    }

    particles[j] = p;
}

// --- tone-map: one disjoint pixel range per thread ----------------

/*
 * Tone-map pixels [lo, hi): read all 8 planes, write only this
 * range's rgb bytes. Empty pixels (d == 0) skip the expf entirely
 * -- most pixels are empty, a large constant-factor win. Call
 * only after every plane for the tick is final.
 */
static inline void tone_map_range(size_t lo, size_t hi, size_t NP)
{
    const bool ph = phosphor.load();

    for (size_t p = lo; p < hi; p++) {
        float r = 0.0f, g = 0.0f, bl = 0.0f;

        for (int k = 0; k < N_COLORS; k++) {
            float d = density[(size_t)k * NP + p];
            if (d <= 0.0f) continue;

            float inten = ph
                ? 255.0f * (1.0f - expf(-d / GLOW_K))
                : fminf(d, 1.0f) * 255.0f;

            r  += inten * COLOR_NORM[k][0];
            g  += inten * COLOR_NORM[k][1];
            bl += inten * COLOR_NORM[k][2];
        }

        rgb[3*p    ] = (uint8_t)std::min(255.0f, std::max(0.0f, r));
        rgb[3*p + 1] = (uint8_t)std::min(255.0f, std::max(0.0f, g));
        rgb[3*p + 2] = (uint8_t)std::min(255.0f, std::max(0.0f, bl));
    }
}

}  // namespace roses

#endif  // ROSES_CORE_H
