// Build: g++ -std=c++20 main.cpp -O3 -DNDEBUG -Isrc/Include -Lsrc/lib -lSDL3 -lSDL3_ttf -o sim_server -fopenmp

#include <cstdio>
#include <cstring>
#include <random>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <utility>
#include <algorithm>

#include "sim_server.hpp"   // start/stop/join, setPaused/isPaused, dtSeconds, sleepMillis, framesSimulated, world/worldMutex
#include "sim_render.hpp"   // run_world_ui(server)

// ===============================
// Helpers shared by both modes
// ===============================
struct SpiralCursor {
    int x = 0, z = 0;   // current
    int dir = 0;        // 0=+x, 1=+z, 2=-x, 3=-z
    int legLen = 1;     // steps on current side
    int stepsOnLeg = 0;
    int legsAtLen = 0;
    std::pair<int,int> next() {
        switch (dir) { case 0: ++x; break; case 1: ++z; break; case 2: --x; break; default: --z; break; }
        if (++stepsOnLeg >= legLen) { stepsOnLeg = 0; dir = (dir+1)&3; if (++legsAtLen==2) { legsAtLen=0; ++legLen; } }
        return {x,z};
    }
};

static int pick_empty_section(const Chunk& C, std::mt19937& rng) {
    std::vector<int> empty; empty.reserve(SECTIONS_Y);
    for (int sy=0; sy<SECTIONS_Y; ++sy) if (!C.sectionLoaded[sy]) empty.push_back(sy);
    if (empty.empty()) return -1;
    std::uniform_int_distribution<int> d(0, (int)empty.size()-1);
    return empty[d(rng)];
}

static void init_one_visible_section(SimServer& server) {
    std::unique_lock<std::mutex> lk(server.worldMutex);

    if (server.world.materials.size() == 0) {
        server.world.materials.add(Material{0.0f, 0.0f, 0.0f, 0.0f});                  // VOID
        server.world.materials.add(Material{500.0f, 100.0f, 1000.0f, 0.05f});          // SOLID
    }
    Chunk* c00 = server.world.ensureChunk(0,0);
    c00->void_ix = 0;

    const int sy = 8;
    fill_section_with(*c00, /*SOLID*/1, 300.0f, sy, server.world.materials); // NEW sig

    const int xMid = CHUNK_W/2, zMid = CHUNK_D/2, y0 = sy*SECTION_EDGE + SECTION_EDGE/2;
    const int iHot = idx(xMid, y0, zMid);
    c00->T_curr[iHot] = 6000.0f; c00->T_next[iHot] = 6000.0f;

    recomputeSectionLoaded(*c00);
}

// ===============================
// Console progress bar utilities
// ===============================
static inline void print_progress_bar(double world_ms, double target_ms,
                                      int width = 40, bool final_line = false)
{
    if (target_ms <= 0.0) target_ms = 1.0;
    double ratio = world_ms / target_ms;
    if (ratio < 0.0) ratio = 0.0;

    // cap visual fill to the bar width; show true ratio in text
    int filled = (int)std::round(std::min(1.0, ratio) * width);

    std::printf("\r[");
    for (int i=0;i<filled;++i) std::printf("#");
    for (int i=filled;i<width;++i) std::printf(" ");
    std::printf("]  %6.2f / %6.2f ms  (%.1f%%)   ",
                world_ms, target_ms, ratio * 100.0);
    std::fflush(stdout);

    if (final_line) {
        std::printf("\n");
        std::fflush(stdout);
    }
}

// ===============================
// Unified stress-growth worker
// ===============================
struct StressGrowthWorker {
    SimServer& server;
    std::atomic<bool> stop{false};
    std::atomic<bool> tripped{false};   // set once when world_ms > dt
    uint32_t seed = std::random_device{}();
    double   dt_seconds = 1.0;

    // throttle progress bar updates
    std::chrono::steady_clock::time_point lastBar{};
    const std::chrono::milliseconds barInterval{100};

    void print_summary_locked(uint32_t seedUsed, double world_ms) {
        // Assumes caller holds worldMutex
        size_t chunks = server.world.chunks.size();
        size_t sections_loaded = 0;
        double max_chunk = 0.0, sum_chunk = 0.0;
        for (const auto& kv : server.world.chunks) {
            const Chunk& C = *kv.second;
            max_chunk = std::max(max_chunk, C.chunk_ms_last);
            sum_chunk += C.chunk_ms_last;
            for (int sy=0; sy<SECTIONS_Y; ++sy) sections_loaded += (C.sectionLoaded[sy] ? 1 : 0);
        }
        std::printf("=== STRESS RESULT ===\n");
        std::printf("Seed: %u\n", seedUsed);
        std::printf("Target dt: %.3f ms\n", dt_seconds*1000.0);
        std::printf("Total chunks: %zu\n", chunks);
        std::printf("Total sections loaded: %zu (max per chunk: %d)\n", sections_loaded, SECTIONS_Y);
        std::printf("World frame time: %.3f ms  (max chunk: %.3f ms, sum: %.3f ms)\n\n",
                    world_ms, max_chunk, sum_chunk);
        std::fflush(stdout);
    }

