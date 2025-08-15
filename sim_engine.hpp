#pragma once
#include <array>
#include <cstdint>
#include <unordered_map>
#include <memory>
#include <chrono>
#include <vector>
#include <utility>
#include <algorithm>

// ====== Dimensions (Minecraft-like): 16 x 384 x 16 per chunk ======
constexpr int CHUNK_W = 16;   // X
constexpr int CHUNK_H = 384;  // Y
constexpr int CHUNK_D = 16;   // Z
constexpr int CHUNK_N = CHUNK_W * CHUNK_H * CHUNK_D;

// Sections are 16x16x16. In this layout: 1 section in X, 24 in Y, 1 in Z.
constexpr int SECTION_EDGE = 16;
constexpr int SECTIONS_X = CHUNK_W / SECTION_EDGE;  // 1
constexpr int SECTIONS_Y = CHUNK_H / SECTION_EDGE;  // 24
constexpr int SECTIONS_Z = CHUNK_D / SECTION_EDGE;  // 1
static_assert(SECTIONS_X == 1 && SECTIONS_Z == 1, "Expected 16x384x16 chunk -> 1x24x1 sections");

inline int idx(int x, int y, int z) { return x + y*CHUNK_W + z*CHUNK_W*CHUNK_H; }

// ====== Materials (indexed to save memory) ======
struct Material {
    float heatCapacity;        // J/(kg*K)
    float thermalConductivity; // W/(m*K)
    float defaultMass;         // kg per cell (cell is 1 m^3)
    float molarMass;           // kg/mol
};

struct MaterialLUT {
    std::vector<Material> table;
    uint16_t add(const Material& m) {
        table.push_back(m);
        return static_cast<uint16_t>(table.size()-1);
    }
    const Material& byIx(uint16_t ix) const { return table[ix]; }
    size_t size() const noexcept { return table.size(); }
    bool   empty() const noexcept { return table.empty(); }
    void   clear() noexcept { table.clear(); }
};

// ====== Chunk ======
struct Chunk {
    std::array<uint16_t, CHUNK_N> matIx{};  // material index per cell (0=void recommended)

    // Temperatures
    std::vector<float> T_curr; // K (front buffer)
    std::vector<float> T_next; // K (back buffer)

    // NEW: mass map (kg per 1 m^3 cell)
    std::vector<float> mass_kg;

    uint16_t void_ix = 0;
    int cx = 0;
    int cz = 0;

    // -------- per-frame timings --------
    double chunk_ms_last = 0.0;                         // sum of sections (last frame)
    std::array<double, SECTIONS_Y> section_ms_last{};   // ms per section (last frame)

    // -------- which sections are "loaded"/exist --------
    std::array<uint8_t, SECTIONS_Y> sectionLoaded{};    // 1 = has any non-void voxel

    Chunk()
        : T_curr(CHUNK_N, 0.0f)
        , T_next(CHUNK_N, 0.0f)
        , mass_kg(CHUNK_N, 0.0f)
    {
        section_ms_last.fill(0.0);
        sectionLoaded.fill(0);
    }
};

// ====== World ======
struct ChunkCoord { int cx, cz; bool operator==(const ChunkCoord& o) const { return cx==o.cx && cz==o.cz; } };
struct CoordHasher { size_t operator()(const ChunkCoord& k) const noexcept { return (std::hash<int>()(k.cx) << 1) ^ std::hash<int>()(k.cz); } };

struct World {
    std::unordered_map<ChunkCoord, std::unique_ptr<Chunk>, CoordHasher> chunks;
    MaterialLUT materials;

    Chunk* ensureChunk(int cx, int cz) {
        ChunkCoord key{cx,cz};
        auto it = chunks.find(key);
        if (it != chunks.end()) return it->second.get();
        auto ptr = std::make_unique<Chunk>();
        ptr->cx = cx; ptr->cz = cz;
        Chunk* raw = ptr.get();
        chunks.emplace(key, std::move(ptr));
        return raw;
    }
    Chunk* findChunk(int cx, int cz) const {
        auto it = chunks.find(ChunkCoord{cx,cz});
        return (it==chunks.end()) ? nullptr : it->second.get();
    }
};

