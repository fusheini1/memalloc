// MemAlloc — Custom Multithreaded Memory Allocator
// Author: Fusheini Abdul-Mumin <abdulmuminfusheini@gmail.com>
// License: MIT

// MemAlloc Visualizer (Step 9.3): a Dear ImGui + GLFW + OpenGL3 application
// that shows LIVE allocator activity. Three worker threads churn allocations
// through ThreadPoolAlloc::instance(); the instrumentation layer
// (viz_hook.hpp) records every operation into a ring buffer, and three
// windows render it each frame:
//
//   (a) "Global Stats"     — memalloc::global_stats() text + a rolling plot
//   (b) "Per-Thread Pools" — occupancy bars per live thread via for_each_live
//   (c) "Event Feed"       — the most recent events from viz::ring(), colored
//
// Links memalloc_core only (NO new/delete override), so the allocator's
// plain API drives the demo workload while ImGui/GLFW internals ride system
// malloc. Closing the window stops and joins the workers BEFORE tearing down
// ImGui/GLFW (clean shutdown, no hang).

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

#include "stats.hpp"
#include "thread_pool_alloc.hpp"
#include "viz_hook.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kWorkerCount = 3;
// Cycling sizes: four pool size classes plus one large (arena) size. Arena
// blocks have no per-object free, so the 2000-byte alloc is throttled to
// every 25th batch — otherwise three workers would exhaust the 4 MiB arena
// in under a second instead of letting the arena bar creep up slowly.
constexpr std::size_t kPoolSizes[] = {16, 64, 256, 1024};
constexpr std::size_t kArenaSize = 2000;
constexpr std::size_t kArenaEvery = 25;
constexpr std::size_t kHoldMax = 8;  // blocks held per worker, then freed

struct HeldBlock {
    void* ptr;
    std::size_t size;
};

