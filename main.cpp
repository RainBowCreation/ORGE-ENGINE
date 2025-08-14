#include <cstdio>
#include <cstring>
#include <random>
#include <chrono>
#include <thread>
#include <atomic>

#include "sim_server.hpp"
#include "sim_render.hpp"  // uses SimServer, renders only

// ===============================
// Helpers shared by both modes
// ===============================

// Spiral cursor for chunk-placement: clockwise, starting at (0,0),
// then (1,0) → (1,1) → (0,1) → (-1,1) → (-1,0) → (-1,-1) → (0,-1) → (1,-1) → (2,-1) → ...
struct SpiralCursor {
    int x = 0, z = 0;   // current
    int dir = 0;        // 0=+x, 1=+z, 2=-x, 3=-z
    int legLen = 1;     // number of steps for current side length
    int stepsOnLeg = 0; // steps taken on current leg
    int legsAtLen = 0;  // how many legs done at this legLen

    // Advance one step and return new (x,z)
    std::pair<int,int> next() {
        switch (dir) {
            case 0: ++x; break; // +x (east)
            case 1: ++z; break; // +z (south)
            case 2: --x; break; // -x (west)
            default: --z; break; // -z (north)
        }
        if (++stepsOnLeg >= legLen) {
            stepsOnLeg = 0;
            dir = (dir + 1) & 3;
            if (++legsAtLen == 2) { legsAtLen = 0; ++legLen; } // increase after every 2 legs
        }
        return {x,z};
    }
};

// Throttle helper to keep CPU use roughly <= 70% by idling a fraction after each step.
static inline void cpu_throttle_sleep(double busy_ms, double targetBusyRatio = 0.70) {
    using namespace std::chrono;
    if (busy_ms <= 0.0) { std::this_thread::sleep_for(milliseconds(1)); return; }
    const double factor = (1.0 / targetBusyRatio) - 1.0; // ~0.4286 for 70%
    const double sleep_ms = std::max(1.0, busy_ms * factor);
    std::this_thread::sleep_for(milliseconds((long)std::ceil(sleep_ms)));
}

// Choose the next empty section index within a chunk; returns -1 if full.
static int pick_empty_section(const Chunk& C, std::mt19937& rng) {
    std::vector<int> empty;
    empty.reserve(SECTIONS_Y);
    for (int sy=0; sy<SECTIONS_Y; ++sy) if (!C.sectionLoaded[sy]) empty.push_back(sy);
    if (empty.empty()) return -1;
    std::uniform_int_distribution<int> d(0, (int)empty.size()-1);
    return empty[d(rng)];
}

