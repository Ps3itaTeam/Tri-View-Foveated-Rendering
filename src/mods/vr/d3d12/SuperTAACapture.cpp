// SuperTAACapture.cpp -- [SUPER TAA 16.11.2026] see SuperTAACapture.hpp
#include "utility/ReleaseBuild.hpp" // [RELEASE SWITCH fix141] REVR_RELEASE, REVR_LOG
#if !REVR_RELEASE // [RELEASE SWITCH fix141] the supersampling TAA's captures: debug build only (the header has the release's empty stand-in)
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <vector>

#include "SuperTAACapture.hpp"

namespace d3d12 {
namespace {
constexpr auto UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

// an R32_FLOAT image a compute shader writes, resting in UNORDERED_ACCESS (kept while the size stays)
bool ensure_float_image(ID3D12Device* device, uint32_t w, uint32_t h, ComPtr<ID3D12Resource>& image) {
    if (image != nullptr) {
        const auto have = image->GetDesc();

        if (have.Width == w && have.Height == h) {
            return true;
        }

        image.Reset();
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, UAV, nullptr, IID_PPV_ARGS(&image)))) {
        image.Reset();
        return false;
    }

    image->SetName(L"RE4VR super TAA capture depth part");
    return true;
}

// A first measure of an anti-aliasing from the captured parts of its results (display-encoded luma 0..1):
//   detail:  mean |Y(x + 1) - Y(x)| and |Y(y + 1) - Y(y)|: fine structure (and noise)
//   flicker: the frame before and the frame after brought to the frame's place by the part's mean motion, all three
//            averaged over blocks of 4 x 4 pixels, mean |B(t) - (B(t - 1) + B(t + 1)) / 2|: brightness that goes back
//            and forth from frame to frame (a thin bright line or a glint that comes and goes with the jitter). The
//            movement of the head is taken out, and fine detail that is simply there hardly changes a block's average.
// Checked against the simulation's true flicker (the error against the exact image, tests/supertaa): it ranks two anti-aliasings right on thin lines, fences, glints and edges, still and moving; on fine
// regular texture the one that keeps more of it scores up to 1.5 times higher although its error flickers less (what
// it kept moves inside the blocks), so the detail number belongs beside it.
struct Series {
    std::vector<float> y[3]{}; // luma: [0] this frame, [1] the last, [2] the one before
    float mv[3][2]{};          // each frame's mean motion in pixels of this image (previous position = pixel + motion)
    uint32_t w{0}, h{0};
    int have{0};
    double detail{0.0}, flicker{0.0};
    int n_detail{0}, n_flicker{0};

    // bilinear, clamped
    static float at(const std::vector<float>& img, uint32_t w, uint32_t h, float x, float y) {
        x = std::clamp(x, 0.0f, (float)(w - 1));
        y = std::clamp(y, 0.0f, (float)(h - 1));
        const uint32_t x0 = (uint32_t)x;
        const uint32_t y0 = (uint32_t)y;
        const uint32_t x1 = (std::min)(x0 + 1, w - 1);
        const uint32_t y1 = (std::min)(y0 + 1, h - 1);
        const float fx = x - (float)x0;
        const float fy = y - (float)y0;
        const float a = img[(size_t)y0 * w + x0] * (1.0f - fx) + img[(size_t)y0 * w + x1] * fx;
        const float b = img[(size_t)y1 * w + x0] * (1.0f - fx) + img[(size_t)y1 * w + x1] * fx;
        return a * (1.0f - fy) + b * fy;
    }

