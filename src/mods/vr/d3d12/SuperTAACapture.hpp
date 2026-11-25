// SuperTAACapture.hpp -- [SUPER TAA 16.11.2026] debug: consecutive frames of the mod's supersampling TAA (d3d12::SuperTAA)
// written exactly as stored, to replay and measure them outside the game (tests/supertaa/analyze_capture.py).
//
// Of one fovea view, a square part in the middle of the view (where the eye looks):
//   every frame        color, motion, depth      the part of the view's inputs (render pixels)
//                      out                       the part of the fovea image the supersampling TAA made
//                      old                       the same part of the image the mod's TAA (d3d12::FoveaTAA) made of the
//                                                same frames (it runs beside it during a capture)
//   first frame        history_before, stats_before, dist_before      the pass's state before it
//   first, last frame  linear, guide, box_centre, box_extent          what its passes handed each other
//                      history_after, stats_after, dist_after         its state after it
// and capture.txt: sizes, the parts' corners, formats, the settings, per frame the jitter and every constant the passes
// got, first numbers for detail and flicker of both anti-aliasings (see Series in the .cpp), and the list of the files.
//
// [CAPTURE fix81 17.11.2026] The rest of the frame's chain goes into the same folders: any pass may add a part of an image
// it reads or writes (extra) and a line of numbers (note) between one tick() and the next; chain.txt lists them (read by
// tests/supertaa/analyze_chain.py). The frames are counted by what was added to them, so a capture also works without the
// supersampling TAA's own pass (the debug view with the periphery alone: the captures of 17.11.2026 made there were empty).
//
// Use (TemporalUpscaler; the GPU test runs the same code):
//   start(frames, part)                      the mod's TAA runs beside the pass from now on (wants_compare)
//   tick() once per frame, before the passes after WARM_FRAMES frames the recording starts (the mod's TAA has settled);
//                                            the tick after the last frame with images sets write_pending()
//   record(...) right after the pass         the pass's images of this frame
//   extra(...) / note(...)                   other images and numbers of this frame
//   write(dir) after that frame's GPU work   the files; returns the text for the log
#pragma once
#include "utility/ReleaseBuild.hpp" // [RELEASE SWITCH fix141] REVR_RELEASE, REVR_LOG

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include <d3d12.h>

#include "ComPtr.hpp"
#include "SuperTAA.hpp"
#include "TextureDump.hpp"

namespace d3d12 {

#if REVR_RELEASE
// [RELEASE SWITCH fix141 23.11.2026] the release has no captures: an empty stand-in (never recording), so the capture's
// branches in the render code are left out by the compiler
struct SuperTAACapture {
    static constexpr int MAX_FRAMES = 64;
    static constexpr int WARM_FRAMES = 32;
    static constexpr int IDLE_FRAMES = 90;
    using DepthPart = std::function<bool(ID3D12Resource* depth, ID3D12Resource* target, uint32_t x, uint32_t y, uint32_t w, uint32_t h)>;
    void start(int, uint32_t) {}
    bool tick() { return false; }
    void cancel() {}
    bool busy() const { return false; }
    bool wants_compare() const { return false; }
    bool recording() const { return false; }
    bool write_pending() const { return false; }
    int frames_recorded() const { return 0; }
    int frame() const { return -1; }
    uint32_t part() const { return 0; }
    void record(ID3D12Device*, ID3D12GraphicsCommandList*, const SuperTAA&, uint32_t, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*,
        D3D12_RESOURCE_STATES, ID3D12Resource*, ID3D12Resource*, const SuperTAA::Params&, const std::string&, const std::string&, const DepthPart&) {}
    void extra(ID3D12Device*, ID3D12GraphicsCommandList*, ID3D12Resource*, D3D12_RESOURCE_STATES, const std::string&, uint32_t, uint32_t, uint32_t,
        uint32_t, const DepthPart& = {}) {}
    void note(const std::string&) {}
    std::string write(const std::filesystem::path&) { return {}; }
};
#else
struct SuperTAACapture {
    static constexpr int MAX_FRAMES = 64;
    static constexpr int WARM_FRAMES = 32;
    static constexpr int IDLE_FRAMES = 90; // frames without a pass while recording: ended with what there is