// ===============================
// Headless STRESS TEST (spiral)
// ===============================
static int run_stress_test(double dt_seconds = 1.0, uint32_t seed = std::random_device{}()) {
    using clock = std::chrono::steady_clock;
    using nsec  = std::chrono::nanoseconds;

    std::mt19937 rng(seed);

    // Distributions for random materials and temperatures
    std::uniform_real_distribution<float> d_heatCap(200.f, 1200.f);  // J/(kg*K)
    std::uniform_real_distribution<float> d_k(1.f, 500.f);           // W/(m*K)
    std::uniform_real_distribution<float> d_rho(500.f, 4000.f);      // kg/m^3
    std::uniform_real_distribution<float> d_temp(0.f, 6000.f);       // Kelvin

    World world;

    // Add VOID and the first random SOLID to start with
    const uint16_t VOID_IX   = world.materials.add(Material{0.f, 0.f, 0.f});
    const uint16_t FIRST_MAT = world.materials.add(Material{d_heatCap(rng), d_k(rng), d_rho(rng)});

    // Start chunk (0,0), single filled section so we have diffusion
    Chunk* C = world.ensureChunk(0,0);
    C->void_ix = VOID_IX;
    fill_section_with(*C, FIRST_MAT, d_temp(rng), /*sy*/ 8);
    recomputeSectionLoaded(*C);

    // Spiral state (next chunk will be at spiral.next())
    SpiralCursor spiral; // current at (0,0)

    size_t total_sections_loaded = 1;

    while (true) {
        auto t0 = clock::now();
        compute_frame_to_backbuffers(world, (float)dt_seconds);
        auto t1 = clock::now();
        swap_all_backbuffers(world);
        auto t2 = clock::now();

        const double world_ms = world_total_ms_last(world);
        const double wall_compute_ms = std::chrono::duration_cast<nsec>(t1 - t0).count() / 1'000'000.0;

        cpu_throttle_sleep(std::max(world_ms, wall_compute_ms), 0.70);

        std::printf("[world] chunks=%zu sections=%zu  frame_ms=%.3f  (wall=%.3f)  dt=%.3f\n",
                    world.chunks.size(), total_sections_loaded, world_ms, wall_compute_ms, dt_seconds*1000.0);

        if (world_ms > dt_seconds * 1000.0) {
            double max_chunk = 0.0, sum_chunk = 0.0;
            for (const auto& kv : world.chunks) {
                max_chunk = std::max(max_chunk, kv.second->chunk_ms_last);
                sum_chunk += kv.second->chunk_ms_last;
            }
            std::printf("\n=== STRESS TEST PAUSED (headless) ===\n");
            std::printf("Seed: %u\n", seed);
            std::printf("Target dt: %.3f ms\n", dt_seconds*1000.0);
            std::printf("Total chunks: %zu\n", world.chunks.size());
            std::printf("Total sections loaded: %zu (max per chunk: %d)\n", total_sections_loaded, SECTIONS_Y);
            std::printf("World frame time: %.3f ms  (max chunk: %.3f ms, sum: %.3f ms)\n\n", world_ms, max_chunk, sum_chunk);
            break;
        }

        // Grow: random empty section in current chunk; else new chunk on spiral
        int sy = pick_empty_section(*C, rng);
        if (sy >= 0) {
            const uint16_t MAT = world.materials.add(Material{d_heatCap(rng), d_k(rng), d_rho(rng)});
            fill_section_with(*C, MAT, d_temp(rng), sy);
            ++total_sections_loaded;
        } else {
            auto [ncx, ncz] = spiral.next();
            C = world.ensureChunk(ncx, ncz);
            C->void_ix = VOID_IX;
            const uint16_t MAT = world.materials.add(Material{d_heatCap(rng), d_k(rng), d_rho(rng)});
            const int sy0 = 8; // first section for new chunk
            fill_section_with(*C, MAT, d_temp(rng), sy0);
            recomputeSectionLoaded(*C);
            ++total_sections_loaded;
            std::printf("  -> new chunk created at (%d,%d)\n", ncx, ncz);
        }
    }

    return 0;
}

// =======================================
// VISUAL STRESS MODE (stress + render)
// =======================================
//
// A small controller that keeps adding content (sections/chunks in spiral order)
// while the measured world frame time <= dt. Once it exceeds dt, it *stops growing*
// but leaves the sim and UI running so you can pause/inspect with Space/ESC.
struct VisualStressController {
    SimServer& server;
    std::atomic<bool> stop{false};
    uint32_t seed = std::random_device{}();
    double dt_seconds = 1.0;

    void operator()() {
        using namespace std::chrono;
        std::mt19937 rng(seed);

        std::uniform_real_distribution<float> d_heatCap(200.f, 1200.f);
        std::uniform_real_distribution<float> d_k(1.f, 500.f);
        std::uniform_real_distribution<float> d_rho(500.f, 4000.f);
        std::uniform_real_distribution<float> d_temp(0.f, 6000.f);

        // Spiral centered at (0,0) (we assume main created chunk (0,0) already)
        SpiralCursor spiral;

        // Current chunk we’re filling
        Chunk* C = nullptr;

        // Make sure we start from (0,0)
        {
            std::unique_lock<std::mutex> lk(server.worldMutex);
            C = server.world.ensureChunk(0,0);
        }

        while (!stop.load()) {
            // 1) Measure current world frame time (sum of chunk_ms_last)
            double world_ms = 0.0;
            {
                std::unique_lock<std::mutex> lk(server.worldMutex);
                world_ms = world_total_ms_last(server.world);
            }

            if (world_ms > dt_seconds * 1000.0) {
                // Growth halts. User can manually pause and inspect via UI.
                std::this_thread::sleep_for(milliseconds(50));
                continue;
            }

            // 2) Attempt to add a random empty section; otherwise advance spiral & create a new chunk
            {
                std::unique_lock<std::mutex> lk(server.worldMutex);

                // Re-acquire pointer (chunk may already exist)
                if (!C) C = server.world.ensureChunk(0,0);

                int sy = pick_empty_section(*C, rng);
                if (sy >= 0) {
                    const uint16_t MAT = server.world.materials.add(Material{d_heatCap(rng), d_k(rng), d_rho(rng)});
                    fill_section_with(*C, MAT, d_temp(rng), sy);
                    recomputeSectionLoaded(*C);
                } else {
                    // Move to next spiral location and seed a new chunk with one section
                    auto [ncx, ncz] = spiral.next();
                    C = server.world.ensureChunk(ncx, ncz);
                    C->void_ix = 0; // material 0 is void (created in main)
                    const uint16_t MAT = server.world.materials.add(Material{d_heatCap(rng), d_k(rng), d_rho(rng)});
                    const int sy0 = 8;
                    fill_section_with(*C, MAT, d_temp(rng), sy0);
                    recomputeSectionLoaded(*C);
                }
            }

            // 3) Don’t hog CPU
            std::this_thread::sleep_for(milliseconds(4));
        }
    }
};

