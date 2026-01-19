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

#pragma once

#define USE_FFX_API 1

#include "render/rendermodule.h"
#include "core/framework.h"
#include "core/uimanager.h"
#include "taa/taarendermodule.h"
#include "translucency/translucencyrendermodule.h"
#include "tonemapping/tonemappingrendermodule.h"

#include <ffx_api/ffx_api.hpp>
#include <ffx_api/ffx_upscale.hpp>
#include <ffx_api/ffx_framegeneration.hpp>

#include <functional>

namespace cauldron
{
    class Texture;
    class ParameterSet;
    class ResourceView;
    class RootSignature;
    class UIRenderModule;
}

class FSRRenderModule : public cauldron::RenderModule
{
    enum UpscalerType
    {
        Upscaler_Native,
        Upscaler_FSRAPI,

    } UpscalerType;

public:
    FSRRenderModule()
        : RenderModule(L"FSRApiRenderModule"), 
          hackOptions(cauldron::GetFramework()->GetConfig()->HackOptions),
          m_SafetyMarginInMs(0.1f),
          m_VarianceFactor (0.1f),
          m_AllowHybridSpin (false),
          m_HybridSpinTime(2),
          m_AllowWaitForSingleObjectOnFence(false),
          framePacingTuning { m_SafetyMarginInMs, m_VarianceFactor, m_AllowHybridSpin, m_HybridSpinTime, m_AllowWaitForSingleObjectOnFence }
    {}
    virtual ~FSRRenderModule();

    void Init(const json& initData);
    void EnableModule(bool enabled) override;
    void OnPreFrame() override;

    virtual bool DebugCheck(std::string marker) override;

    /**
     * @brief   Setup parameters that the FSR API needs this frame and then call the FFX Dispatch.
     */
    void Execute(double deltaTime, cauldron::CommandList* pCmdList) override;
    void PreTransCallback(double deltaTime, cauldron::CommandList* pCmdList);
    void PostTransCallback(double deltaTime, cauldron::CommandList* pCmdList);
    /**
     * @brief   Copy data from those EXRTextureDataBlock to m_pHackColorTarget, m_pHackMV, and m_pHackDepth.
     * 
     * This should be done per-frame before Execute().
     * 
     */
    void HackCopyDataCallback();

    /**
     * @brief   Recreate the FSR API Context to resize internal resources. Called by the framework when the resolution changes.
     */
    void OnResize(const cauldron::ResolutionInfo& resInfo) override;

    /**
     * @brief   Init UI.
     */
    void InitUI(cauldron::UISection* uiSection);

    /**
     * @brief   Returns whether or not FSR requires sample-side re-initialization.
     */
    bool NeedsReInit() const { return m_NeedReInit; }

    /**
     * @brief   Clears FSR re-initialization flag.
     */
    void ClearReInit() { m_NeedReInit = false; }

    /**
     * @brief   Given a FfxApiResource storing FG output, which contains 
     * - void* resource: actual data.
     * - FfxApiResourceDescription description
     * - FfxResourceStates state
     *        
     * we dump out a screenshot to disk for debugging purpose.  
     * 
     * \return success?
     */
    bool ExportDebugFrame(
        const FfxApiResource& resource, 
        const size_t          skipN, 
        std::string           customName = "");
    bool ExportDebugFrame2Inputs(
        const FfxApiResource& resource1, 
        const FfxApiResource& resource2, 
        const size_t          skipN,
        std::string           customName = "");

    void UpdateExportInfo(cauldron::ExportInfo& info);

