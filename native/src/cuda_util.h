// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
//
// 共享 CUDA driver-API 小工具（Player 的 AI 桥与 HeadlessDecode 共用）。
#pragma once

#include <cuda.h>

#include <cstdio>

// H3 修复：cuGraphicsMapResources 之后、显式 Unmap 之前的任何 throw（GetMappedArray /
// cuMemcpy2D 失败）会让资源留在 mapped 状态 —— 三处调用点的 catch 都只 rethrow 不
// Unmap，后续 Register/Unregister/再 Map 行为未定义。RAII guard：Map 成功后立即挂上，
// 析构 best-effort Unmap。Unmap 自身也可能失败，析构绝不抛出，失败只留诊断日志；
// 正常路径显式 check_cu(Unmap) 之后必须 dismiss()，否则析构会重复 Unmap。
class CuGraphicsUnmapGuard
{
public:
    CuGraphicsUnmapGuard(CUgraphicsResource* res, UINT count) : res_(res), count_(count) {}
    ~CuGraphicsUnmapGuard()
    {
        if (count_ == 0) return;
        CUresult r = cuGraphicsUnmapResources(count_, res_, 0);
        if (r != CUDA_SUCCESS)
            fprintf(stderr, "[sumu] CuGraphicsUnmapGuard: best-effort Unmap on exception path "
                "failed (CUresult=%d)\n", static_cast<int>(r));
    }
    CuGraphicsUnmapGuard(const CuGraphicsUnmapGuard&) = delete;
    CuGraphicsUnmapGuard& operator=(const CuGraphicsUnmapGuard&) = delete;
    void dismiss() { count_ = 0; } // 正常路径已显式 Unmap
private:
    CUgraphicsResource* res_;
    UINT count_;
};