void glfw_error_callback(int error, const char* description) {
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

// Demo workload: allocate blocks, hold a few briefly, free them, sleep ~1 ms
// per batch so the event rate is watchable. Arena (2000-byte) frees are
// no-ops by design; those blocks are released when this thread's allocator
// dies at thread exit.
void worker_loop(std::atomic<bool>& stop) {
    memalloc::ThreadPoolAlloc& alloc = memalloc::ThreadPoolAlloc::instance();
    std::vector<HeldBlock> held;
    held.reserve(kHoldMax + 1);
    std::size_t batch = 0;
    while (!stop.load(std::memory_order_relaxed)) {
        // Every 40th batch: a burst of 300 x 64-byte blocks exhausts the
        // 256-block pool slab and forces a CentralHeap chunk acquire — the
        // slow path shows up as yellow "chunk +64KiB" lines in the feed.
        if (batch % 40 == 0 && batch != 0) {
            std::vector<void*> burst;
            burst.reserve(300);
            for (int i = 0; i < 300; ++i) {
                void* p = alloc.allocate(64);
                if (p != nullptr) {
                    burst.push_back(p);
                }
            }
            for (void* p : burst) {
                alloc.deallocate(p, 64);
            }
            ++batch;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        const std::size_t size =
            (batch % kArenaEvery == 0) ? kArenaSize
                                       : kPoolSizes[batch % 4];
        void* p = alloc.allocate(size);
        if (p != nullptr) {
            held.push_back({p, size});
            if (held.size() > kHoldMax) {
                const HeldBlock b = held.front();
                held.erase(held.begin());
                alloc.deallocate(b.ptr, b.size);
            }
        }
        ++batch;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Free any remaining pool blocks (arena blocks go with the allocator).
    for (const HeldBlock& b : held) {
        alloc.deallocate(b.ptr, b.size);
    }
}

// (a) Global Stats: aggregated counters, a smoothed ops/sec readout, a
// rolling plot of allocated bytes, and a per-size-class live-bytes
// histogram. Content lives in a scrollable child so the fixed 440x340
// window never clips anything.
void draw_global_stats(float* history, int& history_count,
                       double& ops_per_sec, std::size_t& prev_ops,
                       double& prev_time) {
    ImGui::Begin("Global Stats");
    ImGui::BeginChild("stats_scroll", ImVec2(0.0f, 0.0f), true);
    const memalloc::Stats gs = memalloc::global_stats();

    // Smoothed ops/sec from the alloc+free counters (EMA over time).
    const std::size_t ops = gs.alloc_count + gs.free_count;
    const double t = ImGui::GetTime();
    const double dt = t - prev_time;
    if (dt > 0.05) {  // guard against frame spikes
        const double raw = static_cast<double>(ops - prev_ops) / dt;
        ops_per_sec =
            (prev_time == 0.0) ? raw : ops_per_sec * 0.9 + raw * 0.1;
        prev_ops = ops;
        prev_time = t;
    }
    ImGui::Text("ops/sec           : %.0f", ops_per_sec);
    ImGui::Text("requested         : %zu bytes", gs.requested);
    ImGui::Text("allocated         : %zu bytes", gs.allocated);
    ImGui::Text("peak allocated    : %zu bytes", gs.peak_allocated);
    ImGui::Text("alloc count       : %zu", gs.alloc_count);
    ImGui::Text("free count        : %zu", gs.free_count);
    ImGui::Text("internal frag     : %.2f%%",
                gs.internal_fragmentation() * 100.0);
    ImGui::Separator();
    history[history_count % 240] = static_cast<float>(gs.allocated);
    ++history_count;
    // values_offset makes PlotLines treat `history` as a circular buffer.
    ImGui::PlotLines("allocated bytes", history, 240, history_count % 240,
                     nullptr, 0.0f, 0.0f, ImVec2(0.0f, 80.0f));

    // Live bytes per size class across all live threads (live = cap -
    // free_count, bytes = live * class size).
    std::size_t live_bytes[memalloc::NUM_CLASSES] = {};
    memalloc::for_each_live(
        [](const memalloc::ThreadPoolAlloc& a, void* user) {
            auto* bytes =
                static_cast<std::size_t(*)[memalloc::NUM_CLASSES]>(user);
            const memalloc::ThreadPoolAlloc::Occupancy occ = a.occupancy();
            for (std::size_t i = 0; i < memalloc::NUM_CLASSES; ++i) {
                (*bytes)[i] += (occ.cap[i] - occ.free_count[i]) *
                               memalloc::SIZE_CLASSES[i];
            }
        },
        &live_bytes);
    float hist[memalloc::NUM_CLASSES];
    float max_bytes = 1.0f;
    for (std::size_t i = 0; i < memalloc::NUM_CLASSES; ++i) {
        hist[i] = static_cast<float>(live_bytes[i]);
        max_bytes = std::max(max_bytes, hist[i]);
    }
    ImGui::PlotHistogram("live bytes/class", hist, memalloc::NUM_CLASSES, 0,
                         nullptr, 0.0f, max_bytes, ImVec2(0.0f, 80.0f));
    ImGui::EndChild();
    ImGui::End();
}

// (b) Per-Thread Pools: one collapsible section per live allocator, with a
// used/cap bar per size class plus the arena. Called under the registry
// mutex by for_each_live; the callbacks only read (occupancy/thread_id), so
// no re-entrancy.
void draw_per_thread_pools() {
    ImGui::Begin("Per-Thread Pools");
    ImGui::BeginChild("pools_scroll", ImVec2(0.0f, 0.0f), true);
    memalloc::for_each_live(
        [](const memalloc::ThreadPoolAlloc& a, void* /*user*/) {
            const memalloc::ThreadPoolAlloc::Occupancy occ = a.occupancy();
            // Each thread is an INDEPENDENT CollapsingHeader: PushID scopes
            // the header's ImGui id by thread id, so any number of threads
            // can be expanded at the same time (no accordion, no shared
            // open state). All start expanded on first launch; the user may
            // collapse them individually.
            ImGui::PushID(static_cast<int>(a.thread_id()));
            std::string title = "Thread ";
            title += std::to_string(a.thread_id());
            ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            if (ImGui::CollapsingHeader(title.c_str())) {
                for (std::size_t i = 0; i < memalloc::NUM_CLASSES; ++i) {
                    const std::size_t used = occ.cap[i] - occ.free_count[i];
                    // cap includes external slabs (total_blocks), so used
                    // is never negative; clamp defensively anyway.
                    const float frac =
                        (occ.cap[i] > 0)
                            ? std::clamp(static_cast<float>(used) /
                                             static_cast<float>(occ.cap[i]),
                                         0.0f, 1.0f)
                            : 0.0f;
                    std::string overlay = std::to_string(
                        memalloc::SIZE_CLASSES[i]);
                    overlay += "  ";
                    overlay += std::to_string(used);
                    overlay += "/";
                    overlay += std::to_string(occ.cap[i]);
                    ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f),
                                       overlay.c_str());
                }
                const float arena_frac =
                    (occ.arena_cap > 0)
                        ? std::clamp(static_cast<float>(occ.arena_used) /
                                         static_cast<float>(occ.arena_cap),
                                     0.0f, 1.0f)
                        : 0.0f;
                std::string a_overlay = "arena  ";
                a_overlay += std::to_string(occ.arena_used);
                a_overlay += "/";
                a_overlay += std::to_string(occ.arena_cap);
                ImGui::ProgressBar(arena_frac, ImVec2(-1.0f, 0.0f),
                                   a_overlay.c_str());
            }
            ImGui::PopID();
        },
        nullptr);
    ImGui::EndChild();
    ImGui::End();
}

// (c) Event Feed: the most recent events from the ring, newest at the
// bottom, auto-scrolling only when new events arrived.
void draw_event_feed(std::vector<memalloc::viz::AllocEvent>& feed,
                     std::size_t& last_count, bool& scroll_to_bottom) {
    ImGui::Begin("Event Feed");
    const std::size_t ring_count = memalloc::viz::ring().count();
    if (ring_count != last_count) {
        feed = memalloc::viz::ring().snapshot_tail(200);
        last_count = ring_count;
        scroll_to_bottom = true;
    }
    ImGui::BeginChild("feed", ImVec2(0.0f, 0.0f), true);
    if (scroll_to_bottom) {
        ImGui::SetScrollHereY(1.0f);
        scroll_to_bottom = false;
    }
    for (const memalloc::viz::AllocEvent& e : feed) {
        ImVec4 color;
        const char* tag;
        if (e.kind == memalloc::viz::kEventChunk) {
            color = ImVec4(0.95f, 0.85f, 0.20f, 1.0f);  // yellow (slow path)
            tag = "chunk +64KiB";
        } else if (e.kind == memalloc::viz::kEventAlloc) {
            color = ImVec4(0.35f, 0.90f, 0.35f, 1.0f);  // green
            tag = "alloc";
        } else {
            color = ImVec4(0.95f, 0.45f, 0.45f, 1.0f);  // red
            tag = "free ";
        }
        ImGui::TextColored(color, "[T%u] %s %zu @0x%llX", e.thread_id, tag,
                           e.size, static_cast<unsigned long long>(e.ptr));
    }
    ImGui::EndChild();
    ImGui::End();
}

}  // namespace