    // fills `target` (R32_FLOAT, w x h, resting in UNORDERED_ACCESS) with the depth values of the part x, y, w x h of the
    // depth(-stencil) texture `depth` (resting in the inputs' state); false = could not (the whole depth is read then)
    using DepthPart = std::function<bool(ID3D12Resource* depth, ID3D12Resource* target, uint32_t x, uint32_t y, uint32_t w, uint32_t h)>;

    // frames: 1..MAX_FRAMES; part: side of the square in render pixels (64..512, even)
    void start(int frames, uint32_t part);
    // once per frame before the passes; false = nothing under way
    bool tick();
    // everything dropped (the images it read are going away)
    void cancel();

    bool busy() const { return m_next >= 0 || m_warm > 0 || m_write; }
    // the mod's TAA has to run beside the pass (warming up or recording)
    bool wants_compare() const { return m_next >= 0 || m_warm > 0; }
    bool recording() const { return m_next >= 0 && !m_write; }
    bool write_pending() const { return m_write; }
    int frames_recorded() const { return m_done; }
    // [CAPTURE fix81] the frame being recorded (-1: none) and the side of the part asked for
    int frame() const { return m_write ? -1 : m_next; }
    uint32_t part() const { return m_part; }

    // One frame, right after the pass of `eye` (the images of one frame are shared by both eyes: the other eye's pass
    // writes them next). color / motion / depth: the pass's inputs resting in in_state (depth may be null); out: the
    // fovea image (UNORDERED_ACCESS); cmp: the mod's TAA's image of this frame or null. `p` must have had keep_constants
    // set. settings_text: lines for the header (the first frame's is used); frame_text: appended to this frame's line.
    void record(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, const SuperTAA& taa, uint32_t eye, ID3D12Resource* color, ID3D12Resource* motion,
        ID3D12Resource* depth, D3D12_RESOURCE_STATES in_state, ID3D12Resource* out, ID3D12Resource* cmp, const SuperTAA::Params& p,
        const std::string& settings_text, const std::string& frame_text, const DepthPart& depth_part);

    // [CAPTURE fix81 17.11.2026] While recording(): the part x, y, w x h (cut to the image) of `res` (resting in `state`) into
    // this frame's folder under `name`, as stored. A depth(-stencil) texture goes through depth_part (as record()'s depth;
    // one such image per frame).
    void extra(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* res, D3D12_RESOURCE_STATES state, const std::string& name, uint32_t x,
        uint32_t y, uint32_t w, uint32_t h, const DepthPart& depth_part = {});
    // ... a line for chain.txt ("frame N <text>")
    void note(const std::string& text);

    // After the GPU finished the list of the last recorded frame: the files into `dir` (made). Returns the first measure
    // of both anti-aliasings as text (for the log).
    std::string write(const std::filesystem::path& dir);

private:
    std::array<TextureDump, MAX_FRAMES> m_dumps{};
    int m_frames{16};
    uint32_t m_part{256};
    int m_warm{0};          // frames left before the recording starts
    int m_next{-1};         // frame being recorded (0..frames-1); -1 = none
    int m_done{0};          // frames recorded
    bool m_write{false};    // all frames recorded: write() after this frame's GPU work
    int m_idle{0};          // frames in a row without record() while recording
    bool m_recorded{false}; // this frame had one
    std::string m_log{};    // capture.txt: the header and a block per frame
    std::string m_xlog{};   // [CAPTURE fix81] chain.txt: the images and notes added with extra() / note()
    bool m_header{false};   // record() wrote capture.txt's header
    ComPtr<ID3D12Resource> m_depth_part{}; // R32_FLOAT, the part's size
    ComPtr<ID3D12Resource> m_extra_depth{}; // [CAPTURE fix81] the same for extra()'s depth
    // the parts (set with the first frame)
    uint32_t m_in_x{0}, m_in_y{0}, m_in_w{0}, m_in_h{0};     // render pixels
    uint32_t m_out_x{0}, m_out_y{0}, m_out_w{0}, m_out_h{0}; // fovea image pixels
    uint32_t m_scale{1};                                     // history texels per fovea image pixel
    uint32_t m_view_w{0}, m_view_h{0}, m_image_w{0}, m_image_h{0};
    float m_mv_scale[2]{0.5f, -0.5f};                        // motion vector -> UV offset (for the measure's motion)
};
#endif

} // namespace d3d12