    void operator()() {
        using namespace std::chrono;
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> d_heatCap(200.f, 1200.f);
        std::uniform_real_distribution<float> d_k(1.f, 500.f);
        std::uniform_real_distribution<float> d_mass(500.f, 4000.f);   // default mass per cell (kg)
        std::uniform_real_distribution<float> d_molar(0.01f, 0.10f);   // kg/mol
        std::uniform_real_distribution<float> d_temp(0.f, 6000.f);

        SpiralCursor spiral;
        Chunk* C = nullptr;
        {
            std::unique_lock<std::mutex> lk(server.worldMutex);
            C = server.world.ensureChunk(0,0);
        }

        lastBar = steady_clock::now();

        while (!stop.load()) {
            double world_ms = 0.0;
            {
                std::unique_lock<std::mutex> lk(server.worldMutex);
                world_ms = world_total_ms_last(server.world); // sum of last per-chunk ms
            }

            // progress bar (throttled)
            auto now = steady_clock::now();
            if (now - lastBar >= barInterval && !tripped.load()) {
                print_progress_bar(world_ms, dt_seconds * 1000.0);
                lastBar = now;
            }

            // Trip once: PAUSE sim, PRINT final bar + result, and STOP growth permanently
            if (world_ms > dt_seconds * 1000.0) {
                bool first = !tripped.exchange(true);
                if (first) {
                    // show a final "100%" bar line before the summary
                    print_progress_bar(world_ms, dt_seconds * 1000.0, 40, true);
                    server.setPaused(true); // clean pause on the sim thread
                    std::unique_lock<std::mutex> lk(server.worldMutex);
                    print_summary_locked(seed, world_ms);
                }
                stop = true; // end worker so no more growth ever happens
                break;
            }

            // Grow one step
            {
                std::unique_lock<std::mutex> lk(server.worldMutex);
                if (!C) C = server.world.ensureChunk(0,0);

                int sy = pick_empty_section(*C, rng);
                if (sy >= 0) {
                    const uint16_t MAT = server.world.materials.add(
                        Material{ d_heatCap(rng), d_k(rng), d_mass(rng), d_molar(rng) });
                    fill_section_with(*C, MAT, d_temp(rng), sy, server.world.materials);
                    recomputeSectionLoaded(*C);
                } else {
                    auto [ncx, ncz] = spiral.next();
                    C = server.world.ensureChunk(ncx, ncz);
                    C->void_ix = 0;
                    const uint16_t MAT = server.world.materials.add(
                        Material{ d_heatCap(rng), d_k(rng), d_mass(rng), d_molar(rng) });
                    const int sy0 = 8;
                    fill_section_with(*C, MAT, d_temp(rng), sy0, server.world.materials);
                    recomputeSectionLoaded(*C);
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    }
};

// ===============================
// Run stress (same sim+growth; render optional)
// ===============================
static int run_stress(bool attachRender, double dt_seconds, uint32_t seed) {
    SimServer server;
    server.dtSeconds = (float)dt_seconds;       // used directly by server worker
    server.sleepMillis.store(1);
    init_one_visible_section(server);

    StressGrowthWorker worker{server};
    worker.seed = seed;
    worker.dt_seconds = dt_seconds;

    server.start();                              // starts background sim thread
    std::thread growThread(std::ref(worker));

    std::thread uiThread;
    if (attachRender) {
        uiThread = std::thread([&](){ (void)run_world_ui(server); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // let SDL init
    }

    if (!attachRender) {
        // Headless: wait until tripped, then exit
        while (!worker.tripped.load()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } else {
        // With render: window stays open paused; Space toggles pause/resume (growth never restarts).
        if (uiThread.joinable()) uiThread.join();
    }

    // Teardown
    worker.stop = true;
    if (growThread.joinable()) growThread.join();
    server.stop();
    server.join();
    return 0;
}

// ===============================
// main
// ===============================
int main(int argc, char** argv) {
    bool headless = false;
    bool stress   = false;

    for (int i=1; i<argc; ++i) {
        if (std::strcmp(argv[i], "--headless")==0) headless = true;
        else if (std::strcmp(argv[i], "--stress")==0) stress = true;
    }

    if (stress) {
        // Same stress logic; only toggle whether the render thread is attached
        return run_stress(/*attachRender=*/!headless, /*dt_seconds=*/1.0, /*seed=*/std::random_device{}());
    }

    // Normal interactive / headless (no stress workload)
    SimServer server;
    server.dtSeconds = 1.0f;
    init_one_visible_section(server);
    server.start();

    if (headless) {
        std::puts("Headless server running. Press Ctrl+C to exit.");
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            auto frames = server.framesSimulated.load();
            std::printf("frames=%llu\n", (unsigned long long)frames);
        }
    }

    // Renderer on its own thread so modules remain independent
    std::thread uiThread([&](){ (void)run_world_ui(server); });
    if (uiThread.joinable()) uiThread.join();

    server.stop();
    server.join();
    return 0;
}

//g++ -std=c++20 main.cpp -O3 -DNDEBUG -Isrc/Include -Lsrc/lib -lSDL3 -lSDL3_ttf -o sim_server -fopenmp