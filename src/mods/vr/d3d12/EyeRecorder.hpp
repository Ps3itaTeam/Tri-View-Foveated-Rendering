// EyeRecorder.hpp -- debug: a few seconds of the two eye images that go to the headset, every frame, as an MP4 video.
//
// [VIDEO 25.10.2026] The frame dump waits for the GPU and writes its BMP files in every dumped frame, so the game runs
// at a few frames per second while it is taken and fast movements look different from the real game. This records at
// the full frame rate instead: each frame both eye images are scaled down and converted to NV12 on the GPU (side by
// side, left eye on the left) and copied into a small ring of readback buffers; the CPU only collects the finished
// copies, it never waits. When the time is up a worker thread encodes the collected frames with Media Foundation
// (H.264, the hardware encoder when there is one) into an MP4 file. Media Foundation is loaded at run time, so the
// mod still loads on Windows editions without it (the video then cannot be written, the status says so).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <d3d12.h>

#include "ComPtr.hpp"
#include "CommandContext.hpp"

namespace d3d12 {

class EyeRecorder {
public:
    EyeRecorder() = default;
    EyeRecorder(const EyeRecorder&) = delete;
    EyeRecorder& operator=(const EyeRecorder&) = delete;
    ~EyeRecorder();

    // Starts at the next on_frame and records `seconds`; the video is written into `dir`. false = still busy.
    bool start(double seconds, const std::filesystem::path& dir);

    // [LIGHT FLICKER TEST 15.11.2026] A measurement instead of a video, for `seconds`: each frame's luma (both eyes, 2 x 2
    // averaged: 848 x 432) against the two frames before it. A pixel that goes up by FLICKER_STEP or more and right back
    // down by as much (or down and back up) counts as a flip; camera motion moves values one way. Each frame carries the
    // tag set before it (the caller's phase; -1 = not measured); a frame and the two before it must have the same tag.
    // Nothing is kept of the frames themselves. false = still busy.
    bool start_measure(double seconds);
    void set_measure_tag(int tag) { m_measure_tag.store(tag, std::memory_order_relaxed); }

    // [LIGHT FLICKER TEST 15.11.2026 evening] the fovea rectangle of each eye in the measured image (848 x 432, left eye left;
    // with a margin): flips there count in the totals only, the rest also as the periphery's. Set before on_frame (render
    // thread); each frame keeps the one set before it.
    struct MeasureMask {
        int32_t x0[2]{0, 0};
        int32_t y0[2]{0, 0};
        int32_t x1[2]{0, 0};
        int32_t y1[2]{0, 0};
        bool valid[2]{false, false};
    };

    void set_measure_mask(const MeasureMask& mask) { m_measure_mask = mask; }

    struct MeasureRow {
        int tag{-1};
        uint64_t frames{0};
        uint64_t flips{0};
        uint32_t cell_x{0}; // the cell of 32 x 32 (of the 848 x 432 image) with the most flips
        uint32_t cell_y{0};
        uint64_t cell_flips{0};
        uint64_t peri_flips{0};      // [LIGHT FLICKER TEST 15.11.2026 evening] outside the fovea rectangles
        uint32_t peri_cell_x{0};     //   the cell with the most of those
        uint32_t peri_cell_y{0};
        uint64_t peri_cell_flips{0};
        // [LIGHTS fix86 18.11.2026] the light's course over the tag's frames at one cell, the same cell for every tag (the
        // periphery's cell with the most flips per frame in any tag): its mean luma in each frame measured (0 .. 255), and
        // whether a fovea rectangle stood elsewhere than in the frame before (bit 0 the left eye's, bit 1 the right eye's)
        uint32_t series_cell_x{0};
        uint32_t series_cell_y{0};
        std::vector<uint8_t> series{};
        std::vector<uint8_t> moved{};
    };

    // true once, when a measurement has finished (rows: one per tag measured, in tag order)
    bool take_measure(std::vector<MeasureRow>& rows);

    static constexpr int FLICKER_STEP = 3; // luma (video range, 219 steps for full scale)

    // Once per frame, after both eye images are complete (right before the submit). left / right rest in `state`
    // and are returned in it; nothing happens while the recorder is idle.
    void on_frame(ID3D12Device* device, ID3D12Resource* left, ID3D12Resource* right, D3D12_RESOURCE_STATES state);

