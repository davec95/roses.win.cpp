/*
 * roses.win.cpp - standalone Win32 port of Roses.cpp/roses.cpp
 *
 * Animated 8-particle rose (Clifford-style) attractor with CRT
 * phosphor trails, as a single self-contained .exe: native Win32
 * window + GDI rendering, no Python shim, no pipe protocol, no
 * third-party libraries, no extra DLLs. All attractor math lives
 * in roses_core.h (shared with the Linux core); this file is the
 * platform shell: window, input, threads, blit.
 *
 * Controls (identical to roses.py / Roses.cpp):
 *   Space      next rose (replay forward, or new random rose
 *              when the cursor is already at the newest)
 *   Backspace  previous rose
 *   p          toggle phosphor trails (off = solid dots)
 *   - / =      phosphor shorter / longer persistence
 *   Enter      toggle fullscreen
 *   Escape     leave fullscreen
 *   q          quit (or close the window)
 *
 * ---------------------------------------------------------------
 * THREADING MODEL: one worker thread per attractor
 *
 * The 8 particles never interact: particle j writes only plane
 * j (plane-major layout in roses_core.h). Worker j owns particle
 * j AND density plane j outright -- the hot loop needs no
 * mutexes, no atomics, no false sharing.
 *
 * Per tick, engine thread + 8 workers pass two sync points using
 * plain auto-reset kernel32 events (each SetEvent releases
 * exactly one waiting thread; counts balance every tick):
 *
 *   [step_start x8]  engine releases workers
 *        PHASE 1: worker j decays plane j, steps particle j
 *   [step_done x8]   engine waits: all planes final
 *   [tone_go x8]     engine releases workers into phase 2
 *        PHASE 2: all 9 threads tone-map a disjoint pixel slice
 *   [tone_done x8]   engine waits: frame complete
 *   engine posts WM_APP_FRAME; UI thread blits the DIB and
 *   acks via ev_ui_done before the engine starts the next tick.
 *
 * Commands (keys, resize) are queued by the UI thread under a
 * tiny mutex and applied by the engine at the top of each tick,
 * while workers are parked -- the same main-only window the
 * Linux core uses.
 *
 * ---------------------------------------------------------------
 * BUILD (cross-compile from Linux, or MSYS2 on Windows):
 *   make -f Makefile.win
 * or:
 *   x86_64-w64-mingw32-g++ -O2 -std=c++20 -static -mwindows \
 *       -o roses.exe roses.win.cpp -lgdi32 -luser32
 *
 * The result is ONE .exe: -static folds in libstdc++/libgcc/CRT,
 * and the win32 thread model means no winpthread DLL. The only
 * DLL imports are Windows system DLLs (kernel32, user32, gdi32,
 * msvcrt) which exist on every Windows install.
 */

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "roses_core.h"

using namespace roses;

// --- globals ------------------------------------------------------

static HWND   g_hwnd = nullptr;
static std::atomic<bool> g_running{true};  // engine loop condition
static bool   g_worker_quit = false;       // workers exit at step_start

// tick parameters, frozen by the engine before releasing workers
static double tick_ca = 0.0, tick_cb = 0.0;
static int    tick_cx = 0, tick_cy = 0;
static size_t tick_np = 0;

// sync events (auto-reset; counts balance every tick)
static HANDLE ev_step_start, ev_step_done, ev_tone_go, ev_tone_done, ev_ui_done;

// command queue: UI thread -> engine thread
struct Cmd { char c; int w; int h; };
static std::mutex cmd_mtx;
static std::deque<Cmd> cmds;

static void push_cmd(char c, int w = 0, int h = 0)
{
    std::lock_guard<std::mutex> lk(cmd_mtx);
    cmds.push_back({ c, w, h });
}

// title buffer: engine writes before posting the frame, UI reads
static wchar_t g_title[192] = L"Rose Attractor";

// --- DIB (GDI blit target) ----------------------------------------

static HBITMAP dib = nullptr;
static BYTE*   dib_bits = nullptr;
static int     dib_w = 0, dib_h = 0;

static void make_dib(HWND hwnd, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    if (dib) { DeleteObject(dib); dib = nullptr; dib_bits = nullptr; }

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;      // top-down: row 0 = top
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC dc = GetDC(hwnd);
    dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&dib_bits, nullptr, 0);
    ReleaseDC(g_hwnd, dc);

    dib_w = w;
    dib_h = h;
    if (dib_bits)
        memset(dib_bits, 0, (size_t)w * h * 4);
}

