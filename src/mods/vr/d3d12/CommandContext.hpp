#pragma once

#include <mutex>
#include <d3d12.h>

#include "ComPtr.hpp"

namespace d3d12 {
struct TextureContext;

struct CommandContext {
    CommandContext() = default;
    virtual ~CommandContext() { this->reset(); }

    bool setup(const wchar_t* name = L"CommandContext object");
    void reset();
    void wait(uint32_t ms);
    void copy(ID3D12Resource* src, ID3D12Resource* dst, 
        D3D12_RESOURCE_STATES src_state = D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATES dst_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    // dst_x/dst_y: linke obere Ecke IM ZIEL. Damit laesst sich der kopierte Ausschnitt
    // versetzt ablegen -- das ist unter OpenXR das Gegenstueck zu den verschobenen
    // Submit-Bounds von OpenVR (VR::get_shifted_bounds).
    void copy_region(ID3D12Resource* src, ID3D12Resource* dst, D3D12_BOX* src_box,
        D3D12_RESOURCE_STATES src_state = D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATES dst_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        UINT dst_x = 0, UINT dst_y = 0);
    void clear_rtv(ID3D12Resource* dst, D3D12_CPU_DESCRIPTOR_HANDLE rtv, const float* color, 
        D3D12_RESOURCE_STATES dst_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    void clear_rtv(TextureContext& tex, const float* color, D3D12_RESOURCE_STATES dst_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    void execute();

    ComPtr<ID3D12CommandAllocator> cmd_allocator{};
    ComPtr<ID3D12GraphicsCommandList> cmd_list{};
    ComPtr<ID3D12Fence> fence{};
    UINT64 fence_value{};
    HANDLE fence_event{};

    std::recursive_mutex mtx{};

    bool waiting_for_fence{false};
    bool has_commands{false};
    uint32_t unsubmitted_waits{0}; // [MENU HANG 14.11.2026] waits in a row that found recorded work which was never submitted
    uint64_t dropped_lists{0};

    std::wstring internal_name{L"CommandContext object"};
    std::string tag_name{"CommandContext object"}; // [SUBMIT TIMELINE 06.11.2026] its submits by name in the GPU timeline
};
}