    void add(TextureDump& dump, const char* name, std::string& line, float mx, float my) {
        std::vector<float> rgb{};
        uint32_t iw = 0, ih = 0;

        if (!dump.read_rgb(name, rgb, iw, ih) || iw < 16 || ih < 16 || (have > 0 && (iw != w || ih != h))) {
            have = 0; // (a frame without this image: the differences start again)
            return;
        }

        w = iw;
        h = ih;
        std::rotate(std::begin(y), std::begin(y) + 2, std::end(y));
        std::memcpy(mv[2], mv[1], sizeof(mv[2]));
        std::memcpy(mv[1], mv[0], sizeof(mv[1]));
        mv[0][0] = mx;
        mv[0][1] = my;
        auto& cur = y[0];
        cur.resize((size_t)w * h);

        for (size_t k = 0; k < cur.size(); ++k) {
            cur[k] = 0.2126f * rgb[k * 3] + 0.7152f * rgb[k * 3 + 1] + 0.0722f * rgb[k * 3 + 2];
        }

        double d = 0.0;

        for (uint32_t py = 0; py + 1 < h; ++py) {
            for (uint32_t px = 0; px + 1 < w; ++px) {
                const size_t k = (size_t)py * w + px;
                d += 0.5 * (std::abs(cur[k + 1] - cur[k]) + std::abs(cur[k + w] - cur[k]));
            }
        }

        d /= (double)(w - 1) * (double)(h - 1);
        detail += d;
        ++n_detail;
        line += std::format(" {} detail {:.5f}", name, d);
        ++have;

        // the frame in the middle of three (counted when the third one arrives): y[1]. What it shows at p was at
        // p + mv[1] in the frame before (y[2]) and is at p - mv[0] in the frame after (y[0]).
        if (have < 3 || y[1].size() != cur.size() || y[2].size() != cur.size()) {
            return;
        }

        const float reach = (std::max)((std::max)(std::abs(mv[1][0]), std::abs(mv[1][1])), (std::max)(std::abs(mv[0][0]), std::abs(mv[0][1])));
        const uint32_t margin = (((uint32_t)std::ceil((std::min)(reach, 64.0f)) + 1u) + 3u) & ~3u; // (whole blocks)

        if (w < 2 * margin + 8 || h < 2 * margin + 8) {
            return;
        }

        double f = 0.0;
        uint32_t blocks = 0;

        for (uint32_t by = margin; by + 4 <= h - margin; by += 4) {
            for (uint32_t bx = margin; bx + 4 <= w - margin; bx += 4) {
                float mid = 0.0f, before = 0.0f, after = 0.0f;

                for (uint32_t py = by; py < by + 4; ++py) {
                    for (uint32_t px = bx; px < bx + 4; ++px) {
                        mid += y[1][(size_t)py * w + px];
                        before += at(y[2], w, h, (float)px + mv[1][0], (float)py + mv[1][1]);
                        after += at(y[0], w, h, (float)px - mv[0][0], (float)py - mv[0][1]);
                    }
                }

                f += std::abs(mid - 0.5f * (before + after)) / 16.0;
                ++blocks;
            }
        }

        if (blocks != 0) {
            f /= (double)blocks;
            flicker += f;
            ++n_flicker;
            line += std::format(" flicker {:.5f}", f);
        }
    }

    static double mean(double sum, int count) { return count > 0 ? sum / (double)count : 0.0; }
};
} // namespace

void SuperTAACapture::start(int frames, uint32_t part) {
    for (auto& d : m_dumps) {
        d.clear();
    }

    m_log.clear();
    m_xlog.clear();
    m_header = false;
    m_frames = std::clamp(frames, 1, (int)MAX_FRAMES);
    m_part = std::clamp(part, 64u, 512u) & ~1u;
    m_warm = WARM_FRAMES;
    m_next = -1;
    m_done = 0;
    m_write = false;
    m_idle = 0;
    m_recorded = false;
}

