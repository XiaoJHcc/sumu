// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
//
// 共享 D3D11 device 创建助手（Player 窗口路径与 HeadlessDecode 无窗口路径共用）。
// 多 GPU 机器（Intel 核显 + NVIDIA 独显 —— 正是目标机型）上
// D3D11CreateDevice(默认适配器) 可能选中核显：CUDA 互操作随后必炸
// （cuD3D11GetDevice 报 NO_DEVICE / cuGraphicsD3D11RegisterResource 失败），整个
// 播放器无法启动。实测触发：驱动更新后系统把默认适配器切到核显。这里优先用
// IDXGIFactory6::EnumAdapterByGpuPreference(HIGH_PERFORMANCE) 显式选高性能适配器；
// 枚举或创建失败（旧系统无 IDXGIFactory6、单 GPU 以外的怪异拓扑）回退默认行为。
// 单 GPU 机器上 HIGH_PERFORMANCE 枚举到的就是唯一 GPU，行为不变。
#pragma once

#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>

// 语义与 D3D11CreateDevice 相同（ppDevice/ppGotFeatureLevel/ppContext 原样透传），
// 仅适配器选择策略不同。返回最终那次 D3D11CreateDevice 的 HRESULT。
inline HRESULT create_d3d11_device_high_perf(
    const D3D_FEATURE_LEVEL* levels, UINT num_levels,
    ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* ppGotFeatureLevel,
    ID3D11DeviceContext** ppContext)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory6> factory6;
    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory6)))) {
        ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(factory6->EnumAdapterByGpuPreference(0,
                DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)))) {
            // 显式给了适配器就必须用 D3D_DRIVER_TYPE_UNKNOWN（SDK 约定）。
            HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                levels, num_levels, D3D11_SDK_VERSION, ppDevice, ppGotFeatureLevel, ppContext);
            if (SUCCEEDED(hr)) {
                DXGI_ADAPTER_DESC desc{};
                adapter->GetDesc(&desc);
                fprintf(stderr, "[sumu] D3D11 device on high-performance adapter: %ls\n",
                    desc.Description);
                return hr;
            }
            fprintf(stderr, "[sumu] D3D11CreateDevice on high-perf adapter failed "
                "(hr=0x%08lx), falling back to default adapter\n", hr);
        }
    }
    // 回退：默认适配器（旧系统无 IDXGIFactory6，或高性能适配器上创建失败）。
    return D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, num_levels, D3D11_SDK_VERSION, ppDevice, ppGotFeatureLevel, ppContext);
}