    /**
     * Except setting m_UpscaleMethod to input method, 
     * it modifies m_CurScale, m_IsNonNative, and m_ScalePreset:
     * 
     * method == 0: turn off FFXAPI; always set m_ScalePreset = NativeAA. 
     * In addition, if originally m_IsNonNative
     * **** is false (FFXAPI currently also off), nothing else happens.
     * **** is true, m_CurScale = <original> m_ScalePreset, m_IsNonNative turned to false,
     *   
     * 
     * 
     * method == 1: turn on FFXAPI; always set m_IsNonNative = true, m_ScalePreset = m_CurScale (original value)
     * In addition, if originally m_IsNonNative
     * **** is false (FFXAPI currently off), nothing else happens.
     * **** is true, (m_CurScale = m_ScalePreset) then (m_ScalePreset = m_CurScale), so m_ScalePreset never changes
     * 
     * 
     * \param method: 0 = native (FFXAPI off), 1 = FFXAPI
     */
    void SetFilter(int32_t method)
    {
        m_UpscaleMethod = method;

        if (m_IsNonNative)
            m_CurScale = m_ScalePreset;
        m_IsNonNative = (m_UpscaleMethod != Upscaler_Native);

        m_ScalePreset = m_IsNonNative ? m_CurScale : FSRScalePreset::NativeAA;
        UpdatePreset((int32_t*)&m_ScalePreset);
    }

    inline std::pair<uint32_t, uint32_t> GetDisplayResolution() {
        return cauldron::GetFramework()->GetResolutionInfo().DisplayResolution();
    }

private:

    /**
     * Unlike DLSS mode, which is deeply entangled with the entire pipeline,
     * FSRScalePreset here is merely used to determine m_UpscaleRatio.
     * (m_MipBias only uses the ratio of FSRScalePreset).
     */
    enum class FSRScalePreset : uint32_t
    {
        NativeAA = 0,       // 1.0f
        Quality,            // 1.5f
        Balanced,           // 1.7f
        Performance,        // 2.f
        UltraPerformance,   // 3.f
        Custom              // 1.f - 3.f range
    };

    static constexpr inline std::array<std::pair<FSRScalePreset, float>, 5> k_PresetRatioList = {
        std::make_pair(FSRScalePreset::NativeAA,         1.0f),
        std::make_pair(FSRScalePreset::Quality,          1.5f),
        std::make_pair(FSRScalePreset::Balanced,         1.7f),
        std::make_pair(FSRScalePreset::Performance,      2.0f),
        std::make_pair(FSRScalePreset::UltraPerformance, 3.0f),
    };

    /**
     * \return The FSRScalePreset whose ratio is closest to m_UpscaleRatio
     */
    inline FSRScalePreset FindMatchingPreset() const
    {
        auto it = std::min_element(k_PresetRatioList.begin(), k_PresetRatioList.end(), [this](const auto& lhs, const auto& rhs) {
            const auto& [preset1, ratio1] = lhs;
            const auto& [preset2, ratio2] = rhs;
            return std::abs(ratio1 - m_UpscaleRatio) < std::abs(ratio2 - m_UpscaleRatio);
        });

        return it->first;
    }

    enum class FSRMaskMode
    {
        Disabled = 0,
        Manual,
        Auto
    };

    enum FSRDebugCheckerMode
    {
        Disabled = 0,
        EnabledNoMessageCallback,
        EnabledWithMessageCallback
    };

    const float cMipBias[static_cast<uint32_t>(FSRScalePreset::Custom)] = {
        std::log2f(1.f / 1.0f) - 1.f + std::numeric_limits<float>::epsilon(),
        std::log2f(1.f / 1.5f) - 1.f + std::numeric_limits<float>::epsilon(),
        std::log2f(1.f / 1.7f) - 1.f + std::numeric_limits<float>::epsilon(),
        std::log2f(1.f / 2.0f) - 1.f + std::numeric_limits<float>::epsilon(),
        std::log2f(1.f / 3.0f) - 1.f + std::numeric_limits<float>::epsilon()
    };

    static void FfxMsgCallback(uint32_t type, const wchar_t* message);
    ffxReturnCode_t UiCompositionCallback(ffxCallbackDescFrameGenerationPresent*);

    void                     SwitchUpscaler(int32_t newUpscaler);

    void                     UpdatePreset(const int32_t* pOldPreset);
    void                     UpdateUpscaleRatio(const float* pOldRatio);
    void                     UpdateMipBias(const float* pOldBias);