bool SuperTAACapture::tick() {
    if (m_warm > 0) {
        if (--m_warm == 0) {
            m_next = 0;
            m_idle = 0;
        }
    } else if (m_next >= 0 && !m_write) {
        // [CAPTURE fix81] the frame before left images in its place (the pass's or others): the next one's turn
        if (m_recorded) {
            m_idle = 0;
            m_done = m_next + 1;

            if (++m_next >= m_frames) {
                m_next = -1;
                m_write = true;
            }
        } else if (++m_idle > IDLE_FRAMES) {
            m_log += std::format("nothing to record for {} frames: capture ended\n", IDLE_FRAMES);
            m_next = -1;
            m_write = true;
        }
    }

    m_recorded = false;
    return busy();
}

void SuperTAACapture::cancel() {
    for (auto& d : m_dumps) {
        d.clear();
    }

    m_log.clear();
    m_xlog.clear();
    m_header = false;
    m_warm = 0;
    m_next = -1;
    m_done = 0;
    m_write = false;
    m_idle = 0;
    m_recorded = false;
    m_depth_part.Reset();
    m_extra_depth.Reset();
}

void SuperTAACapture::extra(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* res, D3D12_RESOURCE_STATES state, const std::string& name,
    uint32_t x, uint32_t y, uint32_t w, uint32_t h, const DepthPart& depth_part) {
    if (m_next < 0 || m_next >= m_frames || m_write || device == nullptr || cmd == nullptr || res == nullptr || w == 0 || h == 0) {
        return;
    }

    const auto rd = res->GetDesc();

    if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || x >= rd.Width || y >= rd.Height) {
        return;
    }

    w = (uint32_t)(std::min)((uint64_t)w, rd.Width - x);
    h = (std::min)(h, rd.Height - y);
    auto& d = m_dumps[m_next];
    bool done = false;

    if ((rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0) {
        // (D3D12 copies a depth-stencil texture whole only: its values of the part through the caller's pass, as record())
        if (depth_part && ensure_float_image(device, w, h, m_extra_depth) && depth_part(res, m_extra_depth.Get(), x, y, w, h) &&
            d.add(device, cmd, m_extra_depth.Get(), UAV, name)) {
            d.add_raw(name, 0, 0, w, h);
            done = true;
        }
    } else if (d.add_region(device, cmd, res, state, name, x, y, w, h)) {
        d.add_raw(name, 0, 0, w, h);
        done = true;
    }

    if (!done) {
        m_xlog += std::format("frame {} image {} could not be read\n", m_next, name);
        return;
    }

    m_xlog += std::format("frame {} image {} format {} part {} {} {} {} of {} {}\n", m_next, name, (int)rd.Format, x, y, w, h, (uint64_t)rd.Width, rd.Height);
    m_recorded = true;
}

void SuperTAACapture::note(const std::string& text) {
    if (m_next < 0 || m_next >= m_frames || m_write) {
        return;
    }

    m_xlog += std::format("frame {} {}\n", m_next, text);
}