int main() {
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        std::fprintf(stderr, "memalloc_viz: glfwInit failed\n");
        return 1;
    }

    // OpenGL 3.2 core profile (GLSL 1.50 on Windows).
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

    GLFWwindow* window =
        glfwCreateWindow(1280, 720, "MemAlloc Visualizer", nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "memalloc_viz: glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);  // vsync

    // Dear ImGui context + backends.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    // Start recording BEFORE the workers spawn so no allocations escape the
    // event feed.
    memalloc::viz::set_enabled(true);
    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    workers.reserve(kWorkerCount);
    for (std::size_t i = 0; i < kWorkerCount; ++i) {
        workers.emplace_back(worker_loop, std::ref(stop));
    }

    // Per-frame state for the windows.
    float history[240] = {};
    int history_count = 0;
    double ops_per_sec = 0.0;
    std::size_t prev_ops = 0;
    double prev_time = 0.0;
    std::vector<memalloc::viz::AllocEvent> feed;
    std::size_t last_event_count = 0;
    bool scroll_to_bottom = true;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Deterministic layout on launch: anchor the three windows to the
        // main viewport's work area (position + size applied with
        // ImGuiCond_Once, so the user can still move/resize afterwards —
        // those changes are saved to imgui.ini). A stale imgui.ini would
        // otherwise win over the Once condition, so it is deleted at
        // launch time in the run script.
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const ImVec2 org = vp->WorkPos;
        const ImVec2 work = vp->WorkSize;

        // Left column, top: Global Stats.
        ImGui::SetNextWindowPos(ImVec2(org.x + 10, org.y + 10), ImGuiCond_Once);
        ImGui::SetNextWindowSize(ImVec2(440, 340), ImGuiCond_Once);
        draw_global_stats(history, history_count, ops_per_sec, prev_ops,
                          prev_time);

        // Left column, below Global Stats: Per-Thread Pools.
        ImGui::SetNextWindowPos(ImVec2(org.x + 10, org.y + 360), ImGuiCond_Once);
        ImGui::SetNextWindowSize(ImVec2(440, work.y - 380), ImGuiCond_Once);
        draw_per_thread_pools();

        // Right side, full height: Event Feed.
        ImGui::SetNextWindowPos(ImVec2(org.x + work.x - 540, org.y + 10),
                                ImGuiCond_Once);
        ImGui::SetNextWindowSize(ImVec2(530, work.y - 20), ImGuiCond_Once);
        draw_event_feed(feed, last_event_count, scroll_to_bottom);

        ImGui::Render();

        int display_w = 0, display_h = 0;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.06f, 0.06f, 0.22f, 1.00f);  // dark blue
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // Clean shutdown: stop the workers and JOIN them BEFORE tearing down
    // ImGui/GLFW. Worker allocators (and their stats) retire at thread exit.
    stop.store(true, std::memory_order_relaxed);
    for (std::thread& w : workers) {
        w.join();
    }
    workers.clear();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
