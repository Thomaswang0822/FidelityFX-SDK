// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

/// DEBUG Edit:
/// First, define the BIND Macro of all SRV we want to look at (see ffx_frameinterpolation.cpp) here.
/// Next, if not already there, define the Sample function in ffx_frameinterpolation_callbacks_hlsl.h 
///     and keep an eye on return type (may need unpack/postprocess, e.g. for int type).
/// Last, define new draw function in ffx_frameinterpolation_debug_view.h and used in computeDebugView().

#define FFX_FRAMEINTERPOLATION_BIND_SRV_PREVIOUS_INTERPOLATION_SOURCE           0
#define FFX_FRAMEINTERPOLATION_BIND_SRV_CURRENT_INTERPOLATION_SOURCE            1
#define FFX_FRAMEINTERPOLATION_BIND_SRV_INPUT_DEPTH                             2
#define FFX_FRAMEINTERPOLATION_BIND_SRV_INPUT_MOTION_VECTORS                    3

#define FFX_FRAMEINTERPOLATION_BIND_SRV_DILATED_MOTION_VECTORS                  4
#define FFX_FRAMEINTERPOLATION_BIND_SRV_DILATED_DEPTH                           5
#define FFX_FRAMEINTERPOLATION_BIND_SRV_RECONSTRUCTED_DEPTH_PREVIOUS_FRAME      6
#define FFX_FRAMEINTERPOLATION_BIND_SRV_RECONSTRUCTED_DEPTH_INTERPOLATED_FRAME  7
#define FFX_FRAMEINTERPOLATION_BIND_SRV_DISOCCLUSION_MASK                       8

#define FFX_FRAMEINTERPOLATION_BIND_SRV_GAME_MOTION_VECTOR_FIELD_X              9
#define FFX_FRAMEINTERPOLATION_BIND_SRV_GAME_MOTION_VECTOR_FIELD_Y              10
#define FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW_MOTION_VECTOR_FIELD_X      11
#define FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW_MOTION_VECTOR_FIELD_Y      12

#define FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW                            13
#define FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW_CONFIDENCE                 14
#define FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW_GLOBAL_MOTION              15
#define FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW_SCENE_CHANGE_DETECTION     16

#define FFX_FRAMEINTERPOLATION_BIND_SRV_PRESENT_BACKBUFFER                      17
#define FFX_FRAMEINTERPOLATION_BIND_SRV_INPAINTING_PYRAMID                      18
#define FFX_FRAMEINTERPOLATION_BIND_SRV_DISTORTION_FIELD                        19

#define FFX_FRAMEINTERPOLATION_BIND_UAV_OUTPUT                                  0

#define FFX_FRAMEINTERPOLATION_BIND_CB_FRAMEINTERPOLATION                       0

#include "frameinterpolation/ffx_frameinterpolation_callbacks_hlsl.h"
#include "frameinterpolation/ffx_frameinterpolation_common.h"
#include "frameinterpolation/ffx_frameinterpolation_debug_view.h"

#ifndef FFX_FRAMEINTERPOLATION_THREAD_GROUP_WIDTH
#define FFX_FRAMEINTERPOLATION_THREAD_GROUP_WIDTH 8
#endif // #ifndef FFX_FRAMEINTERPOLATION_THREAD_GROUP_WIDTH
#ifndef FFX_FRAMEINTERPOLATION_THREAD_GROUP_HEIGHT
#define FFX_FRAMEINTERPOLATION_THREAD_GROUP_HEIGHT 8
#endif // #ifndef FFX_FRAMEINTERPOLATION_THREAD_GROUP_HEIGHT
#ifndef FFX_FRAMEINTERPOLATION_THREAD_GROUP_DEPTH
#define FFX_FRAMEINTERPOLATION_THREAD_GROUP_DEPTH 1
#endif // #ifndef FFX_FRAMEINTERPOLATION_THREAD_GROUP_DEPTH
#ifndef FFX_FRAMEINTERPOLATION_NUM_THREADS
#define FFX_FRAMEINTERPOLATION_NUM_THREADS [numthreads(FFX_FRAMEINTERPOLATION_THREAD_GROUP_WIDTH, FFX_FRAMEINTERPOLATION_THREAD_GROUP_HEIGHT, FFX_FRAMEINTERPOLATION_THREAD_GROUP_DEPTH)]
#endif // #ifndef FFX_FRAMEINTERPOLATION_NUM_THREADS

FFX_FRAMEINTERPOLATION_NUM_THREADS
void CS(FfxInt32x2 iPxPos : SV_DispatchThreadID)
{
    computeDebugView(iPxPos);
}