    bool busy() const { return m_phase.load() != Phase::Idle; }
    std::string status() const;

    // device reset: a recording in progress is dropped, the GPU objects are released (an encode keeps running)
    void reset();

    static constexpr uint32_t WIDTH = 1696;  // both eyes side by side (848 each), multiples of 16 for the encoder
    static constexpr uint32_t HEIGHT = 864;

private:
    enum class Phase { Idle, Recording, Draining, Encoding };

    struct Frame {
        std::vector<uint8_t> nv12{};
        int64_t time_100ns{0};
    };

    struct Slot {
        CommandContext commands{};
        ComPtr<ID3D12Resource> nv12{};     // UAV buffer the shader writes (rests in UNORDERED_ACCESS)
        ComPtr<ID3D12Resource> readback{}; // CPU-readable copy of it
        bool pending{false};
        int64_t time_100ns{0};
        int tag{-1};                       // [LIGHT FLICKER TEST] the measurement's tag when the frame was taken
        MeasureMask mask{};                // [LIGHT FLICKER TEST 15.11.2026 evening] the fovea rectangles then
    };

    static constexpr uint32_t RING = 4;
    static constexpr uint32_t MAX_FRAMES = 4 * 150; // 4 s at up to 150 fps
    static constexpr uint64_t NV12_BYTES = (uint64_t)WIDTH * HEIGHT * 3 / 2;

    bool setup(ID3D12Device* device);
    void collect(bool wait);
    void set_status(const std::string& s);
    void encode(std::vector<Frame> frames, std::filesystem::path path, double seconds_recorded, uint32_t dropped);

    std::atomic<Phase> m_phase{Phase::Idle};
    double m_seconds{4.0};
    std::filesystem::path m_dir{};
    std::chrono::steady_clock::time_point m_t0{};
    bool m_started{false};
    uint32_t m_next_slot{0};
    uint32_t m_dropped{0};
    std::vector<Frame> m_frames{};

    ComPtr<ID3D12RootSignature> m_root_sig{};
    ComPtr<ID3D12PipelineState> m_pso{};
    ComPtr<ID3D12DescriptorHeap> m_heap{};
    uint32_t m_descriptor_size{0};
    Slot m_slots[RING]{};
    bool m_setup_failed{false};

    std::thread m_worker{};
    mutable std::mutex m_status_mutex{};
    std::string m_status{};

    // [LIGHT FLICKER TEST 15.11.2026] the measurement (render thread; the result handed over under m_status_mutex)
    static constexpr uint32_t MEASURE_W = WIDTH / 2;
    static constexpr uint32_t MEASURE_H = HEIGHT / 2;
    static constexpr uint32_t CELL = 32;
    static constexpr uint32_t CELLS_X = (MEASURE_W + CELL - 1) / CELL;
    static constexpr uint32_t CELLS_Y = (MEASURE_H + CELL - 1) / CELL;
    static constexpr int MAX_TAGS = 16;
    void measure_frame(const uint8_t* nv12, int tag, const MeasureMask& mask);
    void finish_measure();
    bool m_measure{false};
    std::atomic<int> m_measure_tag{-1};
    MeasureMask m_measure_mask{};
    std::vector<uint8_t> m_luma[3]{}; // this frame and the two before it (rotating)
    int m_luma_tag[3]{-1, -1, -1};
    uint32_t m_luma_count{0};         // frames taken so far (the rotation)
    struct TagSums {
        uint64_t frames{0};
        uint64_t flips{0};
        std::vector<uint32_t> cells{};
        uint64_t peri_flips{0};           // [LIGHT FLICKER TEST 15.11.2026 evening]
        std::vector<uint32_t> peri_cells{};
        std::vector<uint8_t> cell_means{}; // [LIGHTS fix86] per frame measured: every cell's mean luma (CELLS_X * CELLS_Y each)
        std::vector<uint8_t> moved{};      //   per frame measured: the fovea rectangles moved (bits as MeasureRow::moved)
    };
    static constexpr size_t SERIES_FRAMES = 600; // per tag at most
    MeasureMask m_prev_mask{};                   // [LIGHTS fix86] the rectangles of the frame before
    bool m_have_prev_mask{false};
    TagSums m_tags[MAX_TAGS]{};
    std::vector<MeasureRow> m_measure_rows{};
    bool m_measure_ready{false};
};

} // namespace d3d12