// copy the tone-mapped rgb buffer (RGB24) into the DIB (BGRX32)
// and blit to the window. dims must match; a mismatch means the
// engine has not applied the latest resize yet -- skip the frame.
static void present_frame(void)
{
    if (!dib || !dib_bits) return;
    if (fb_w.load() != dib_w || fb_h.load() != dib_h) return;

    const size_t NP = (size_t)fb_w.load() * fb_h.load();
    for (size_t p = 0; p < NP; p++) {
        dib_bits[4*p    ] = rgb[3*p + 2];   // B
        dib_bits[4*p + 1] = rgb[3*p + 1];   // G
        dib_bits[4*p + 2] = rgb[3*p    ];   // R
        dib_bits[4*p + 3] = 0;              // X
    }

    HDC dc = GetDC(g_hwnd);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP old = (HBITMAP)SelectObject(mem, dib);
    BitBlt(dc, 0, 0, dib_w, dib_h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(g_hwnd, dc);
}

// --- worker: one thread per attractor ------------------------------

static void worker(int j)
{
    for (;;) {
        WaitForSingleObject(ev_step_start, INFINITE);
        if (g_worker_quit) break;

        // phase 1: decay + step, plane j touched only by this thread
        decay_plane(j, tick_np);
        step_particle(j, tick_ca, tick_cb, tick_cx, tick_cy, tick_np);
        SetEvent(ev_step_done);

        // phase 2: tone-map this thread's disjoint pixel slice
        WaitForSingleObject(ev_tone_go, INFINITE);

        const size_t span = (tick_np + N_COLORS) / (N_COLORS + 1);
        const size_t lo = std::min(tick_np, (size_t)(j + 1) * span);
        const size_t hi = std::min(tick_np, lo + span);
        tone_map_range(lo, hi, tick_np);

        SetEvent(ev_tone_done);
    }
}

// --- engine thread: the tick loop ----------------------------------

static void pump_cmds(void)
{
    std::deque<Cmd> local;
    {
        std::lock_guard<std::mutex> lk(cmd_mtx);
        local.swap(cmds);
    }

    for (const Cmd& c : local) {
        switch (c.c) {
        case 'n': next_rose();           break;
        case 'b': back_rose();           break;
        case 'p': toggle_phosphor();     break;
        case '-': shorter_persistence(); break;
        case '=': longer_persistence();  break;
        case 'R': render();              break;
        case 'q': g_running = false;     break;
        case 'r':
            if (c.w > 0 && c.h > 0 &&
                (c.w != fb_w.load() || c.h != fb_h.load()))
                create_accumulator(c.w, c.h);
            break;
        default:
            break;
        }
    }
}

static void engine_thread(void)
{
    // first rose: index 0, saved into the buffer
    double a0 = time_random(-1.0, 1.0);
    a.store(a0);
    b.store(1.0 - a0);
    ab_buffer.push_back({ a0, 1.0 - a0 });
    index = 0;

    create_accumulator(fb_w.load() > 0 ? fb_w.load() : WINDOW_W,
                       fb_h.load() > 0 ? fb_h.load() : WINDOW_H);
    render();

    // one worker thread per attractor: worker j owns particle j
    // and density plane j
    std::thread workers[N_COLORS];
    for (int j = 0; j < N_COLORS; j++)
        workers[j] = std::thread(worker, j);

    while (g_running) {
        // engine-only window: workers are parked at step_start,
        // so commands, resizes and reseeds touch shared state safely
        pump_cmds();
        if (!g_running) break;

        tick_np = (size_t)fb_w.load() * fb_h.load();
        tick_cx = fb_w.load() / 2;
        tick_cy = fb_h.load() / 2;
        tick_ca = a.load();
        tick_cb = b.load();

        // release workers into phase 1, wait for all planes final
        for (int k = 0; k < N_COLORS; k++) SetEvent(ev_step_start);
        for (int k = 0; k < N_COLORS; k++)
            WaitForSingleObject(ev_step_done, INFINITE);

        // release workers into phase 2; engine tone-maps slice 0
        for (int k = 0; k < N_COLORS; k++) SetEvent(ev_tone_go);
        const size_t span = (tick_np + N_COLORS) / (N_COLORS + 1);
        tone_map_range(0, std::min(tick_np, span), tick_np);
        for (int k = 0; k < N_COLORS; k++)
            WaitForSingleObject(ev_tone_done, INFINITE);

        // title + frame handoff
        double ca = a.load(), cb = b.load();
        swprintf(g_title, 192,
                 L"Rose Attractor   #%d   a=%.6f   b=%.6f",
                 index, ca, cb);

        PostMessageW(g_hwnd, WM_APP + 1, 0, 0);

        // wait for the UI blit before touching rgb again. this
        // wait is INFINITE on purpose: the engine must never
        // reallocate rgb (resize) or overwrite it (next tick)
        // while present_frame is still reading it. a wedge is
        // impossible -- WM_DESTROY clears g_running and sets
        // ev_ui_done, and the modal drag loop always ends.
        WaitForSingleObject(ev_ui_done, INFINITE);
    }

    // wake workers parked at step_start so they observe quit
    g_worker_quit = true;
    for (int k = 0; k < N_COLORS; k++) SetEvent(ev_step_start);
    for (int j = 0; j < N_COLORS; j++) workers[j].join();
}

// --- window ---------------------------------------------------------

static bool  fullscreen = false;
static LONG  saved_style = 0;
static RECT  saved_rect = {};

static void enter_fullscreen(void)
{
    if (fullscreen) return;
    saved_style = GetWindowLong(g_hwnd, GWL_STYLE);
    GetWindowRect(g_hwnd, &saved_rect);

    HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    GetMonitorInfo(mon, &mi);

    SetWindowLong(g_hwnd, GWL_STYLE,
                  saved_style & ~(WS_CAPTION | WS_THICKFRAME));
    SetWindowPos(g_hwnd, HWND_TOP,
                 mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_FRAMECHANGED);
    fullscreen = true;
}

static void exit_fullscreen(void)
{
    if (!fullscreen) return;
    SetWindowLong(g_hwnd, GWL_STYLE, saved_style);
    SetWindowPos(g_hwnd, HWND_TOP,
                 saved_rect.left, saved_rect.top,
                 saved_rect.right - saved_rect.left,
                 saved_rect.bottom - saved_rect.top,
                 SWP_FRAMECHANGED);
    fullscreen = false;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg,
                                WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_APP + 1:            // frame ready: blit + ack engine
        present_frame();
        SetWindowTextW(hwnd, g_title);
        SetEvent(ev_ui_done);
        return 0;

    case WM_KEYDOWN:
        switch (wParam) {
        case VK_SPACE:    push_cmd('n'); break;
        case VK_BACK:     push_cmd('b'); break;
        case 'P':         push_cmd('p'); break;
        case VK_OEM_MINUS: push_cmd('-'); break;
        case VK_OEM_PLUS:  push_cmd('='); break;
        case VK_RETURN:
            if (fullscreen) exit_fullscreen();
            else            enter_fullscreen();
            break;
        case VK_ESCAPE:   exit_fullscreen();  break;
        case 'Q':         DestroyWindow(hwnd); break;
        default: break;
        }
        return 0;

    case WM_SIZE:
        {
            int w = (int)LOWORD(lParam);
            int h = (int)HIWORD(lParam);
            if (w > 0 && h > 0) {
                make_dib(hwnd, w, h);
                push_cmd('r', w, h);
            }
        }
        return 0;

    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (dib) {
                HDC mem = CreateCompatibleDC(dc);
                HBITMAP old = (HBITMAP)SelectObject(mem, dib);
                BitBlt(dc, 0, 0, dib_w, dib_h, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old);
                DeleteDC(mem);
            }
            EndPaint(hwnd, &ps);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;              // DIB covers the client; no flicker

    case WM_DESTROY:
        g_running = false;
        SetEvent(ev_ui_done);  // unblock engine if it is waiting
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hinst, HINSTANCE, LPSTR, int)
{
    SetProcessDPIAware();

    ev_step_start = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    ev_step_done  = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    ev_tone_go    = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    ev_tone_done  = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    ev_ui_done    = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hinst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"RosesWindow";
    RegisterClassW(&wc);

    // client area exactly WINDOW_W x WINDOW_H
    RECT cr = { 0, 0, WINDOW_W, WINDOW_H };
    AdjustWindowRect(&cr, WS_OVERLAPPEDWINDOW, FALSE);

    g_hwnd = CreateWindowW(L"RosesWindow", L"Rose Attractor",
                          WS_OVERLAPPEDWINDOW,
                          CW_USEDEFAULT, CW_USEDEFAULT,
                          cr.right - cr.left, cr.bottom - cr.top,
                          nullptr, nullptr, hinst, nullptr);
    if (!g_hwnd) return 1;

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    // engine thread starts after the window exists (WM_SIZE has
    // already created the DIB and queued the initial resize)
    std::thread engine(engine_thread);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_running = false;
    engine.join();

    CloseHandle(ev_step_start);
    CloseHandle(ev_step_done);
    CloseHandle(ev_tone_go);
    CloseHandle(ev_tone_done);
    CloseHandle(ev_ui_done);
    if (dib) DeleteObject(dib);

    return (int)msg.wParam;
}