    cauldron::ResolutionInfo UpdateResolution(uint32_t displayWidth, uint32_t displayHeight);
    void                     UpdateFSRContext(bool enabled);

    /**
     * Populate m_hackColorPaths, m_hackMVDPaths, and m_pHackJitterXY.
     * 
     * \return 
     */
    bool                     LoadHackTextureInfo();

    cauldron::UIRenderModule*   m_pUIRenderModule = nullptr;
    cauldron::ResourceView*     m_pRTResourceView = nullptr;

    int32_t         m_UpscaleMethod   = Upscaler_FSRAPI;
    int32_t         m_UiUpscaleMethod = Upscaler_FSRAPI;
    // ONLY used in SetFilter() as a temp value
    FSRScalePreset  m_CurScale        = FSRScalePreset::Custom;
    // The actual value used by app
    FSRScalePreset  m_ScalePreset     = FSRScalePreset::Custom;
    // Determined by m_ScalePreset in UpdatePreset()
    float           m_UpscaleRatio    = 2.f;
    float           m_LetterboxRatio  = 1.f;
    float           m_MipBias         = cMipBias[static_cast<uint32_t>(FSRScalePreset::NativeAA)];
    FSRMaskMode     m_MaskMode        = FSRMaskMode::Auto;
    float           m_Sharpness       = 0.8f;
    uint32_t        m_JitterIndex     = 0;
    float           m_JitterX         = 0.f;
    float           m_JitterY         = 0.f;
    uint64_t        m_FrameID         = 0;

    bool m_IsNonNative                              = true;
    bool m_UpscaleRatioEnabled                      = false;
    bool m_UseMask                                  = true;
    bool m_UseDistortionField                       = false;
    bool m_RCASSharpen                              = true;
    bool m_SharpnessEnabled                         = false;
    bool m_NeedReInit                               = false;

    bool m_FrameInterpolationAvailable              = false;
    bool m_AsyncComputeAvailable                    = false;
    bool m_EnableMaskOptions                        = true;
    bool m_EnableWaitCallbackModeUI                 = true;
    bool m_FrameInterpolation                       = true;
    bool m_EnableAsyncCompute                       = true;
    bool m_AllowAsyncCompute                        = true;
    bool m_PendingEnableAsyncCompute                = true;
    /// Originally set to true, but not using callback gives us more explicit control.
    /// See .cpp line 2140
    bool m_UseCallback                              = false;
    bool m_DrawFrameGenerationDebugTearLines        = false;
    bool m_DrawFrameGenerationDebugResetIndicators  = false;
    bool m_DrawFrameGenerationDebugPacingLines      = false;
    bool m_DrawFrameGenerationDebugView             = false;
    bool m_DrawUpscalerDebugView                    = false;
    bool m_PresentInterpolatedOnly                  = true;
    bool m_SimulatePresentSkip                      = false;
    bool m_ResetUpscale                             = false;
    bool m_ResetFrameInterpolation                  = false;
    bool m_DoublebufferInSwapchain                  = false;
    bool m_OfUiEnabled                              = true;

    // FFX API Context members
    std::vector<uint64_t> m_FsrVersionIds;
    uint32_t m_FsrVersionIndex = 0;
    bool        m_overrideVersion = false;
    uint64_t    m_currentUpscaleContextVersionId = 0;
    const char* m_currentUpscaleContextVersionName = nullptr;

    bool m_ffxBackendInitialized = false;
    ffx::Context m_UpscalingContext = nullptr;
    ffx::Context m_FrameGenContext  = nullptr;
    ffx::Context m_SwapChainContext = nullptr;
    ffx::ConfigureDescFrameGeneration m_FrameGenerationConfig{};

    // Backup UI elements
    std::vector<cauldron::UIElement*> m_UIElements{}; // weak ptr