void SuperTAACapture::record(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, const SuperTAA& taa, uint32_t eye, ID3D12Resource* color,
    ID3D12Resource* motion, ID3D12Resource* depth, D3D12_RESOURCE_STATES in_state, ID3D12Resource* out, ID3D12Resource* cmp, const SuperTAA::Params& p,
    const std::string& settings_text, const std::string& frame_text, const DepthPart& depth_part) {
    if (m_next < 0 || m_next >= m_frames || m_write || device == nullptr || cmd == nullptr || color == nullptr || motion == nullptr || out == nullptr) {
        return;
    }

    const int i = m_next;
    const auto cd = color->GetDesc();
    const auto od = out->GetDesc();
    const uint32_t rw = (uint32_t)cd.Width;
    const uint32_t rh = cd.Height;
    const uint32_t ow = (uint32_t)od.Width;
    const uint32_t oh = od.Height;
    const uint32_t hw = taa.history_w();
    const uint32_t hh = taa.history_h();

    if (rw == 0 || rh == 0 || ow == 0 || oh == 0 || hw < ow || hh < oh) {
        return;
    }

    if (!m_header) {
        m_header = true;

        // ([CAPTURE fix81] the frames are counted by everything added to them: the pass may join later)
        if (i != 0) {
            m_log += std::format("the supersampling TAA's pass began at frame {} of the capture\n", i);
        }

        // the part: the middle of the view (where the eye looks), even corners and sizes
        m_view_w = rw;
        m_view_h = rh;
        m_image_w = ow;
        m_image_h = oh;
        m_scale = (std::max)(hw / ow, 1u);
        m_in_w = (std::min)(m_part, rw) & ~1u;
        m_in_h = (std::min)(m_part, rh) & ~1u;
        m_in_x = ((rw - m_in_w) / 2) & ~1u;
        m_in_y = ((rh - m_in_h) / 2) & ~1u;
        // the fovea image's pixels that part covers (the fovea image is the whole view at the fovea's size)
        const double fx = (double)ow / (double)rw;
        const double fy = (double)oh / (double)rh;
        m_out_x = (uint32_t)std::floor((double)m_in_x * fx);
        m_out_y = (uint32_t)std::floor((double)m_in_y * fy);
        m_out_w = (std::min)((uint32_t)std::ceil((double)(m_in_x + m_in_w) * fx), ow) - m_out_x;
        m_out_h = (std::min)((uint32_t)std::ceil((double)(m_in_y + m_in_h) * fy), oh) - m_out_y;
        m_mv_scale[0] = p.mv_scale_x;
        m_mv_scale[1] = p.mv_scale_y;

        m_log = std::format("supersampling TAA capture: {} frames, eye {}\n"
                            "view {} {} fovea_image {} {} history {} {} scale {}\n"
                            "part_in {} {} {} {}\npart_out {} {} {} {}\npart_history {} {} {} {}\n"
                            "formats: colour {} motion {} depth {} fovea_image {}\n"
                            "mv_scale {:.9g} {:.9g}\ndepth_k {:.9g} {:.9g} {:.9g} {:.9g} valid {} reverse_z {}\n"
                            "old_taa {}\n",
            m_frames, eye, rw, rh, ow, oh, hw, hh, m_scale, m_in_x, m_in_y, m_in_w, m_in_h, m_out_x, m_out_y, m_out_w, m_out_h, m_out_x * m_scale,
            m_out_y * m_scale, m_out_w * m_scale, m_out_h * m_scale, (int)cd.Format, (int)motion->GetDesc().Format,
            depth != nullptr ? (int)depth->GetDesc().Format : 0, (int)od.Format, p.mv_scale_x, p.mv_scale_y, p.depth_k[0], p.depth_k[1], p.depth_k[2],
            p.depth_k[3], p.depth_k_valid ? 1 : 0, p.reverse_z ? 1 : 0, cmp != nullptr ? 1 : 0);
        m_log += settings_text;

        if (!m_log.empty() && m_log.back() != '\n') {
            m_log += '\n';
        }

        m_log += "files of a frame (folder frameNN, every image as stored, rows packed): color, motion, depth = the part of the view's inputs; out = the "
                 "supersampling TAA's part of the fovea image; old = the mod's TAA's; first frame: history_before, stats_before, dist_before; first and "
                 "last frame: linear, guide, box_centre, box_extent, history_after, stats_after, dist_after\n";
    } else if (rw != m_view_w || rh != m_view_h || ow != m_image_w || oh != m_image_h || hw != ow * m_scale) {
        m_log += "the sizes changed: capture ended\n";
        m_next = -1;
        m_write = true;
        return;
    }

    auto& d = m_dumps[i];

    // a part of an image, as stored
    auto part = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES state, const char* name, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
        if (res == nullptr) {
            return false;
        }

        if (!d.add_region(device, cmd, res, state, name, x, y, w, h)) {
            m_log += std::format("frame {}: {} could not be read\n", i, name);
            return false;
        }

        d.add_raw(name, 0, 0, w, h);
        return true;
    };

    const uint32_t S = m_scale;
    part(color, in_state, "color", m_in_x, m_in_y, m_in_w, m_in_h);
    part(motion, in_state, "motion", m_in_x, m_in_y, m_in_w, m_in_h);

    if (depth != nullptr) {
        // D3D12 copies a depth-stencil texture whole only: its values of the part go into an R32_FLOAT image first (the
        // caller's pass); without one, or if it fails, the whole depth is read
        const bool depth_stencil = (depth->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
        bool done = false;

        if (!depth_stencil) {
            done = part(depth, in_state, "depth", m_in_x, m_in_y, m_in_w, m_in_h);
        } else if (depth_part && ensure_float_image(device, m_in_w, m_in_h, m_depth_part) &&
                   depth_part(depth, m_depth_part.Get(), m_in_x, m_in_y, m_in_w, m_in_h) && d.add(device, cmd, m_depth_part.Get(), UAV, "depth")) {
            d.add_raw("depth", 0, 0, m_in_w, m_in_h);
            done = true;
        }

        if (!done && depth_stencil && d.add(device, cmd, depth, in_state, "depth")) {
            d.add_raw("depth", m_in_x, m_in_y, m_in_w, m_in_h);
        }
    }

    part(out, UAV, "out", m_out_x, m_out_y, m_out_w, m_out_h);
    part(cmp, UAV, "old", m_out_x, m_out_y, m_out_w, m_out_h);

    if (i == 0) {
        part(taa.history_before(eye), UAV, "history_before", m_out_x * S, m_out_y * S, m_out_w * S, m_out_h * S);
        part(taa.stats_before(eye), UAV, "stats_before", m_in_x, m_in_y, m_in_w, m_in_h);
        part(taa.distances_before(eye), UAV, "dist_before", m_in_x, m_in_y, m_in_w, m_in_h);
    }

    if (i == 0 || i + 1 == m_frames) {
        part(taa.linear(), UAV, "linear", m_in_x, m_in_y, m_in_w, m_in_h);
        part(taa.guide(), UAV, "guide", m_in_x, m_in_y, m_in_w, m_in_h);
        part(taa.box_centre(), UAV, "box_centre", m_in_x, m_in_y, m_in_w, m_in_h);
        part(taa.box_extent(), UAV, "box_extent", m_in_x, m_in_y, m_in_w, m_in_h);
        part(taa.history(eye), UAV, "history_after", m_out_x * S, m_out_y * S, m_out_w * S, m_out_h * S);
        part(taa.stats(eye), UAV, "stats_after", m_in_x, m_in_y, m_in_w, m_in_h);
        part(taa.distances(eye), UAV, "dist_after", m_in_x, m_in_y, m_in_w, m_in_h);
    }

    m_log += std::format("frame {}: jitter {:.9g} {:.9g} reset_asked {} old_taa {} {}\n{}", i, p.jitter_x, p.jitter_y, p.reset ? 1 : 0, cmp != nullptr ? 1 : 0,
        frame_text, taa.last_constants());

    // ([CAPTURE fix81] the frame is closed by the next tick(): other passes may still add their images to it)
    m_recorded = true;
}