// =======================================
// Regular interactive init (shared)
// =======================================
static void init_one_visible_section(SimServer& server) {
    std::unique_lock<std::mutex> lk(server.worldMutex);

    // Materials: 0 = void, 1 = solid (baseline)
    if (server.world.materials.size() == 0) {
        server.world.materials.add(Material{0.0f,   0.0f,   0.0f});   // VOID index 0
        server.world.materials.add(Material{500.0f,100.0f,1000.0f}); // SOLID index 1
    }

    // Ensure one chunk exists at (0,0)
    Chunk* c00 = server.world.ensureChunk(0,0);
    c00->void_ix = 0; // void = 0

    // Fill exactly ONE section fully (e.g., sy = 8) with solid at 300 K
    const int sectionY = 8; // 0..23
    fill_section_with(*c00, /*SOLID*/ 1, 300.0f, sectionY);

    // Tiny heater so you can watch diffusion immediately
    const int xMid = CHUNK_W/2, zMid = CHUNK_D/2, y0 = sectionY*SECTION_EDGE + SECTION_EDGE/2;
    const int iHot = idx(xMid, y0, zMid);
    c00->T_curr[iHot] = 6000.0f;
    c00->T_next[iHot] = 6000.0f;

    recomputeSectionLoaded(*c00);
}

// =======================================
// main
// =======================================
int main(int argc, char** argv) {
    bool headless = false;
    bool stress   = false;
    bool wantRender = true; // default interactive render
    uint32_t seed = std::random_device{}();

    for (int i=1;i<argc;++i) {
        if (std::strcmp(argv[i], "--headless")==0) { headless = true; wantRender = false; }
        else if (std::strcmp(argv[i], "--stress")==0) { stress = true; }            // stress on
        else if (std::strcmp(argv[i], "--render")==0) { wantRender = true; }        // explicit render (with stress)
        else if (std::strcmp(argv[i], "--no-render")==0) { wantRender = false; }    // explicit headless
        else if (std::strcmp(argv[i], "--seed")==0 && i+1<argc) { seed = (uint32_t)std::stoul(argv[++i]); }
    }

    // Mode 1: headless stress (no rendering)
    if (stress && !wantRender) {
        return run_stress_test(1.0 /*dt seconds*/, seed);
    }

    // Interactive server
    SimServer server;
    server.dtSeconds = 1.0f; // ΔT per frame = 1s
    init_one_visible_section(server);

    // Mode 2: visual stress (stress + render)
    std::thread stressThread;
    VisualStressController controller{server};
    controller.seed = seed;
    controller.dt_seconds = 1.0;

    if (stress && wantRender) {
        // Start sim + growth + UI
        server.start();
        stressThread = std::thread(std::ref(controller));
        int rc = run_world_ui(server);
        controller.stop = true;
        if (stressThread.joinable()) stressThread.join();
        server.stop();
        server.join();
        return rc;
    }

    // Mode 3: normal interactive (no stress)
    if (!wantRender) {
        // server-only
        server.start();
        std::puts("Sim server running headless. Press Ctrl+C to exit.");
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            auto frames = server.framesSimulated.load();
            std::printf("frames=%llu\n", (unsigned long long)frames);
        }
        return 0;
    }

    // Normal render
    server.start();
    int rc = run_world_ui(server);
    server.stop();
    server.join();
    return rc;
}

// g++ -std=c++20 main.cpp -O3 -DNDEBUG -Isrc/Include -Lsrc/lib -lSDL3 -lSDL3_ttf -o therm_world -fopenmp