// ====== Helpers to mark which sections exist (non-void) ======
inline void recomputeSectionLoaded(Chunk& C) {
    C.sectionLoaded.fill(0);
    for (int sy=0; sy<SECTIONS_Y; ++sy) {
        const int y0 = sy * SECTION_EDGE;
        const int y1 = y0 + SECTION_EDGE;
        bool any = false;
        for (int z=0; z<CHUNK_D && !any; ++z) {
            for (int y=y0; y<y1 && !any; ++y) {
                for (int x=0; x<CHUNK_W; ++x) {
                    if (C.matIx[idx(x,y,z)] != C.void_ix) { any = true; break; }
                }
            }
        }
        C.sectionLoaded[sy] = any ? 1 : 0;
    }
}
inline void markSectionLoaded(Chunk& C, int sy, bool loaded) {
    if (sy >= 0 && sy < SECTIONS_Y) C.sectionLoaded[sy] = loaded ? 1 : 0;
}
inline void recomputeAllSectionLoaded(World& world) {
    for (auto& kv : world.chunks) recomputeSectionLoaded(*kv.second);
}

// ====== Neighbor sampling across chunk borders ======
struct NeighborSample {
    float     T;         // neighbor temperature
    uint16_t  mix;       // neighbor material index
    bool      exists;    // true if a cell exists (inside world), false = treat as void
};
inline NeighborSample sample_neighbor_T(const World& world, const Chunk& C, int x, int y, int z, int dx, int dy, int dz) {
    int nx = x + dx, ny = y + dy, nz = z + dz;

    if (ny < 0 || ny >= CHUNK_H) {
        return NeighborSample{0.0f, C.void_ix, false};
    }

    int ncx = C.cx, ncz = C.cz;
    int lx  = nx,   lz  = nz;

    if (nx < 0)           { ncx = C.cx - 1; lx = CHUNK_W - 1; }
    else if (nx >= CHUNK_W){ ncx = C.cx + 1; lx = 0; }
    if (nz < 0)           { ncz = C.cz - 1; lz = CHUNK_D - 1; }
    else if (nz >= CHUNK_D){ ncz = C.cz + 1; lz = 0; }

    const Chunk* CC = &C;
    if (ncx != C.cx || ncz != C.cz) {
        CC = world.findChunk(ncx, ncz);
        if (!CC) return NeighborSample{0.0f, C.void_ix, false};
    }

    int i = idx(lx, ny, lz);
    return NeighborSample{ CC->T_curr[i], CC->matIx[i], true };
}