std::string SuperTAACapture::write(const std::filesystem::path& dir) {
    const int n = std::clamp(m_done, 0, (int)MAX_FRAMES);
    Series now{}, old{};
    std::string numbers{};
    std::string report{};
    double speed = 0.0;
    int speed_frames = 0;

    for (int i = 0; i < n; ++i) {
        std::string line = std::format("  frame {:2}:", i);

        // how far the picture moved in this frame: the part's mean motion (view pixels; previous position = pixel + motion)
        std::vector<float> xy{};
        uint32_t w = 0, h = 0;
        double mx = 0.0, my = 0.0;
        bool have_motion = false;

        if (m_dumps[i].read_xy("motion", xy, w, h) && w != 0 && h != 0) {
            for (size_t k = 0; k + 1 < xy.size(); k += 2) {
                mx += (double)xy[k];
                my += (double)xy[k + 1];
            }

            mx = mx / ((double)w * (double)h) * (double)m_mv_scale[0] * (double)m_view_w;
            my = my / ((double)w * (double)h) * (double)m_mv_scale[1] * (double)m_view_h;
            have_motion = std::isfinite(mx) && std::isfinite(my);

            if (!have_motion) {
                mx = my = 0.0;
            }
        }

        // (in pixels of the fovea image for the measure)
        const float ix = (float)(mx * (double)m_image_w / (double)(std::max)(m_view_w, 1u));
        const float iy = (float)(my * (double)m_image_h / (double)(std::max)(m_view_h, 1u));
        now.add(m_dumps[i], "out", line, ix, iy);
        old.add(m_dumps[i], "old", line, ix, iy);

        if (have_motion) {
            const double px = std::sqrt(mx * mx + my * my);
            speed += px;
            ++speed_frames;
            line += std::format(" | moved {:+.2f} {:+.2f} px", -mx, -my);
        }

        numbers += line + "\n";
        // (BMPs of the first and the last frame only: the files as stored are what the analysis reads)
        report += std::format("frame {}:\n", i) + m_dumps[i].write_all(dir / std::format("frame{:02}", i), 1024, i == 0 || i + 1 == n);
    }

    for (auto& d : m_dumps) {
        d.clear();
    }

    const double moved = Series::mean(speed, speed_frames);
    std::string summary = std::format("  the picture moved {:.2f} pixels per frame on average\n", moved);
    summary += std::format("  supersampling TAA: detail {:.5f}  flicker {:.5f}  ({} frames)\n", Series::mean(now.detail, now.n_detail),
        Series::mean(now.flicker, now.n_flicker), now.n_detail);

    if (old.n_detail > 0) {
        const double d0 = Series::mean(old.detail, old.n_detail);
        const double f0 = Series::mean(old.flicker, old.n_flicker);
        summary += std::format("  the mod's TAA:     detail {:.5f}  flicker {:.5f}  ({} frames)\n", d0, f0, old.n_detail);

        if (d0 > 0.0 && f0 > 0.0) {
            summary += std::format("  supersampling TAA against the mod's TAA: detail {:+.0f}%, flicker {:+.0f}%\n",
                100.0 * (Series::mean(now.detail, now.n_detail) / d0 - 1.0), 100.0 * (Series::mean(now.flicker, now.n_flicker) / f0 - 1.0));
        }
    }

    summary += "  (display-encoded luma 0..1 of the captured part. detail: mean difference between neighbouring pixels. flicker: brightness going back and "
               "forth from frame to frame, measured on averages of 4 x 4 pixels with the part's mean motion taken out. On thin lines, wire and glints "
               "a lower flicker is the steadier image; on fine regular texture the image that keeps more of it scores somewhat higher, so read it "
               "with the detail. The files are measured again outside the game, along the motion vectors of every pixel.)\n";

    std::error_code ec{};
    std::filesystem::create_directories(dir, ec);
    {
        std::ofstream f(dir / "capture.txt");
        f << m_log << "frames_recorded " << n << "\n\nfirst measure:\n" << summary << numbers << "\nfiles:\n" << report;
    }

    // [CAPTURE fix81] the other images of the frames and the passes' numbers
    if (!m_xlog.empty()) {
        std::ofstream f(dir / "chain.txt");
        f << "the frame's chain beside the supersampling TAA's pass: per frame the images added (files of the same names in frameNN, as stored) and the "
             "passes' numbers\nframes_recorded "
          << n << "\n"
          << m_xlog;
    }

    m_xlog.clear();
    m_header = false;
    m_log.clear();
    m_done = 0;
    m_write = false;
    m_next = -1;
    return summary;
}

} // namespace d3d12
#endif // !REVR_RELEASE
