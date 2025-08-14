#pragma once
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include "sim_engine.hpp"

// Small server that owns the world and advances it on a background thread.
class SimServer {
public:
    World world;

    // Readers (renderer) should lock this mutex to snapshot/draw safely.
    std::mutex worldMutex;

    // Control
    std::atomic<bool> running{false};
    std::atomic<bool> paused{false};
    float dtSeconds = 1.0f; // DeltaT per simulation step (1 sec)

    // Optional: micro-pause after each frame to reduce CPU (set 0 for max speed)
    std::atomic<int> sleepMillis{1};

    // Stats
    std::atomic<uint64_t> framesSimulated{0};

    SimServer() = default;
    ~SimServer() { stop(); join(); }

    void start() {
        if (running.load()) return;
        running = true;
        worker = std::thread([this]{ this->runLoop(); });
    }

    void stop() {
        running = false;
        cv.notify_all();
    }

    void join() {
        if (worker.joinable()) worker.join();
    }

    void setPaused(bool p) {
        paused = p;
        if (!p) cv.notify_all();
    }
    bool isPaused() const { return paused.load(); }

    // Manual single step (headless / tests)
    void stepOnce() {
        // Compute without lock
        compute_frame_to_backbuffers(world, dtSeconds);
        // Publish with a very short lock
        {
            std::unique_lock<std::mutex> lk(worldMutex);
            swap_all_backbuffers(world);
        }
        ++framesSimulated;
    }

private:
    std::thread worker;
    std::condition_variable cv;
    std::mutex cvMutex;

    void runLoop() {
        using namespace std::chrono_literals;
        while (running.load()) {
            if (paused.load()) {
                std::unique_lock<std::mutex> lk(cvMutex);
                cv.wait_for(lk, 5ms, [&]{ return !paused.load() || !running.load(); });
                continue;
            }

            // 1) heavy work WITHOUT the lock
            compute_frame_to_backbuffers(world, dtSeconds);

            // 2) quick publish WITH the lock (O(1) vector swaps)
            {
                std::unique_lock<std::mutex> lk(worldMutex);
                swap_all_backbuffers(world);
            }

            ++framesSimulated;

            // small configurable nap to keep CPU sane (set sleepMillis=0 for flat out)
            int ms = sleepMillis.load();
            if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            else        std::this_thread::yield();
        }
    }
};
