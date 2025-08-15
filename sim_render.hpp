#pragma once
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <string>
#include <limits>
#include <cstdio>
#include <optional>
#include <cmath>
#include <mutex>
#include "sim_engine.hpp"
#include "sim_server.hpp"
#include "tiny_ttf.hpp"

// ---------- Color map ----------
static inline SDL_Color temperatureToColor(float temp, float scaleMin, float scaleMax) {
    if (scaleMax - scaleMin < 1e-6f) return SDL_Color{0,0,0,255};
    float t = (temp - scaleMin) / (scaleMax - scaleMin);
    t = std::clamp(t, 0.0f, 1.0f);
    float r = std::clamp(255.0f * (2.0f*t - 0.5f), 0.0f, 255.0f);
    float g = std::clamp(255.0f * (1.0f - std::fabs(2.0f*t - 1.0f)), 0.0f, 255.0f);
    float b = std::clamp(255.0f * (1.0f - 2.0f*t), 0.0f, 255.0f);
    return SDL_Color{(Uint8)r,(Uint8)g,(Uint8)b,255};
}

// ---------- Header/UI ----------
struct UIStyle {
    int headerHeight = 64;     // room for gradient + text
    int pixelScale   = 4;      // cell pixels in chunk view
    int mapTileSize  = 64;     // per-chunk tile size in world map
};
static inline TTF_Font* loadTinyFont(int pt = 18) {
    SDL_IOStream* io = SDL_IOFromConstMem(tiny_ttf, tiny_ttf_len);
    if (!io) return nullptr;
    return TTF_OpenFontIO(io, true, (float)pt);
}
static inline void drawColorGradientHeader(SDL_Renderer* renderer, int windowWidth, float scaleMin, float scaleMax, const UIStyle&) {
    const int barHeight = 20;
    const int barY = 10;
    for (int x = 0; x < windowWidth; ++x) {
        float temp = scaleMin + (scaleMax - scaleMin) * (float(x) / std::max(1, windowWidth - 1));
        SDL_Color color = temperatureToColor(temp, scaleMin, scaleMax);
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, 255);
        SDL_FRect rect = { (float)x, (float)barY, 1.0f, (float)barHeight };
        SDL_RenderFillRect(renderer, &rect);
    }
}
static inline void drawText(SDL_Renderer* r, TTF_Font* f, const std::string& text, float x, float y) {
    if (!f) return;
    SDL_Color color{255,255,255,255};
    SDL_Surface* surf = TTF_RenderText_Solid(f, text.c_str(), (int)text.size(), color);
    if (!surf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
    if (!tex) { SDL_DestroySurface(surf); return; }
    SDL_FRect dst{ x, y, (float)surf->w, (float)surf->h };
    // simple drop shadow
    SDL_FRect shadow{ x+1, y+1, (float)surf->w, (float)surf->h };
    SDL_SetTextureColorMod(tex, 0,0,0); SDL_RenderTexture(r, tex, nullptr, &shadow);
    SDL_SetTextureColorMod(tex, 255,255,255); SDL_RenderTexture(r, tex, nullptr, &dst);
    SDL_DestroyTexture(tex);
    SDL_DestroySurface(surf);
}
static inline void drawTextCentered(SDL_Renderer* r, TTF_Font* f, const std::string& text, float cx, float cy) {
    if (!f) return;
    SDL_Color color{255,255,255,255};
    SDL_Surface* surf = TTF_RenderText_Solid(f, text.c_str(), (int)text.size(), color);
    if (!surf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
    if (!tex) { SDL_DestroySurface(surf); return; }
    SDL_FRect dst{ cx - surf->w*0.5f, cy - surf->h*0.5f, (float)surf->w, (float)surf->h };
    SDL_FRect shadow{ dst.x+1, dst.y+1, dst.w, dst.h };
    SDL_SetTextureColorMod(tex, 0,0,0); SDL_RenderTexture(r, tex, nullptr, &shadow);
    SDL_SetTextureColorMod(tex, 255,255,255); SDL_RenderTexture(r, tex, nullptr, &dst);
    SDL_DestroyTexture(tex);
    SDL_DestroySurface(surf);
}

// ---------- Helpers ----------
static inline std::optional<std::pair<float,float>> chunk_minmax_nonvoid(const Chunk& C) {
    const uint16_t void_ix = C.void_ix;
    float mn = std::numeric_limits<float>::max();
    float mx = std::numeric_limits<float>::lowest();
    bool any=false;
    for (int i=0;i<CHUNK_N;++i) {
        if (C.matIx[i]==void_ix) continue;
        float v = C.T_curr[i];
        mn = std::min(mn, v);
        mx = std::max(mx, v);
        any=true;
    }
    if (!any) return std::nullopt;
    return std::make_pair(mn,mx);
}
static inline std::optional<float> chunk_avg_nonvoid(const Chunk& C) {
    const uint16_t void_ix = C.void_ix;
    double sum = 0.0;
    size_t cnt = 0;
    for (int i=0;i<CHUNK_N;++i) {
        if (C.matIx[i]==void_ix) continue;
        sum += C.T_curr[i];
        ++cnt;
    }
    if (!cnt) return std::nullopt;
    return static_cast<float>(sum / (double)cnt);
}
static inline std::pair<float,float> slice_minmax_nonvoid(const Chunk& C, int z) {
    const uint16_t void_ix = C.void_ix;
    float mn = std::numeric_limits<float>::max();
    float mx = std::numeric_limits<float>::lowest();
    bool any=false;
    const int base = z * CHUNK_W * CHUNK_H;
    for (int i=0;i<CHUNK_W*CHUNK_H;++i) {
        if (C.matIx[base+i]==void_ix) continue;
        float v = C.T_curr[base+i];
        mn = std::min(mn, v);
        mx = std::max(mx, v);
        any=true;
    }
    if (!any) return {0.f,6000.f};
    return {mn,mx};
}

// ---------- View state ----------
enum class RenderMode { WorldMap, ChunkView };
struct WorldView {
    RenderMode mode = RenderMode::WorldMap;
    UIStyle st{};
    bool ctrl = false;
    bool shift = false;
    int frame = 0; // snapshot of server.framesSimulated

    // world map
    int sel_cx = 0, sel_cz = 0;
    // chunk view
    int zSlice = CHUNK_D / 2;
    int focus_cx = 0, focus_cz = 0;
};

static inline void init_view_from_world(WorldView& v, const World& world) {
    if (world.chunks.size() <= 1) {
        v.mode = RenderMode::ChunkView;
        if (!world.chunks.empty()) {
            auto it = world.chunks.begin();
            v.focus_cx = it->first.cx;
            v.focus_cz = it->first.cz;
            v.sel_cx = v.focus_cx;
            v.sel_cz = v.focus_cz;
        }
    } else {
        v.mode = RenderMode::WorldMap;
        auto it = world.chunks.begin();
        v.sel_cx = it->first.cx;
        v.sel_cz = it->first.cz;
    }
}
static inline std::string fmt_ms(double ms) {
    if (ms < 0.001) return "<0.001";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", ms);
    return std::string(buf);
}

// ---------- World Map ----------
static inline void render_world_map(SDL_Renderer* r, TTF_Font* font,
                                    const World& world, bool paused, WorldView& v,
                                    int winW, int winH)
{
    const int header = v.st.headerHeight;
    const int tile   = v.st.mapTileSize;

    int minCX= v.sel_cx, maxCX= v.sel_cx;
    int minCZ= v.sel_cz, maxCZ= v.sel_cz;
    for (const auto& kv : world.chunks) {
        minCX = std::min(minCX, kv.first.cx); maxCX = std::max(maxCX, kv.first.cx);
        minCZ = std::min(minCZ, kv.first.cz); maxCZ = std::max(maxCZ, kv.first.cz);
    }

    float scaleMin = 0.0f, scaleMax = 6000.0f;
    if (v.ctrl) {
        float mn = std::numeric_limits<float>::max();
        float mx = std::numeric_limits<float>::lowest();
        bool any=false;
        for (const auto& kv : world.chunks) {
            auto mm = chunk_minmax_nonvoid(*kv.second);
            if (!mm) continue;
            mn = std::min(mn, mm->first);
            mx = std::max(mx, mm->second);
            any=true;
        }
        if (any && mn<=mx) { scaleMin = mn; scaleMax = mx; }
    }

    SDL_SetRenderDrawColor(r, 0,0,0,255);
    SDL_RenderClear(r);

    double total_ms_all_chunks = 0.0;
    size_t chunks_with_work = 0;

    for (int cz = minCZ; cz <= maxCZ; ++cz) {
        for (int cx = minCX; cx <= maxCX; ++cx) {
            const int ox = (cx - minCX) * tile + 10;
            const int oy = header + (cz - minCZ) * tile + 10;

            const Chunk* C = world.findChunk(cx, cz);
            SDL_Color col{0,0,0,255};
            if (C) {
                auto avg = chunk_avg_nonvoid(*C);
                if (avg) col = temperatureToColor(*avg, scaleMin, scaleMax);
                total_ms_all_chunks += C->chunk_ms_last;
                if (C->chunk_ms_last > 0.0) ++chunks_with_work;
            }
            SDL_SetRenderDrawColor(r, col.r, col.g, col.b, 255);
            SDL_FRect rect{ (float)ox, (float)oy, (float)tile, (float)tile };
            SDL_RenderFillRect(r, &rect);

            SDL_SetRenderDrawColor(r, 40,40,40,255);
            SDL_RenderRect(r, &rect);

            if (cx==v.sel_cx && cz==v.sel_cz) {
                SDL_SetRenderDrawColor(r, 255,255,255,255);
                SDL_FRect sel{ rect.x-1, rect.y-1, rect.w+2, rect.h+2 };
                SDL_RenderRect(r, &sel);
            }

            if (C && font) {
                drawTextCentered(r, font, fmt_ms(C->chunk_ms_last),
                                 rect.x + rect.w*0.5f, rect.y + rect.h*0.5f);
            }
        }
    }

    double avg_ms_per_chunk = (chunks_with_work ? (total_ms_all_chunks / (double)chunks_with_work) : 0.0);

    drawColorGradientHeader(r, winW, 0.0f, 6000.0f, v.st);
    if (font) {
        char info[256];
        std::snprintf(info,sizeof(info),
            "[WORLD] chunks=%zu  sel=(%d,%d)  frame=%d  paused=%d  | per-frame: avg/chunk=%.3f ms  total=%.3f ms  (WASD/arrows, Enter=open, Space=pause)",
            world.chunks.size(), v.sel_cx, v.sel_cz, v.frame, paused?1:0, avg_ms_per_chunk, total_ms_all_chunks);
        drawText(r, font, info, 10.0f, 36.0f);
    }
}

// ---------- Chunk View ----------
static inline void render_chunk_view(SDL_Renderer* r, TTF_Font* font,
                                     const World& world, bool paused, WorldView& v,
                                     int winW, int winH)
{
    const int header = v.st.headerHeight;
    const int scale  = v.st.pixelScale;
    const Chunk* C = world.findChunk(v.focus_cx, v.focus_cz);

    SDL_SetRenderDrawColor(r, 0,0,0,255);
    SDL_RenderClear(r);

    float scaleMin = 0.0f, scaleMax = 6000.0f;
    if (v.ctrl && C) {
        auto mm = slice_minmax_nonvoid(*C, v.zSlice);
        scaleMin = mm.first; scaleMax = mm.second;
        if (scaleMax - scaleMin < 1e-6f) { scaleMin = 0.f; scaleMax = 6000.f; }
    }

    if (C) {
        const uint16_t void_ix = C->void_ix;
        for (int y = 0; y < CHUNK_H; ++y) {
            for (int x = 0; x < CHUNK_W; ++x) {
                int i = idx(x,y,v.zSlice);
                if (C->matIx[i]==void_ix) continue; // void stays black
                float t = C->T_curr[i];
                SDL_Color col = temperatureToColor(t, scaleMin, scaleMax);
                SDL_SetRenderDrawColor(r, col.r, col.g, col.b, 255);
                SDL_FRect px = {
                    (float)(x * scale),
                    (float)(header + y * scale),
                    (float)scale,
                    (float)scale
                };
                SDL_RenderFillRect(r, &px);
            }
        }

        // per-frame section timing overlays
        if (font) {
            double total_ms_sections = 0.0;
            int    loaded_count = 0;
            for (int sy=0; sy<SECTIONS_Y; ++sy) {
                if (!C->sectionLoaded[sy]) continue;
                total_ms_sections += C->section_ms_last[sy];
                ++loaded_count;
            }
            double avg_ms_per_section = (loaded_count ? (total_ms_sections / (double)loaded_count) : 0.0);

            drawColorGradientHeader(r, winW, scaleMin, scaleMax, v.st);
            char head[256];
            std::snprintf(head, sizeof(head),
                "[CHUNK] (%d,%d)  z=%d  frame=%d  paused=%d  | per-frame: avg/section=%.3f ms  total sections=%.3f ms  (Up/Down slice, Esc=back, Space=pause, Shift+Click=paint all layers)",
                v.focus_cx, v.focus_cz, v.zSlice, v.frame, paused?1:0, avg_ms_per_section, total_ms_sections);
            drawText(r, font, head, 10.0f, 36.0f);

            const float cx = (CHUNK_W * scale) * 0.5f;
            for (int sy=0; sy<SECTIONS_Y; ++sy) {
                if (!C->sectionLoaded[sy] && C->section_ms_last[sy] <= 0.0) continue;
                float y_center = header + (sy*SECTION_EDGE + SECTION_EDGE*0.5f) * scale;
                drawTextCentered(r, font, fmt_ms(C->section_ms_last[sy]), cx, y_center);
            }
            return;
        }
    }

    drawColorGradientHeader(r, winW, scaleMin, scaleMax, v.st);
    if (font) {
        char line[256];
        std::snprintf(line, sizeof(line),
            "[CHUNK] at (%d,%d)  z=%d  frame=%d  paused=%d",
            v.focus_cx, v.focus_cz, v.zSlice, v.frame, paused?1:0);
        drawText(r, font, line, 10.0f, 36.0f);
    }
}

// ---------- Input helpers ----------
static inline void move_selection(WorldView& v, int dx, int dz) {
    v.sel_cx += dx; v.sel_cz += dz;
}

// ---------- Threaded UI runner (no stepping here) ----------
static inline int run_world_ui(SimServer& server) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) { std::fprintf(stderr,"SDL_Init failed: %s\n", SDL_GetError()); return 1; }
    if (TTF_Init() == -1) { std::fprintf(stderr,"TTF_Init failed: %s\n", SDL_GetError()); SDL_Quit(); return 1; }

    TTF_Font* font = loadTinyFont(18);

    // Snapshot world under lock to initialize view
    {
        std::unique_lock<std::mutex> lk(server.worldMutex);
        if (server.world.materials.table.empty()) {
            server.world.materials.add(Material{0, 0, 0, 0});                    // 0 = void
            server.world.materials.add(Material{500.0f, 100.0f, 1000.0f, 0.05f}); // 1 = generic solid
        }
        init_view_from_world(*(new WorldView), server.world); // dummy to pre-touch; real init below
        recomputeAllSectionLoaded(server.world);
    }

    WorldView view{};
    {
        std::unique_lock<std::mutex> lk(server.worldMutex);
        init_view_from_world(view, server.world);
    }

    const uint16_t SOLID_IX = 1;

    int winW = 1280, winH = 800;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (SDL_CreateWindowAndRenderer("Thermal World (threaded)", winW, winH, SDL_WINDOW_RESIZABLE, &window, &renderer) < 0) {
        std::fprintf(stderr, "SDL_CreateWindowAndRenderer failed: %s\n", SDL_GetError());
        if (font) TTF_CloseFont(font);
        TTF_Quit(); SDL_Quit(); return 1;
    }

    bool running = true;
    bool left=false, middle=false, right=false;
    int mouseX=0, mouseY=0;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = false;
            else if (e.type == SDL_EVENT_WINDOW_RESIZED) {
                SDL_GetRenderOutputSize(renderer, &winW, &winH);
            }
            else if (e.type == SDL_EVENT_KEY_DOWN) {
                if (e.key.scancode == SDL_SCANCODE_LCTRL || e.key.scancode == SDL_SCANCODE_RCTRL) view.ctrl = true;
                if (e.key.scancode == SDL_SCANCODE_LSHIFT || e.key.scancode == SDL_SCANCODE_RSHIFT) view.shift = true;

                if (e.key.key == SDLK_Q) running = false;
                else if (e.key.key == SDLK_SPACE) server.setPaused(!server.isPaused());

                if (view.mode == RenderMode::WorldMap) {
                    if (e.key.key == SDLK_W || e.key.key == SDLK_UP)    move_selection(view, 0, -1);
                    if (e.key.key == SDLK_S || e.key.key == SDLK_DOWN)  move_selection(view, 0, +1);
                    if (e.key.key == SDLK_A || e.key.key == SDLK_LEFT)  move_selection(view, -1, 0);
                    if (e.key.key == SDLK_D || e.key.key == SDLK_RIGHT) move_selection(view, +1, 0);
                    if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) {
                        view.mode = RenderMode::ChunkView;
                        view.focus_cx = view.sel_cx;
                        view.focus_cz = view.sel_cz;
                    }
                } else { // ChunkView
                    if (e.key.key == SDLK_ESCAPE) {
                        view.mode = RenderMode::WorldMap;
                        view.sel_cx = view.focus_cx;
                        view.sel_cz = view.focus_cz;
                    } else if (e.key.key == SDLK_W || e.key.key == SDLK_UP) {
                        if (view.zSlice < CHUNK_D-1) view.zSlice++;
                    } else if (e.key.key == SDLK_S || e.key.key == SDLK_DOWN) {
                        if (view.zSlice > 0) view.zSlice--;
                    }
                }
            }
            else if (e.type == SDL_EVENT_KEY_UP) {
                if (e.key.scancode == SDL_SCANCODE_LCTRL || e.key.scancode == SDL_SCANCODE_RCTRL) view.ctrl = false;
                if (e.key.scancode == SDL_SCANCODE_LSHIFT || e.key.scancode == SDL_SCANCODE_RSHIFT) view.shift = false;
            }
            else if (e.type == SDL_EVENT_MOUSE_MOTION) {
                mouseX = e.motion.x; mouseY = e.motion.y;
            }
            else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                left   = (e.button.button == SDL_BUTTON_LEFT);
                right  = (e.button.button == SDL_BUTTON_RIGHT);
                middle = (e.button.button == SDL_BUTTON_MIDDLE);
            }
            else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                if (e.button.button == SDL_BUTTON_LEFT) left=false;
                if (e.button.button == SDL_BUTTON_RIGHT) right=false;
                if (e.button.button == SDL_BUTTON_MIDDLE) middle=false;
            }
        }

        // Sync view frame/paused flags from server
        view.frame = (int)server.framesSimulated.load();
        const bool pausedNow = server.isPaused();

        // Editing only in Chunk View AND when paused
        if (pausedNow && view.mode == RenderMode::ChunkView && (left || middle || right)) {
            std::unique_lock<std::mutex> lk(server.worldMutex);
            Chunk* C = server.world.findChunk(view.focus_cx, view.focus_cz);
            if (C) {
                int localX = mouseX / view.st.pixelScale;
                int localY = (std::max(0, mouseY - view.st.headerHeight)) / view.st.pixelScale;

                auto paint = [&](float Tval, bool allLayers){
                    if (localX<0 || localX>=CHUNK_W || localY<0 || localY>=CHUNK_H) return;
                    const int sy = localY / SECTION_EDGE;

                    auto set_voxel = [&](int x, int y, int z){
                        const int i = idx(x,y,z);
                        C->T_curr[i] = Tval; C->T_next[i] = Tval;
                        C->matIx[i] = SOLID_IX; // mark as solid => section loaded
                        C->mass_kg[i]= server.world.materials.byIx(SOLID_IX).defaultMass;
                    };

                    if (!allLayers) {
                        set_voxel(localX, localY, view.zSlice);
                    } else {
                        for (int z=0; z<CHUNK_D; ++z) set_voxel(localX, localY, z);
                    }
                    markSectionLoaded(*C, sy, true);
                };

                const bool allLayers = view.shift;
                if (left)   paint(0.0f,   allLayers);
                if (middle) paint(300.0f, allLayers);
                if (right)  paint(6000.0f,allLayers);
            }
        }

        // Render with try-lock so we don't stall sim thread; if busy, draw a tiny “updating” banner.
        bool drew = false;
        if (std::unique_lock<std::mutex> lk(server.worldMutex, std::try_to_lock); lk.owns_lock()) {
            if (view.mode == RenderMode::WorldMap) {
                render_world_map(renderer, font, server.world, pausedNow, view, winW, winH);
            } else {
                render_chunk_view(renderer, font, server.world, pausedNow, view, winW, winH);
            }
            drew = true;
        } else {
            // lightweight fallback frame
            SDL_SetRenderDrawColor(renderer, 0,0,0,255);
            SDL_RenderClear(renderer);
            if (font) drawText(renderer, font, "Updating simulation...", 10.0f, 10.0f);
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(16); // ~60 fps pacing
    }

    if (font) TTF_CloseFont(font);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    return 0;
}