    // FSR resources
    const cauldron::Texture*  m_pColorTarget           = nullptr;
    const cauldron::Texture*  m_pTonemappedColorTarget = nullptr;
    const cauldron::Texture*  m_pTempTexture           = nullptr;
    const cauldron::Texture*  m_pDepthTarget           = nullptr;
    const cauldron::Texture*  m_pMotionVectors         = nullptr;
    const cauldron::Texture*  m_pReactiveMask          = nullptr;
    const cauldron::Texture*  m_pCompositionMask       = nullptr;
    const cauldron::Texture*  m_pOpaqueTexture         = nullptr;

    // Reference to the HackOptions stored in Framework, init in constructor
    const cauldron::HackOptionDef& hackOptions;
    // and our hacking data: input {frame_t_color, motion vectors, depth} textures
    cauldron::Texture* m_pHackColorTarget = nullptr;
    cauldron::Texture* m_pHackMV          = nullptr;
    cauldron::Texture* m_pHackDepth       = nullptr;
    // finally the data storage
    std::vector<std::pair<float, float>>                        m_pHackJitterXY  = {};
    //std::vector<std::unique_ptr<cauldron::EXRTextureDataBlock>> m_pHackColorData = {};
    //std::vector<std::unique_ptr<cauldron::EXRTextureDataBlock>> m_pHackMVData    = {};
    //std::vector<std::unique_ptr<cauldron::EXRTextureDataBlock>> m_pHackDepthData = {};

    std::vector<std::filesystem::path> m_hackColorPaths = {};
    std::vector<std::filesystem::path> m_hackMVDPaths   = {};
    // see ExportDebugFrame()
    static constexpr size_t m_kSkipFramesInput = 0;
    static constexpr size_t m_kSkipFramesSR    = 1;
    static constexpr size_t m_kSkipFramesFG    = 3;

    // Raster views for reactive/composition masks
    std::vector<const cauldron::RasterView*> m_RasterViews           = {};
    cauldron::ResourceView*                  m_pUiTargetResourceView = nullptr;

    // For resolution updates
    std::function<cauldron::ResolutionInfo(uint32_t, uint32_t)> m_pUpdateFunc = nullptr;

    bool     s_enableSoftwareMotionEstimation = true;
    int32_t  s_uiRenderMode      = 2;
    int32_t  s_uiRenderModeNextFrame = 2; // needs to be in-sync with s_uiRenderMode after deviating at most 1 frame.

    // Surfaces for different UI render modes
    uint32_t                 m_curUiTextureIndex  = 0;
    const cauldron::Texture* m_pUiTexture[2]      = {};
    const cauldron::Texture* m_pHudLessTexture[2] = {};
    const cauldron::Texture* m_pDistortionField[2] = {};

    TAARenderModule*          m_pTAARenderModule         = nullptr;
    ToneMappingRenderModule*  m_pToneMappingRenderModule = nullptr;
    TranslucencyRenderModule* m_pTransRenderModule       = nullptr;

    //Set Constant Buffer KeyValue via Configure Context KeyValue API. Valid Post Context creation.
    int32_t                  m_UpscalerCBKey = 0;
    float                    m_UpscalerCBValue = 1.f;
    float                    m_UpscalerCBValueStore[5] = {1.f,1.f,1.f,1.0f/3,-1.0f/3};
    void                     SetUpscaleConstantBuffer(uint64_t key, float value);

    FSRDebugCheckerMode      m_GlobalDebugCheckerMode = FSRDebugCheckerMode::Disabled;
    void                     SetGlobalDebugCheckerMode(FSRDebugCheckerMode mode, bool recreate);

    //Set Swapchain waitcallback via Configure Context KeyValue API
    int32_t                  m_waitCallbackMode = 0;

    //Set Swapchain Frame pacing Tuning
    float m_SafetyMarginInMs; // in Millisecond
    float m_VarianceFactor; // valid range [0.0,1.0]
    bool  m_AllowHybridSpin;
    uint32_t m_HybridSpinTime;
    bool m_AllowWaitForSingleObjectOnFence;
    FfxApiSwapchainFramePacingTuning framePacingTuning;
};

// alias to get sample.cpp to use this class.
using FSRApiRenderModule = FSRRenderModule;