// ====== SIMULATION CORE ======
inline void simulate_section_16x16x16(World& world, Chunk& C, const MaterialLUT& mats, int sy, float dt_seconds) {
    const int y0 = sy * SECTION_EDGE;
    const int y1 = y0 + SECTION_EDGE;
    constexpr float inv_dx2 = 1.0f;

    for (int z=0; z<CHUNK_D; ++z) {
        for (int y=y0; y<y1; ++y) {
            for (int x=0; x<CHUNK_W; ++x) {
                const int i = idx(x,y,z);
                const uint16_t mix = C.matIx[i];
                if (mix == C.void_ix) { C.T_next[i] = C.T_curr[i]; continue; }

                const Material& m  = mats.byIx(mix);
                // Thermal capacity of this cell = mass(kg) * heatCapacity(J/kg*K)
                const float Cth    = std::max(1e-8f, C.mass_kg[i] * m.heatCapacity);
                const float Tc     = C.T_curr[i];

                NeighborSample nb[6] = {
                    sample_neighbor_T(world, C, x,y,z, +1, 0, 0),
                    sample_neighbor_T(world, C, x,y,z, -1, 0, 0),
                    sample_neighbor_T(world, C, x,y,z,  0,+1, 0),
                    sample_neighbor_T(world, C, x,y,z,  0,-1, 0),
                    sample_neighbor_T(world, C, x,y,z,  0, 0,+1),
                    sample_neighbor_T(world, C, x,y,z,  0, 0,-1),
                };

                float dT = 0.0f;
                for (int n=0;n<6;++n) {
                    if (!nb[n].exists) continue;
                    const Material& mn = mats.byIx(nb[n].mix);
                    const float k1 = m.thermalConductivity, k2 = mn.thermalConductivity;
                    float k_eff = 0.0f;
                    //if (k1 > 0.0f && k2 > 0.0f) k_eff = 2.0f * k1 * k2 / (k1 + k2);
                    //else                        k_eff = std::max(k1, k2);
                    if (k1 <= 0.0f || k2 <= 0.0f) {
                        k_eff = 0.0f;
                    } else {
                        k_eff = 2.0f * k1 * k2 / (k1 + k2);
                    }
                    dT += (k_eff * (nb[n].T - Tc)) * inv_dx2;
                }

                float Tnew = Tc + (dt_seconds / Cth) * dT;
                if      (Tnew <   0.0f) Tnew = 0.0f;
                else if (Tnew > 6000.0f) Tnew = 6000.0f;
                C.T_next[i] = Tnew;
            }
        }
    }
}

// ====== Frame functions (compute without lock, swap with O(1) under lock) ======
inline void compute_frame_to_backbuffers(World& world, float dt_seconds) {
    using clock = std::chrono::steady_clock;
    using nsec  = std::chrono::nanoseconds;

    for (auto& kv : world.chunks) {
        Chunk& C = *kv.second;
        C.chunk_ms_last = 0.0;
        C.section_ms_last.fill(0.0);

        for (int sy=0; sy<SECTIONS_Y; ++sy) {
            if (!C.sectionLoaded[sy]) continue;
            auto s0 = clock::now();
            simulate_section_16x16x16(world, C, world.materials, sy, dt_seconds);
            auto s1 = clock::now();
            double ms = std::chrono::duration_cast<nsec>(s1 - s0).count() / 1'000'000.0;
            C.section_ms_last[sy] = ms;
            C.chunk_ms_last      += ms;
        }
        // NOTE: no swap here; we only filled T_next
    }
}

inline void swap_all_backbuffers(World& world) {
    for (auto& kv : world.chunks) {
        Chunk& C = *kv.second;
        std::swap(C.T_curr, C.T_next); // O(1) vector swap
    }
}

// Legacy combined step (kept for single-threaded callers if you ever need it):
inline void step_frame(World& world, float dt_seconds) {
    compute_frame_to_backbuffers(world, dt_seconds);
    swap_all_backbuffers(world);
}

// ====== Fill one entire 16x16x16 section ======
// NOTE: now needs mats to set per-voxel mass to material.defaultMass when unspecified.
inline void fill_section_with(Chunk& C, uint16_t mat_ix, float T, int sy, const MaterialLUT& mats) {
    if (sy < 0 || sy >= SECTIONS_Y) return;
    const float mdef = mats.byIx(mat_ix).defaultMass;
    const int y0 = sy * SECTION_EDGE;
    const int y1 = y0 + SECTION_EDGE;
    for (int z = 0; z < CHUNK_D; ++z) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < CHUNK_W; ++x) {
                const int i = idx(x,y,z);
                C.matIx[i]   = mat_ix;
                C.T_curr[i]  = T;
                C.T_next[i]  = T;
                C.mass_kg[i] = (mat_ix == C.void_ix) ? 0.0f : mdef;
            }
        }
    }
    markSectionLoaded(C, sy, (mat_ix != C.void_ix));
}

// Sum of per-chunk elapsed ms from the most recent frame.
inline double world_total_ms_last(const World& world) {
    double total = 0.0;
    for (const auto& kv : world.chunks) total += kv.second->chunk_ms_last;
    return total;
}