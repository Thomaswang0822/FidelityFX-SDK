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

#include "fsrapirendermodule.h"
#include "render/rendermodules/ui/uirendermodule.h"
#include "render/rasterview.h"
#include "render/dynamicresourcepool.h"
#include "render/parameterset.h"
#include "render/pipelineobject.h"
#include "render/rootsignature.h"
#include "render/swapchain.h"
#include "render/resourceviewallocator.h"
#include "core/backend_interface.h"
#include "core/scene.h"
#include "misc/assert.h"
#include "render/profiler.h"
#include "translucency/translucencyrendermodule.h"
#include "core/win/framework_win.h"

#if defined(FFX_API_DX12)
#include <ffx_api/dx12/ffx_api_dx12.hpp>
#include "render/dx12/device_dx12.h"
#include "render/dx12/commandlist_dx12.h"
#elif defined(FFX_API_VK)
#include <ffx_api/vk/ffx_api_vk.hpp>
#include "render/vk/device_vk.h"
#include "render/vk/commandlist_vk.h"
#include "render/vk/swapchain_vk.h"
#endif  // FFX_API_DX12

#include <filesystem>
#include <directx/d3dx12_core.h>

#include <unordered_map>

/**
 * Frame Interpolation SRV textures. 
 * pair.first is our custom name used in building export filename;
 * pair.second is the internal name used by shaders.
 */
namespace SRV_debug
{
    typedef std::pair<std::string, std::wstring> twinName;

    static const twinName InputDepth{"InputDepth", L"r_input_depth"};
    static const twinName InputMV{"InputMV", L"r_input_motion_vectors"};
    static const twinName InputDF{"InputDF", L"r_input_distortion_field"};
    static const twinName DilatedDepth{"DilatedDepth", L"r_dilated_depth"};
    static const twinName DilatedMV{"DilatedMV", L"r_dilated_motion_vectors"};
    static const twinName RecDepthPrev{"RecDepthPrev", L"r_reconstructed_depth_previous_frame"};
    static const twinName RecDepthInterp{"RecDepthInterp", L"r_reconstructed_depth_interpolated_frame"};
    /// 
    static const twinName PrevInterpSource{"PrevInterpSource", L"r_previous_interpolation_source"};
    static const twinName CurrInterpSource{"CurrInterpSource", L"r_current_interpolation_source"};
    static const twinName DisMask{"DisMask", L"r_disocclusion_mask"};
    static const twinName GameMV_X{"GameMV", L"r_game_motion_vector_field_x"};
    static const twinName GameMV_Y{"GameMV", L"r_game_motion_vector_field_y"};
    static const twinName OFlowMV_X{"OFlowMV", L"r_optical_flow_motion_vector_field_x"};
    static const twinName OFlowMV_Y{"OFlowMV", L"r_optical_flow_motion_vector_field_y"};
    static const twinName OFlowVec{"OFlowVec", L"r_optical_flow"};
    static const twinName OFlowVecDebug{"OFlowVecDebug", L"r_optical_flow_vec_debug"};
    static const twinName OFlowConf{"OFlowConf", L"r_optical_flow_confidence"};
    static const twinName OFlowGlobalMotion{"OFlowGlobalMotion", L"r_optical_flow_global_motion"};
    static const twinName OFlowSCD{"OFlowSCD", L"r_optical_flow_scd"};
    static const twinName Output{"Output", L"r_output"};
    static const twinName InpaintingMask{"InpaintingMask", L"r_inpainting_mask"};
    static const twinName InpaintingPyramid{"InpaintingPyramid", L"r_inpainting_pyramid"};
    static const twinName PresentBB{"PresentBB", L"r_present_backbuffer"};
    static const twinName COUNTERS{"COUNTERS", L"r_counters"};

    // unordered_set will auto remove duplicates
    static const std::unordered_set<std::string> Uint32MVs = {
        GameMV_X.first, 
        GameMV_Y.first, 
        OFlowMV_X.first, 
        OFlowMV_Y.first
    };

    static const std::unordered_set<std::string> FP16MVs = {
        InputMV.first,
        DilatedMV.first,
        OFlowVecDebug.first,
    };

    /// #define SCD_OUTPUT_SCENE_CHANGE_SLOT         0
    /// #define SCD_OUTPUT_HISTORY_BITS_SLOT         1
    /// #define SCD_OUTPUT_COMPLETED_WORKGROUPS_SLOT 2
    static const std::array<std::wstring, 3> ValueNamesSCD = {L"SCENE_CHANGE", L"HISTORY_BITS", L"COMPLETED_WORKGROUPS"};
    static const std::array<std::wstring, 2> ValueNamesDF  = {L"X", L"Y"};

    static bool LogValues(const void* data, cauldron::ExportInfo::BitUnpackMode mode, size_t frameID)
    {
        if (mode == cauldron::ExportInfo::BitUnpackMode::LogSCD) {
            const uint32_t* pValues = reinterpret_cast<const uint32_t*>(data);
            cauldron::CauldronWarning(L"Frame %d %s values: %s = %d, %s = %d, %s = %d.",
                frameID, StringToWString(OFlowSCD.first).c_str(),
                ValueNamesSCD[0].c_str(), pValues[0],
                ValueNamesSCD[1].c_str(), pValues[1],
                ValueNamesSCD[2].c_str(), pValues[2]
            );
            return true;
        }
        else if (mode == cauldron::ExportInfo::BitUnpackMode::LogDF) {
            const uint8_t* pValues = reinterpret_cast<const uint8_t*>(data);
            cauldron::CauldronWarning(L"Frame %d %s values: %s = %d, %s = %d.",
                frameID, StringToWString(InputDF.first).c_str(),
                ValueNamesDF[0].c_str(), pValues[0],
                ValueNamesDF[1].c_str(), pValues[1]
            );
            return true;
        }

        // If not any "direct log" SRV, keep success = false s.t. we proceed to export EXR.
        return false;
    }
}  // namespace TextureNames

using namespace std;
using namespace cauldron;
namespace fs = std::filesystem;

void RestoreApplicationSwapChain(bool recreateSwapchain = true);

/**
 * Instead of treating these exr files as Content Texture, we must read them directly
 * as Render Texture. The former is handled via TextureLoader and ContentManager, 
 * and the latter is handled via DynamicResourcePool.
 * We must use `void Texture::CopyData(TextureDataBlock* pTextureDataBlock)`, and this function
 * takes >40ms according to the debugger. Thus we must put it in init stage.
 * 
 * \return bool success?
 */
bool FSRRenderModule::LoadHackTextures()
{
    // first do a sanity check on those paths in HackOptions
    for (const auto& inPath : hackOptions.hackPaths)
    {
        CauldronAssert(ASSERT_ERROR, fs::exists(inPath), 
            L"Input folder %s deosn't exist", inPath.c_str());

    }
    if (!fs::exists(hackOptions.outPath))
    {
        // Defensive, in case path doesn't exist
        CauldronWarning(L"Sceenshot output dir DNE and will be created: %s", hackOptions.outPath.c_str());
        fs::create_directory(hackOptions.outPath);
    }

    std::vector<std::wstring>       textureLoadPaths  = hackOptions.hackPaths;
    const std::vector<std::wstring> renderTargetNames = {L"CurrFrameHack", L"MvHack", L"DepthHack"};
    // temporarily used
    std::vector<std::vector<cauldron::Texture*>*> hackTargets = {&m_pHackColors, &m_pHackMVs, &m_pHackDepths};

    std::vector<std::filesystem::path> exrFiles;

    size_t       nTextures;
    std::wstring rtFullname;

    // Populate exr filepath lists and return count
    auto populatePathList = [](std::wstring folderPath, std::vector<std::filesystem::path>& outPaths) -> size_t {
        outPaths.clear();
        for (const auto& entry : filesystem::directory_iterator(folderPath))
        {
            if (entry.path().extension() == ".exr")
                outPaths.push_back(entry.path());
        }
        // Sort files to ensure proper frame order (assuming filenames contain frame numbers)
        std::sort(outPaths.begin(), outPaths.end()); 

        return outPaths.size();
    };

    for (int typeIdx=0; typeIdx<3; ++typeIdx)
    {
        // 0: ColorRGB, 1: MotionVectors, 2: Depth
        auto type = static_cast<EXRTextureDataBlock::SpecialChannelType>(typeIdx);

        // grab all exr files of current input type, parse jitter if reading ColorRGB
        bool parseJitter = hackOptions.parseJitter && 
                           type == EXRTextureDataBlock::SpecialChannelType::ColorRGB;

        nTextures        = populatePathList(textureLoadPaths[typeIdx], exrFiles);
        CauldronAssert(ASSERT_CRITICAL, nTextures == hackOptions.frameCount, 
                       L"No. input files counted by EXRTextureDataBlock::ParseJitter() (%d) and lambda function (%d) don't match.",
                       nTextures, hackOptions.frameCount);
        if (parseJitter)
            EXRTextureDataBlock::ParseJitter(exrFiles, m_pHackJitterXY);

        /// NOTE: hackOptions.frameCount is the fixed total number of frames to load,
        /// but user can set a smaller hackOptions.outputMaxCount to check in test runs.
        nTextures = hackOptions.outputMaxCount;
        for (size_t frameIdx = 0; frameIdx < nTextures; ++frameIdx)
        {
            rtFullname = renderTargetNames[typeIdx] + L"_" + std::to_wstring(frameIdx);
            TextureLoadInfo textureInfo(exrFiles[frameIdx]);

            // NOTE, m_UpscaleRatio is display res / render res, controlled by m_CurScale.
            auto textureDB = std::make_unique<EXRTextureDataBlock>();
            if (type == EXRTextureDataBlock::SpecialChannelType::ColorRGB)
            {
                textureDB->SetResourceFormat(GetFramework()->GetSwapChain()->GetSwapChainFormat());
            }
            TextureDesc textureDesc = {};

            // first get the render target
            hackTargets[typeIdx]->push_back(const_cast<cauldron::Texture*>(GetFramework()->GetRenderTexture(rtFullname.c_str())));
            CauldronAssert(ASSERT_CRITICAL, hackTargets[typeIdx]->back() != nullptr, L"Hack render target %s GetRenderTexture() failed", rtFullname.c_str());

            // load
            bool loaded = false;
            if (type == EXRTextureDataBlock::SpecialChannelType::ColorRGB)
            {
                loaded = textureDB->LoadTextureData(textureInfo.TextureFile, textureInfo.AlphaThreshold, textureDesc);
            }
            else
            {
                loaded = textureDB->LoadJitterData1K(textureInfo.TextureFile, textureInfo.AlphaThreshold, textureDesc, type);
            }
            CauldronAssert(ASSERT_CRITICAL, loaded, L"Hack texture %s loaded failed", textureLoadPaths[typeIdx].c_str());
            // then copy
            hackTargets[typeIdx]->back()->CopyData(textureDB.get());

        }  // end of each texture
    }  // end of all textures of one type

    //CheckHackColors("AfterLoad");

    return true;
}

void FSRRenderModule::Init(const json& initData)
{
    m_pTAARenderModule   = static_cast<TAARenderModule*>(GetFramework()->GetRenderModule("TAARenderModule"));
    m_pTransRenderModule = static_cast<TranslucencyRenderModule*>(GetFramework()->GetRenderModule("TranslucencyRenderModule"));
    m_pToneMappingRenderModule = static_cast<ToneMappingRenderModule*>(GetFramework()->GetRenderModule("ToneMappingRenderModule"));
    CauldronAssert(ASSERT_CRITICAL, m_pTAARenderModule, L"FidelityFX FSR Sample: Error: Could not find TAA render module.");
    CauldronAssert(ASSERT_CRITICAL, m_pTransRenderModule, L"FidelityFX FSR Sample: Error: Could not find Translucency render module.");
    CauldronAssert(ASSERT_CRITICAL, m_pToneMappingRenderModule, L"FidelityFX FSR Sample: Error: Could not find Tone Mapping render module.");

    // Fetch needed resources
    m_pColorTarget           = GetFramework()->GetColorTargetForCallback(GetName());
    m_pTonemappedColorTarget = GetFramework()->GetRenderTexture(L"SwapChainProxy");
    m_pDepthTarget           = GetFramework()->GetRenderTexture(L"DepthTarget");
    m_pMotionVectors         = GetFramework()->GetRenderTexture(L"GBufferMotionVectorRT");
    m_pDistortionField[0]    = GetFramework()->GetRenderTexture(L"DistortionField0");
    m_pDistortionField[1]    = GetFramework()->GetRenderTexture(L"DistortionField1");
    m_pReactiveMask          = GetFramework()->GetRenderTexture(L"ReactiveMask");
    m_pCompositionMask       = GetFramework()->GetRenderTexture(L"TransCompMask");
    CauldronAssert(ASSERT_CRITICAL, m_pMotionVectors && m_pDistortionField[0] && m_pDistortionField[1] && m_pReactiveMask && m_pCompositionMask, L"Could not get one of the needed resources for FSR Rendermodule.");

    // Get a CPU resource view that we'll use to map the render target to
    GetResourceViewAllocator()->AllocateCPURenderViews(&m_pRTResourceView);

    // Create render resolution opaque render target to use for auto-reactive mask generation
    TextureDesc desc = m_pColorTarget->GetDesc();
    const ResolutionInfo& resInfo = GetFramework()->GetResolutionInfo();
    desc.Width = resInfo.RenderWidth;
    desc.Height = resInfo.RenderHeight;
    desc.Name = L"FSR_OpaqueTexture";
    m_pOpaqueTexture = GetDynamicResourcePool()->CreateRenderTexture(&desc, [](TextureDesc& desc, uint32_t displayWidth, uint32_t displayHeight, uint32_t renderingWidth, uint32_t renderingHeight)
        {
            desc.Width = renderingWidth;
            desc.Height = renderingHeight;
        });

    // Register additional exports for translucency pass
    BlendDesc      reactiveCompositionBlend = {
        true, Blend::InvDstColor, Blend::One, BlendOp::Add, Blend::One, Blend::Zero, BlendOp::Add, static_cast<uint32_t>(ColorWriteMask::Red)};

    OptionalTransparencyOptions transOptions;
    transOptions.OptionalTargets.push_back(std::make_pair(m_pReactiveMask, reactiveCompositionBlend));
    transOptions.OptionalTargets.push_back(std::make_pair(m_pCompositionMask, reactiveCompositionBlend));
    transOptions.OptionalAdditionalOutputs = L"float ReactiveTarget : SV_TARGET1; float CompositionTarget : SV_TARGET2;";
    transOptions.OptionalAdditionalExports =
        L"float hasAnimatedTexture = 0.f; output.ReactiveTarget = ReactiveMask; output.CompositionTarget = max(Alpha, hasAnimatedTexture);";

    // Add additional exports for FSR to translucency pass
    m_pTransRenderModule->AddOptionalTransparencyOptions(transOptions);

    // Create temporary texture to copy color into before upscale
    {
        TextureDesc desc = m_pColorTarget->GetDesc();
        desc.Name        = L"UpscaleIntermediateTarget";
        desc.Width       = m_pColorTarget->GetDesc().Width;
        desc.Height      = m_pColorTarget->GetDesc().Height;

        m_pTempTexture = GetDynamicResourcePool()->CreateRenderTexture(
            &desc, [](TextureDesc& desc, uint32_t displayWidth, uint32_t displayHeight, uint32_t renderingWidth, uint32_t renderingHeight) {
                desc.Width  = displayWidth;
                desc.Height = displayHeight;
            });
        CauldronAssert(ASSERT_CRITICAL, m_pTempTexture, L"Couldn't create intermediate texture.");
    }
   
    // Create raster views on the reactive mask and composition masks (for clearing and rendering)
    m_RasterViews.resize(2);
    m_RasterViews[0] = GetRasterViewAllocator()->RequestRasterView(m_pReactiveMask, ViewDimension::Texture2D);
    m_RasterViews[1] = GetRasterViewAllocator()->RequestRasterView(m_pCompositionMask, ViewDimension::Texture2D);

    // Set our render resolution function as that to use during resize to get render width/height from display width/height
    m_pUpdateFunc = 
        
        [this](uint32_t displayWidth, uint32_t displayHeight) { return this->UpdateResolution(displayWidth, displayHeight); };

    //////////////////////////////////////////////////////////////////////////
    // Register additional execution callbacks during the frame

    // Register a post-lighting callback to copy opaque texture
    ExecuteCallback callbackPreTrans = [this](double deltaTime, CommandList* pCmdList) {
        this->PreTransCallback(deltaTime, pCmdList);
    };
    ExecutionTuple callbackPreTransTuple = std::make_pair(L"FSRRenderModule::PreTransCallback", std::make_pair(this, callbackPreTrans));
    GetFramework()->RegisterExecutionCallback(L"LightingRenderModule", false, callbackPreTransTuple);

    // Register a post-transparency callback to generate reactive mask
    ExecuteCallback callbackPostTrans = [this](double deltaTime, CommandList* pCmdList) {
        this->PostTransCallback(deltaTime, pCmdList);
    };
    ExecutionTuple callbackPostTransTuple = std::make_pair(L"FSRRenderModule::PostTransCallback", std::make_pair(this, callbackPostTrans));
    GetFramework()->RegisterExecutionCallback(L"TranslucencyRenderModule", false, callbackPostTransTuple);

    m_curUiTextureIndex     = 0;
    
    // Get the proper UI color target
    m_pUiTexture[0] = GetFramework()->GetRenderTexture(L"UITarget0");
    m_pUiTexture[1] = GetFramework()->GetRenderTexture(L"UITarget1");
    
    // Create FrameInterpolationSwapchain
    // Separate from FSR generation so it can be done when the engine creates the swapchain
    // should not be created and destroyed with FSR, as it requires a switch to windowed mode

    
#if defined(FFX_API_DX12)

    m_FrameInterpolationAvailable = true;
    m_AsyncComputeAvailable       = true;

#elif defined(FFX_API_VK)

    const cauldron::FIQueue* pAsyncComputeQueue = cauldron::GetDevice()->GetImpl()->GetFIAsyncComputeQueue();
    const cauldron::FIQueue* pPresentQueue      = cauldron::GetDevice()->GetImpl()->GetFIPresentQueue();
    const cauldron::FIQueue* pImageAcquireQueue = cauldron::GetDevice()->GetImpl()->GetFIImageAcquireQueue();

    m_FrameInterpolationAvailable = pPresentQueue->queue != VK_NULL_HANDLE && pImageAcquireQueue->queue != VK_NULL_HANDLE;
    m_AsyncComputeAvailable       = m_FrameInterpolationAvailable && pAsyncComputeQueue->queue != VK_NULL_HANDLE;

#endif  // defined(FFX_API_DX12)

    if (!m_FrameInterpolationAvailable)
    {
        m_FrameInterpolation = false;
        s_uiRenderMode       = 0;  // no UI handling
        s_uiRenderModeNextFrame = 0;
        CauldronWarning(L"Frame interpolation isn't available on this device.");
    }
    if (!m_AsyncComputeAvailable)
    {
        m_EnableAsyncCompute        = false;
        m_PendingEnableAsyncCompute = false;
        m_AllowAsyncCompute         = false;

        CauldronWarning(L"Async compute Frame interpolation isn't available on this device.");
    }

    if (m_FrameInterpolationAvailable)
    {
#if defined(FFX_API_DX12)
        IDXGISwapChain4* dxgiSwapchain = GetSwapChain()->GetImpl()->DX12SwapChain();
        dxgiSwapchain->AddRef();
        cauldron::GetSwapChain()->GetImpl()->SetDXGISwapChain(nullptr);

        ffx::CreateContextDescFrameGenerationSwapChainForHwndDX12 createSwapChainDesc{};
        dxgiSwapchain->GetHwnd(&createSwapChainDesc.hwnd);
        DXGI_SWAP_CHAIN_DESC1 desc1;
        dxgiSwapchain->GetDesc1(&desc1);
        createSwapChainDesc.desc = &desc1;
        DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc;
        dxgiSwapchain->GetFullscreenDesc(&fullscreenDesc);
        createSwapChainDesc.fullscreenDesc = &fullscreenDesc;
        dxgiSwapchain->GetParent(IID_PPV_ARGS(&createSwapChainDesc.dxgiFactory));
        createSwapChainDesc.gameQueue = GetDevice()->GetImpl()->DX12CmdQueue(cauldron::CommandQueue::Graphics);

        dxgiSwapchain->Release();
        dxgiSwapchain = nullptr;
        createSwapChainDesc.swapchain = &dxgiSwapchain;

        ffx::ReturnCode retCode = ffx::CreateContext(m_SwapChainContext, nullptr, createSwapChainDesc);
        CauldronAssert(ASSERT_CRITICAL, retCode == ffx::ReturnCode::Ok, L"Couldn't create the ffxapi fg swapchain (dx12): %d", (uint32_t)retCode);
        createSwapChainDesc.dxgiFactory->Release();

        cauldron::GetSwapChain()->GetImpl()->SetDXGISwapChain(dxgiSwapchain);

        // In case the app is handling Alt-Enter manually we need to update the window association after creating a different swapchain
        IDXGIFactory7* factory = nullptr;
        if (SUCCEEDED(dxgiSwapchain->GetParent(IID_PPV_ARGS(&factory))))
        {
            factory->MakeWindowAssociation(cauldron::GetFramework()->GetImpl()->GetHWND(), DXGI_MWA_NO_WINDOW_CHANGES);
            factory->Release();
        }

        dxgiSwapchain->Release();

        // Lets do the same for HDR as well as it will need to be re initialized since swapchain was re created
        cauldron::GetSwapChain()->SetHDRMetadataAndColorspace();

#elif defined(FFX_API_VK)

        // Create frameinterpolation swapchain
        cauldron::SwapChain* pSwapchain       = cauldron::GetFramework()->GetSwapChain();
        VkSwapchainKHR       currentSwapchain = pSwapchain->GetImpl()->VKSwapChain();

        ffx::CreateContextDescFrameGenerationSwapChainVK createSwapChainDesc{};
        createSwapChainDesc.physicalDevice        = cauldron::GetDevice()->GetImpl()->VKPhysicalDevice();
        createSwapChainDesc.device                = cauldron::GetDevice()->GetImpl()->VKDevice();
        createSwapChainDesc.swapchain             = &currentSwapchain;
        createSwapChainDesc.createInfo            = *cauldron::GetFramework()->GetSwapChain()->GetImpl()->GetCreateInfo();
        createSwapChainDesc.allocator             = nullptr;
        createSwapChainDesc.gameQueue.queue       = cauldron::GetDevice()->GetImpl()->VKCmdQueue(cauldron::CommandQueue::Graphics);
        createSwapChainDesc.gameQueue.familyIndex = cauldron::GetDevice()->GetImpl()->VKCmdQueueFamily(cauldron::CommandQueue::Graphics);
        createSwapChainDesc.gameQueue.submitFunc  = nullptr;  // this queue is only used in vkQueuePresentKHR, hence doesn't need a callback

        createSwapChainDesc.asyncComputeQueue.queue       = pAsyncComputeQueue->queue;
        createSwapChainDesc.asyncComputeQueue.familyIndex = pAsyncComputeQueue->family;
        createSwapChainDesc.asyncComputeQueue.submitFunc  = nullptr;

        createSwapChainDesc.presentQueue.queue       = pPresentQueue->queue;
        createSwapChainDesc.presentQueue.familyIndex = pPresentQueue->family;
        createSwapChainDesc.presentQueue.submitFunc  = nullptr;

        createSwapChainDesc.imageAcquireQueue.queue       = pImageAcquireQueue->queue;
        createSwapChainDesc.imageAcquireQueue.familyIndex = pImageAcquireQueue->family;
        createSwapChainDesc.imageAcquireQueue.submitFunc  = nullptr;

        // make sure swapchain is not holding a ref to real swapchain
        cauldron::GetFramework()->GetSwapChain()->GetImpl()->SetVKSwapChain(VK_NULL_HANDLE);

        auto convertQueueInfo = [](VkQueueInfoFFXAPI queueInfo) {
            VkQueueInfoFFX info;
            info.queue       = queueInfo.queue;
            info.familyIndex = queueInfo.familyIndex;
            info.submitFunc  = queueInfo.submitFunc;
            return info;
        };

        VkFrameInterpolationInfoFFX frameInterpolationInfo = {};
        frameInterpolationInfo.device                      = createSwapChainDesc.device;
        frameInterpolationInfo.physicalDevice              = createSwapChainDesc.physicalDevice;
        frameInterpolationInfo.pAllocator                  = createSwapChainDesc.allocator;
        frameInterpolationInfo.gameQueue                   = convertQueueInfo(createSwapChainDesc.gameQueue);
        frameInterpolationInfo.asyncComputeQueue           = convertQueueInfo(createSwapChainDesc.asyncComputeQueue);
        frameInterpolationInfo.presentQueue                = convertQueueInfo(createSwapChainDesc.presentQueue);
        frameInterpolationInfo.imageAcquireQueue           = convertQueueInfo(createSwapChainDesc.imageAcquireQueue);

        ffx::ReturnCode retCode = ffx::CreateContext(m_SwapChainContext, nullptr, createSwapChainDesc);

        ffx::QueryDescSwapchainReplacementFunctionsVK replacementFunctions{};
        ffx::Query(m_SwapChainContext, replacementFunctions);
        cauldron::GetDevice()->GetImpl()->SetSwapchainMethodsAndContext(nullptr,
                                                                        nullptr,
                                                                        replacementFunctions.pOutGetSwapchainImagesKHR,
                                                                        replacementFunctions.pOutAcquireNextImageKHR,
                                                                        replacementFunctions.pOutQueuePresentKHR,
                                                                        replacementFunctions.pOutSetHdrMetadataEXT,
                                                                        replacementFunctions.pOutCreateSwapchainFFXAPI,
                                                                        replacementFunctions.pOutDestroySwapchainFFXAPI,
                                                                        nullptr,
                                                                        replacementFunctions.pOutGetLastPresentCountFFXAPI,
                                                                        m_SwapChainContext,
                                                                        &frameInterpolationInfo);

        // Set frameinterpolation swapchain to engine
        cauldron::GetFramework()->GetSwapChain()->GetImpl()->SetVKSwapChain(currentSwapchain, true);

        // we need to re initialize HDR info since swapchain was re created
        cauldron::GetSwapChain()->SetHDRMetadataAndColorspace();
#endif  // defined(FFX_API_DX12)
    }

    // Fetch hudless texture resources
    m_pHudLessTexture[0] = GetFramework()->GetRenderTexture(L"HudlessTarget0");
    m_pHudLessTexture[1] = GetFramework()->GetRenderTexture(L"HudlessTarget1");

    // Start disabled as this will be enabled externally
    cauldron::RenderModule::SetModuleEnabled(false);

    {
        // Register upscale method picker picker
        UISection* uiSection = GetUIManager()->RegisterUIElements("Upscaling", UISectionType::Sample);
        InitUI(uiSection);
    }

    //////////////////////////////////////////////////////////////////////////
    // Finish up init

    /// TODO: set m_ScalePreset to Custom
    if (hackOptions.enableHack)
        m_ScalePreset = FSRScalePreset::Custom;

    SwitchUpscaler(m_UiUpscaleMethod);

    /// We load the hacking textures in the very end, because it needs render resolution to be updated
    /// different from display resolution. This is done AFTER:
    /// - setting m_pUpdateFunc (see above);
    /// - calling SwitchUpscaler, which ultimately calls m_pUpdateFunc to change GetFramework()->GetResolutionInfo()
    ///     and Framework::ResizeEvent() to actually rezie render targets
    if (hackOptions.enableHack)
        CauldronAssert(ASSERT_CRITICAL, LoadHackTextures(), L"Loading hack textures failed");


    // That's all we need for now
    SetModuleReady(true);
}

FSRRenderModule::~FSRRenderModule()
{
    // Destroy the FSR context
    UpdateFSRContext(false);

    if (m_SwapChainContext != nullptr)
    {
        // Restore the application's swapchain
        ffx::DestroyContext(m_SwapChainContext);
        RestoreApplicationSwapChain(false);
    }
}


void FSRRenderModule::EnableModule(bool enabled)
{
    // If disabling the render module, we need to disable the upscaler with the framework
    if (!enabled)
    {
        // Toggle this now so we avoid the context changes in OnResize
        SetModuleEnabled(enabled);

        // Destroy the FSR context
        UpdateFSRContext(false);

        if (GetFramework()->UpscalerEnabled())
            GetFramework()->EnableUpscaling(false);
        if (GetFramework()->FrameInterpolationEnabled())
            GetFramework()->EnableFrameInterpolation(false);

        UIRenderModule* uimod = static_cast<UIRenderModule*>(GetFramework()->GetRenderModule("UIRenderModule"));
        uimod->SetAsyncRender(false);
        uimod->SetRenderToTexture(false);
        uimod->SetCopyHudLessTexture(false);

        CameraComponent::SetJitterCallbackFunc(nullptr);
    }
    else
    {
        UIRenderModule* uimod = static_cast<UIRenderModule*>(GetFramework()->GetRenderModule("UIRenderModule"));
        uimod->SetAsyncRender(s_uiRenderMode == 2);
        uimod->SetRenderToTexture(s_uiRenderMode == 1);
        uimod->SetCopyHudLessTexture(s_uiRenderMode == 3);

        // Setup everything needed when activating FSR
        // Will also enable upscaling
        UpdatePreset(nullptr);

        // Toggle this now so we avoid the context changes in OnResize
        SetModuleEnabled(enabled);

        // Create the FSR context
        UpdateFSRContext(true);

        if (m_UpscaleMethod == Upscaler_FSRAPI)
        {
            // Set the jitter callback to use
            CameraJitterCallback jitterCallback = [this](Vec2& values) {
                // Increment jitter index for frame
                ++m_JitterIndex;

                // Update FSR jitter for built in TAA
                const ResolutionInfo& resInfo = GetFramework()->GetResolutionInfo();

                ffx::ReturnCode                     retCode;
                int32_t                             jitterPhaseCount;
                ffx::QueryDescUpscaleGetJitterPhaseCount getJitterPhaseDesc{};
                getJitterPhaseDesc.displayWidth   = resInfo.DisplayWidth;
                getJitterPhaseDesc.renderWidth    = resInfo.RenderWidth;
                getJitterPhaseDesc.pOutPhaseCount = &jitterPhaseCount;

                retCode = ffx::Query(m_UpscalingContext, getJitterPhaseDesc);
                CauldronAssert(ASSERT_CRITICAL, retCode == ffx::ReturnCode::Ok,
                    L"ffxQuery(FSR_GETJITTERPHASECOUNT) returned %d", retCode);

                ffx::QueryDescUpscaleGetJitterOffset getJitterOffsetDesc{};
                getJitterOffsetDesc.index                              = m_JitterIndex;
                getJitterOffsetDesc.phaseCount                         = jitterPhaseCount;
                getJitterOffsetDesc.pOutX                              = &m_JitterX;
                getJitterOffsetDesc.pOutY                              = &m_JitterY;

                retCode = ffx::Query(m_UpscalingContext, getJitterOffsetDesc);

                CauldronAssert(ASSERT_CRITICAL, retCode == ffx::ReturnCode::Ok,
                    L"ffxQuery(FSR_GETJITTEROFFSET) returned %d", retCode);

                values = Vec2(-2.f * m_JitterX / resInfo.RenderWidth, 2.f * m_JitterY / resInfo.RenderHeight);
            };
            CameraComponent::SetJitterCallbackFunc(jitterCallback);
        }

        ClearReInit();
    }

    // Show or hide UI elements for active upscaler
    for (auto& i : m_UIElements)
    {
        i->Show(enabled);
    }
}

FfxErrorCode waitCallback(wchar_t* fenceName, uint64_t fenceValueToWaitFor)
{
    CAUDRON_LOG_DEBUG(L"waiting on '%ls' with value %llu", fenceName, fenceValueToWaitFor);
    return FFX_API_RETURN_OK;
}

void FSRRenderModule::InitUI(UISection* pUISection)
{
    static const bool        alwaysTrue   = true;
    std::vector<const char*> comboOptions = {"Native", "FSR (ffxapi)"};
    pUISection->RegisterUIElement<UICombo>("Method", (int32_t&)m_UiUpscaleMethod, std::move(comboOptions), [this](int32_t cur, int32_t old) { SwitchUpscaler(cur); });

    m_UIElements.emplace_back(
        pUISection->RegisterUIElement<UICheckBox>("Override FSR Version", m_overrideVersion, [this](int32_t, int32_t) { m_NeedReInit = true; }, false));

    // get version info from ffxapi
    ffx::QueryDescGetVersions versionQuery{};
    versionQuery.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
#ifdef FFX_API_DX12
    versionQuery.device = GetDevice()->GetImpl()->DX12Device();
#endif
    uint64_t versionCount = 0;
    versionQuery.outputCount = &versionCount;
    ffxQuery(nullptr, &versionQuery.header);

    std::vector<const char*> versionNames;
    m_FsrVersionIds.resize(versionCount);
    versionNames.resize(versionCount);
    versionQuery.versionIds = m_FsrVersionIds.data();
    versionQuery.versionNames = versionNames.data();
    ffxQuery(nullptr, &versionQuery.header);

    m_UIElements.emplace_back(pUISection->RegisterUIElement<UICombo>(
        "FSR Version", (int32_t&)m_FsrVersionIndex, std::move(versionNames), m_overrideVersion, [this](int32_t, int32_t) { m_NeedReInit = true; }, false));

    // Setup scale preset options
    std::vector<const char*> presetComboOptions = { "Native AA (1.0x)", "Quality (1.5x)", "Balanced (1.7x)", "Performance (2x)", "Ultra Performance (3x)", "Custom" };
    m_UIElements.emplace_back(pUISection->RegisterUIElement<UICombo>(
        "Scale Preset", (int32_t&)m_ScalePreset, std::move(presetComboOptions), m_IsNonNative, [this](int32_t cur, int32_t old) { UpdatePreset(&old); }, false));
    
    // Setup mip bias
    m_UIElements.emplace_back(pUISection->RegisterUIElement<UISlider<float>>(
        "Mip LOD Bias",
        m_MipBias,
        -5.f, 0.f,
        [this](float cur, float old) {
            UpdateMipBias(&old);
        },
        false
    ));

    // Setup scale factor (disabled for all but custom)
    m_UIElements.emplace_back(pUISection->RegisterUIElement<UISlider<float>>(
        "Custom Scale",
        m_UpscaleRatio,
        1.f, 3.f,
        m_UpscaleRatioEnabled,
        [this](float cur, float old) {
            UpdateUpscaleRatio(&old);
        },
        false
    ));

    m_UIElements.emplace_back(pUISection->RegisterUIElement<UISlider<float>>(
        "Letterbox size", m_LetterboxRatio, 0.1f, 1.f, [this](float cur, float old) { UpdateUpscaleRatio(&old); }, false));

    m_UIElements.emplace_back(pUISection->RegisterUIElement<UIButton>("Reset Upscaling", [this]() { m_ResetUpscale = true; }));
    m_UIElements.emplace_back(pUISection->RegisterUIElement<UICheckBox>("Draw upscaler debug view", m_DrawUpscalerDebugView, nullptr, false));

    // Reactive mask
    std::vector<const char*> maskComboOptions{ "Disabled", "Manual Reactive Mask Generation", "Autogen FSR2 Helper Function" };
    m_UIElements.emplace_back(pUISection->RegisterUIElement<UICombo>("Reactive Mask Mode", (int32_t&)m_MaskMode, std::move(maskComboOptions), m_EnableMaskOptions, nullptr, false));

    // Use mask
    m_UIElements.emplace_back(pUISection->RegisterUIElement<UICheckBox>("Use Transparency and Composition Mask", m_UseMask, m_EnableMaskOptions, nullptr, false));

    // Sharpening
    m_UIElements.emplace_back(pUISection->RegisterUIElement<UICheckBox>("RCAS Sharpening", m_RCASSharpen, nullptr, false, false));
    m_UIElements.emplace_back(pUISection->RegisterUIElement<UISlider<float>>("Sharpness", m_Sharpness, 0.f, 1.f, m_RCASSharpen, nullptr, false));

    //Set Upscaler CB KeyValue post context creation
    std::vector<const char*>        configureUpscaleKeyLabels = { "fVelocity", "fReactivenessScale", "fShadingChangeScale", "fAccumulationAddedPerFrame", "fMinDisocclusionAccumulation"};
    
    m_UIElements.emplace_back(pUISection->RegisterUIElement<UICombo>(
        "Upscaler CB Key to set",
        m_UpscalerCBKey,
        configureUpscaleKeyLabels,
        m_EnableMaskOptions,
        [this](float, float) { m_UpscalerCBValue =  m_UpscalerCBValueStore[m_UpscalerCBKey]; },
        m_EnableMaskOptions));
    m_UIElements.emplace_back(pUISection->RegisterUIElement<UISlider<float>>(
        "Upscaler CB Value to set",
        m_UpscalerCBValue,
        -1.f, 2.f,
        m_EnableMaskOptions,
        [this](float, float) { m_UpscalerCBValueStore[m_UpscalerCBKey] = m_UpscalerCBValue; SetUpscaleConstantBuffer(m_UpscalerCBKey, m_UpscalerCBValue); },
        m_EnableMaskOptions));

    // Debug Checker
    std::vector<const char*> debugMessageComboOptions{ "Disabled", "Enabled. Set nullptr message callback", "Enabled. Set Cauldron message callback" };
    m_UIElements.emplace_back(pUISection->RegisterUIElement<UICombo>("Debug Checker", (int32_t&)m_GlobalDebugCheckerMode, debugMessageComboOptions, 
        [this](int32_t nextVal, int32_t prevVal)
        { 
            if ((prevVal == FSRDebugCheckerMode::Disabled && nextVal != FSRDebugCheckerMode::Disabled) || (prevVal != FSRDebugCheckerMode::Disabled && nextVal == FSRDebugCheckerMode::Disabled))
            {
                SetGlobalDebugCheckerMode(static_cast<FSRDebugCheckerMode> (nextVal), true);
            }
            else
            {
                SetGlobalDebugCheckerMode(static_cast<FSRDebugCheckerMode> (nextVal), false);
            }
        }));

    // Frame interpolation
    m_UIElements.emplace_back(pUISection->RegisterUIElement<UICheckBox>("Frame Interpolation", m_FrameInterpolation, m_FrameInterpolationAvailable,
        [this](bool, bool)
        {
            m_OfUiEnabled = m_FrameInterpolation && s_enableSoftwareMotionEstimation;
            
            GetFramework()->EnableFrameInterpolation(m_FrameInterpolation);
            
            // Ask main loop to re-initialize.
            m_NeedReInit = true;
        }, 
        false));

    m_UIElements.emplace_back(pUISection->RegisterUIElement<UICheckBox>("Support Async Compute", m_PendingEnableAsyncCompute,
        m_AsyncComputeAvailable,
        [this](bool, bool) 
        {
            // Ask main loop to re-initialize.
            m_NeedReInit = true;
        }, 
        false));

    m_UIElements.emplace_back(pUISection->RegisterUIElement<UICheckBox>("Allow async compute", m_AllowAsyncCompute, m_PendingEnableAsyncCompute, nullptr, false));
    m_UIElements.emplace_back(pUISection->RegisterUIElement<UICheckBox>("Use callback", m_UseCallback, m_FrameInterpolation, nullptr, false));
    m_UIElements.emplace_back(pUISection->RegisterUIElement<UICheckBox>("Use Distortion Field Input", m_UseDistortionField, m_FrameInterpolation, nullptr, false));
    m_UIElements.emplace_back(pUISection->RegisterUIElement<UICheckBox>("Draw frame generation tear lines", m_DrawFrameGenerationDebugTearLines, m_FrameInterpolation, nullptr, false));
    m_UIElements.emplace_back(pUISection->RegisterUIElement<UICheckBox>("Draw frame generation pacing lines", m_DrawFrameGenerationDebugPacingLines, m_FrameInterpolation, nullptr, false));
    m_UIElements.emplace_back(pUISection->RegisterUIElement<UICheckBox>("Draw frame generation reset indicators", m_DrawFrameGenerationDebugResetIndicators, m_FrameInterpolation, nullptr, false));
    m_UIElements.emplace_back(pUISection->RegisterUIElement<UICheckBox>("Draw frame generation debug view", m_DrawFrameGenerationDebugView, m_FrameInterpolation, nullptr, false));
    m_UIElements.emplace_back(pUISection->RegisterUIElement<UICheckBox>("Present interpolated only", m_PresentInterpolatedOnly, m_FrameInterpolation, nullptr, false));
    m_UIElements.emplace_back(pUISection->RegisterUIElement<UIButton>("Reset Frame Interpolation", m_FrameInterpolation, [this]() { m_ResetFrameInterpolation = true; }));
    m_UIElements.emplace_back(pUISection->RegisterUIElement<UIButton>("Simulate present skip", m_FrameInterpolation, [this]() { m_SimulatePresentSkip = true; }));
    
    std::vector<const char*>        uiRenderModeLabels = { "No UI handling (not recommended)", "UiTexture", "UiCallback", "Pre-Ui Backbuffer" };
    m_UIElements.emplace_back(pUISection->RegisterUIElement<UICombo>(
        "UI Composition Mode",
        s_uiRenderModeNextFrame,
        uiRenderModeLabels, 
        m_FrameInterpolation,
        [this](int32_t, int32_t) 
        {
            // Ask main loop to re-initialize at start of next frame. Next frame set s_uiRenderMode = s_uiRenderModeNextFrame.
            m_NeedReInit = true;
        },
        false));
    m_UIElements.emplace_back(pUISection->RegisterUIElement<UICheckBox>("DoubleBuffer UI resource in swapchain", m_DoublebufferInSwapchain, m_FrameInterpolation, nullptr, false));

    std::vector<const char*>        waitCallbackModeLabels = { "nullptr", "CAUDRON_LOG_DEBUG(\"waitCallback\")"};
    m_UIElements.emplace_back(pUISection->RegisterUIElement<UICombo>(
        "WaitCallback Mode",
        m_waitCallbackMode,
        waitCallbackModeLabels,
        m_EnableWaitCallbackModeUI,
        [this](int32_t, int32_t)
        {
#if defined(FFX_API_DX12)
            ffx::ConfigureDescFrameGenerationSwapChainKeyValueDX12 m_swapchainKeyValueConfig{};
#elif defined(FFX_API_VK)
            ffx::ConfigureDescFrameGenerationSwapChainKeyValueVK m_swapchainKeyValueConfig{};
#endif
            m_swapchainKeyValueConfig.key = FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_WAITCALLBACK;
            if (m_waitCallbackMode == 0)
            {
                m_swapchainKeyValueConfig.ptr = nullptr;
            }
            else if (m_waitCallbackMode == 1)
            {
                m_swapchainKeyValueConfig.ptr = waitCallback;
            }
            ffx::Configure(m_SwapChainContext, m_swapchainKeyValueConfig);

        },
        m_EnableMaskOptions));


    m_UIElements.emplace_back(pUISection->RegisterUIElement<UISlider<float>>(
        "Frame Pacing safetyMarginInMs",
        m_SafetyMarginInMs,
        0.0f, 1.0f,
        m_FrameInterpolation,
        [this](float, float) {
#if defined(FFX_API_DX12)
            ffx::ConfigureDescFrameGenerationSwapChainKeyValueDX12 m_swapchainKeyValueConfig{};
#elif defined(FFX_API_VK)
            ffx::ConfigureDescFrameGenerationSwapChainKeyValueVK m_swapchainKeyValueConfig{};
#endif
            m_swapchainKeyValueConfig.key = FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING;
            m_swapchainKeyValueConfig.ptr = &framePacingTuning;

            framePacingTuning.safetyMarginInMs = m_SafetyMarginInMs;

            ffx::Configure(m_SwapChainContext, m_swapchainKeyValueConfig);
        },
        m_FrameInterpolation));
    m_UIElements.emplace_back(pUISection->RegisterUIElement<UISlider<float>>(
        "Frame Pacing varianceFactor",
        m_VarianceFactor,
        0.0f, 1.0f,
        m_FrameInterpolation,
        [this](float, float) {
#if defined(FFX_API_DX12)
            ffx::ConfigureDescFrameGenerationSwapChainKeyValueDX12 m_swapchainKeyValueConfig{};
#elif defined(FFX_API_VK)
            ffx::ConfigureDescFrameGenerationSwapChainKeyValueVK m_swapchainKeyValueConfig{};
#endif
            m_swapchainKeyValueConfig.key = FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING;
            m_swapchainKeyValueConfig.ptr = &framePacingTuning;

            framePacingTuning.varianceFactor = m_VarianceFactor;

            ffx::Configure(m_SwapChainContext, m_swapchainKeyValueConfig);
        },
        m_FrameInterpolation));
    m_UIElements.emplace_back(pUISection->RegisterUIElement<UICheckBox>(
        "Frame Pacing allowHybridSpin", 
        m_AllowHybridSpin, 
        m_FrameInterpolation,
        [this](bool, bool) {
#if defined(FFX_API_DX12)
            ffx::ConfigureDescFrameGenerationSwapChainKeyValueDX12 m_swapchainKeyValueConfig{};
#elif defined(FFX_API_VK)
            ffx::ConfigureDescFrameGenerationSwapChainKeyValueVK m_swapchainKeyValueConfig{};
#endif
            m_swapchainKeyValueConfig.key = FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING;
            m_swapchainKeyValueConfig.ptr = &framePacingTuning;

            framePacingTuning.allowHybridSpin = m_AllowHybridSpin;

            ffx::Configure(m_SwapChainContext, m_swapchainKeyValueConfig);
        }));
    m_UIElements.emplace_back(pUISection->RegisterUIElement<UISlider<int32_t>>(
        "hybridSpinTime in timer resolution units",
        (int32_t&) m_HybridSpinTime,
        0, 10,
        m_FrameInterpolation,
        [this](int32_t, int32_t) {
#if defined(FFX_API_DX12)
            ffx::ConfigureDescFrameGenerationSwapChainKeyValueDX12 m_swapchainKeyValueConfig{};
#elif defined(FFX_API_VK)
            ffx::ConfigureDescFrameGenerationSwapChainKeyValueVK m_swapchainKeyValueConfig{};
#endif
            m_swapchainKeyValueConfig.key = FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING;
            m_swapchainKeyValueConfig.ptr = &framePacingTuning;

            framePacingTuning.hybridSpinTime = m_HybridSpinTime;

            ffx::Configure(m_SwapChainContext, m_swapchainKeyValueConfig);
        },
        m_FrameInterpolation));
    m_UIElements.emplace_back(pUISection->RegisterUIElement<UICheckBox>(
        "allowWaitForSingleObjectOnFence",
        m_AllowWaitForSingleObjectOnFence,
        m_FrameInterpolation,
        [this](bool, bool) {
#if defined(FFX_API_DX12)
            ffx::ConfigureDescFrameGenerationSwapChainKeyValueDX12 m_swapchainKeyValueConfig{};
#elif defined(FFX_API_VK)
            ffx::ConfigureDescFrameGenerationSwapChainKeyValueVK m_swapchainKeyValueConfig{};
#endif
            m_swapchainKeyValueConfig.key = FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING;
            m_swapchainKeyValueConfig.ptr = &framePacingTuning;

            framePacingTuning.allowWaitForSingleObjectOnFence = m_AllowWaitForSingleObjectOnFence;

            ffx::Configure(m_SwapChainContext, m_swapchainKeyValueConfig);
        }));
    
    /// This InitUI will becalled before SwitchUpscaler(), which will call both
    /// EnableModule(false) and EnableModule(true). Thus it's unnecessary to call with false here.
    //EnableModule(true);
}

bool FSRRenderModule::ExportDebugFrame(const FfxApiResource& debugResource, const size_t skipN, std::string customName)
{
    size_t      frameID     = m_FrameID;

    /// When calling at the end of Execute(), we skip frames to align with actual displayed frame.
    /// For FG, this is 3
    /// Frame 0 and 1 are empty;
    /// Frame 2 are not-yet interpolated real frame 0
    /// 
    /// For SR (before FG), this is 1
    /// 
    /// For inputs (actual API resource to bind inputs, instead of our Debug resources), no skip
    ///  
    /// hackOptions.storeOutput should be checked before calling
    std::string suffix = [skipN]() { 
        if (skipN == m_kSkipFramesInput)
            return "input";
        else if (skipN == m_kSkipFramesSR)
            return "sr";
        else if (skipN == m_kSkipFramesFG)
            return "fg";
        else
            return "WRONG";
    }();

    size_t outputCount = hackOptions.enableHack ? hackOptions.outputMaxCount : 15;
    
    if (frameID < skipN + outputCount || frameID >= 2 * outputCount + skipN)
    //if (frameID >= outputCount)
        return true;
    frameID -= skipN + outputCount;

    const ResourceState resourceState = SDKWrapper::GetFrameworkState(static_cast<FfxResourceStates>(debugResource.state));
    const TextureDesc   textureDesc   = SDKWrapper::GetFrameworkTextureDescription(debugResource.description);
    GPUResource*        resource      = GPUResource::GetWrappedResourceFromSDK(
        StringToWString(customName).c_str(), debugResource.resource, &textureDesc, resourceState);
    
    std::filesystem::path outputPath(hackOptions.outPath != L"" ? hackOptions.outPath : L"../media/TEST_SCENE/outputs");
    // construct the full filename as <output_dir>/<identifier>_<frame_id formatted to 3 digits>.exr
    std::string idString = std::to_string(frameID);
    std::string filename = 
        (customName == "" ? hackOptions.identifier : customName) + "_" + 
        std::string(3 - idString.length(), '0') + idString + suffix + ".exr";
    outputPath.append(filename);
    // Adapted from SwapChain::DumpAllToFile()
    {
        D3D12_RESOURCE_DESC fromDesc = resource->GetImpl()->DX12Desc();

        CD3DX12_HEAP_PROPERTIES readBackHeapProperties(D3D12_HEAP_TYPE_READBACK);

        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Alignment           = 0;
        bufferDesc.DepthOrArraySize    = 1;
        bufferDesc.Dimension           = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Flags               = D3D12_RESOURCE_FLAG_NONE;
        bufferDesc.Format              = DXGI_FORMAT_UNKNOWN;
        bufferDesc.Height              = 1;
        bufferDesc.Width               = fromDesc.Width * fromDesc.Height * GetResourceFormatStride(textureDesc.Format);
        bufferDesc.Layout              = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufferDesc.MipLevels           = 1;
        bufferDesc.SampleDesc.Count    = 1;
        bufferDesc.SampleDesc.Quality  = 0;

        ID3D12Resource* pResourceReadBack = nullptr;
        GetDevice()->GetImpl()->DX12Device()->CreateCommittedResource(
            &readBackHeapProperties, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&pResourceReadBack));

        CommandList* pCmdList = GetDevice()->CreateCommandList(L"DebugFrameToFileCL", CommandQueue::Graphics);
        
        // Only transition if not already in CopySource state
        if (resourceState != ResourceState::CopySource) {
            Barrier barrier = Barrier::Transition(resource, resourceState, ResourceState::CopySource);
            ResourceBarrier(pCmdList, 1, &barrier);
        }

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout[1]             = {0};
        uint32_t                           num_rows[1]           = {0};
        UINT64                             row_sizes_in_bytes[1] = {0};
        UINT64                             uploadHeapSize        = 0;
        GetDevice()->GetImpl()->DX12Device()->GetCopyableFootprints(&fromDesc, 0, 1, 0, layout, num_rows, row_sizes_in_bytes, &uploadHeapSize);

        CD3DX12_TEXTURE_COPY_LOCATION copyDest(pResourceReadBack, layout[0]);
        CD3DX12_TEXTURE_COPY_LOCATION copySrc(resource->GetImpl()->DX12Resource(), 0);
        pCmdList->GetImpl()->DX12CmdList()->CopyTextureRegion(&copyDest, 0, 0, 0, &copySrc, nullptr);

        ID3D12Fence* pFence;
        CauldronThrowOnFail(GetDevice()->GetImpl()->DX12Device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&pFence)));
        CauldronThrowOnFail(GetDevice()->GetImpl()->DX12CmdQueue(CommandQueue::Graphics)->Signal(pFence, 1));
        CauldronThrowOnFail(pCmdList->GetImpl()->DX12CmdList()->Close());

        ID3D12CommandList* CmdListList[] = {pCmdList->GetImpl()->DX12CmdList()};
        GetDevice()->GetImpl()->DX12CmdQueue(CommandQueue::Graphics)->ExecuteCommandLists(1, CmdListList);

        // Wait for fence
        HANDLE mHandleFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        pFence->SetEventOnCompletion(1, mHandleFenceEvent);
        WaitForSingleObject(mHandleFenceEvent, INFINITE);
        CloseHandle(mHandleFenceEvent);
        pFence->Release();

        UINT64*     pData = NULL;
        D3D12_RANGE range;
        range.Begin = 0;
        range.End   = uploadHeapSize;
        CauldronThrowOnFail(pResourceReadBack->Map(0, &range, reinterpret_cast<void**>(&pData)));
        ExportInfo info = {
            textureDesc.Width, textureDesc.Height, layout[0].Footprint.RowPitch, 
            NumChannelsFromFormat(textureDesc.Format),
            customName == "" ? hackOptions.identifier : customName,
            outputPath.string()
        };
        UpdateExportInfo(info);
        bool success = SRV_debug::LogValues(pData, info.unpackMode, frameID);
        // Short circuit export if only logging values.
        success = success || DispatchTemplateExport(textureDesc.Format, pData, info);
        CauldronAssert(ASSERT_CRITICAL, success, L"Failed to export FSR frame to %ls", outputPath.c_str());

        pResourceReadBack->Unmap(0, NULL);

        GetDevice()->FlushAllCommandQueues();

        // Release
        pResourceReadBack->Release();
        pResourceReadBack = nullptr;
        //delete pCmdList;

        // Only transition back if we originally transitioned it
        if (resourceState != ResourceState::CopySource) {
            Barrier barrierBack = Barrier::Transition(resource, ResourceState::CopySource, resourceState);
            ResourceBarrier(pCmdList, 1, &barrierBack);
        }
        delete pCmdList;
    }


    delete resource;
    return true;
}

bool FSRRenderModule::ExportDebugFrame2Inputs(
    const FfxApiResource& debugResource1, 
    const FfxApiResource& debugResource2, 
    const size_t skipN, 
    std::string customName)
{
    size_t      frameID     = GetFramework()->GetFrameID();

    /// When calling at the end of Execute(), we skip frames to align with actual displayed frame.
    /// For FG, this is 3
    /// Frame 0 and 1 are empty;
    /// Frame 2 are not-yet interpolated real frame 0
    ///
    /// For SR (before FG), this is 1
    ///
    /// For inputs (actual API resource to bind inputs, instead of our Debug resources), no skip
    ///
    /// hackOptions.storeOutput should be checked before calling
    std::string suffix = [skipN]() {
        if (skipN == m_kSkipFramesInput)
            return "input";
        else if (skipN == m_kSkipFramesSR)
            return "sr";
        else if (skipN == m_kSkipFramesFG)
            return "fg";
        else
            return "WRONG";
    }();

    size_t outputCount = hackOptions.enableHack ? hackOptions.outputMaxCount : 15;

    if (frameID < skipN + outputCount || frameID >= 2 * outputCount + skipN)
        return true;
    frameID -= skipN + outputCount;

    // Get both resources
    const ResourceState resourceState1 = SDKWrapper::GetFrameworkState(static_cast<FfxResourceStates>(debugResource1.state));
    const TextureDesc   textureDesc1   = SDKWrapper::GetFrameworkTextureDescription(debugResource1.description);
    GPUResource*        resource1      = GPUResource::GetWrappedResourceFromSDK(L"debugResource1", debugResource1.resource, &textureDesc1, resourceState1);

    const ResourceState resourceState2 = SDKWrapper::GetFrameworkState(static_cast<FfxResourceStates>(debugResource2.state));
    const TextureDesc   textureDesc2   = SDKWrapper::GetFrameworkTextureDescription(debugResource2.description);
    GPUResource*        resource2      = GPUResource::GetWrappedResourceFromSDK(L"debugResource2", debugResource2.resource, &textureDesc2, resourceState2);

    /// NEW: do a sanity check on number of original channels. For now we only support (1,1)
    /// After this check, most parameters are the same.
    uint32_t nCh = NumChannelsFromFormat(textureDesc1.Format);
    bool     sameFormat = (textureDesc1.Format == textureDesc2.Format) &&
                          (textureDesc1.getDim() == textureDesc2.getDim());    
    CauldronAssert(ASSERT_CRITICAL, (nCh == 1 && sameFormat), L"ExportDebugFrame2Inputs only supports (1，1) input format for now.");


    std::filesystem::path outputPath(hackOptions.enableHack ? hackOptions.outPath : L"../media/TEST_SCENE/outputs");
    // construct the full filename as <output_dir>/<identifier>_<frame_id formatted to 3 digits>.exr
    std::string idString = std::to_string(frameID);
    std::string filename =
        (customName == "" ? hackOptions.identifier : customName) + "_" + std::string(3 - idString.length(), '0') + idString + suffix + ".exr";
    outputPath.append(filename);

    // Adapted from SwapChain::DumpAllToFile()
    {
        D3D12_RESOURCE_DESC fromDesc1 = resource1->GetImpl()->DX12Desc();
        D3D12_RESOURCE_DESC fromDesc2 = resource2->GetImpl()->DX12Desc();

        CD3DX12_HEAP_PROPERTIES readBackHeapProperties(D3D12_HEAP_TYPE_READBACK);

        // Create readback buffer for MV
        D3D12_RESOURCE_DESC bufferDesc1 = {};
        bufferDesc1.Alignment           = 0;
        bufferDesc1.DepthOrArraySize    = 1;
        bufferDesc1.Dimension           = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc1.Flags               = D3D12_RESOURCE_FLAG_NONE;
        bufferDesc1.Format              = DXGI_FORMAT_UNKNOWN;
        bufferDesc1.Height              = 1;
        bufferDesc1.Width               = fromDesc1.Width * fromDesc1.Height * GetResourceFormatStride(textureDesc1.Format);
        bufferDesc1.Layout              = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufferDesc1.MipLevels           = 1;
        bufferDesc1.SampleDesc.Count    = 1;
        bufferDesc1.SampleDesc.Quality  = 0;

        ID3D12Resource* pResourceReadBack1 = nullptr;
        GetDevice()->GetImpl()->DX12Device()->CreateCommittedResource(
            &readBackHeapProperties, D3D12_HEAP_FLAG_NONE, &bufferDesc1, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&pResourceReadBack1));

        // Create readback buffer for Depth
        D3D12_RESOURCE_DESC bufferDesc2 = {};
        bufferDesc2.Alignment           = 0;
        bufferDesc2.DepthOrArraySize    = 1;
        bufferDesc2.Dimension           = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc2.Flags               = D3D12_RESOURCE_FLAG_NONE;
        bufferDesc2.Format              = DXGI_FORMAT_UNKNOWN;
        bufferDesc2.Height              = 1;
        bufferDesc2.Width               = fromDesc2.Width * fromDesc2.Height * GetResourceFormatStride(textureDesc2.Format);
        bufferDesc2.Layout              = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufferDesc2.MipLevels           = 1;
        bufferDesc2.SampleDesc.Count    = 1;
        bufferDesc2.SampleDesc.Quality  = 0;

        ID3D12Resource* pResourceReadBack2 = nullptr;
        GetDevice()->GetImpl()->DX12Device()->CreateCommittedResource(
            &readBackHeapProperties, D3D12_HEAP_FLAG_NONE, &bufferDesc2, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&pResourceReadBack2));

        CommandList* pCmdList = GetDevice()->CreateCommandList(L"DebugFrame2InputsToFileCL", CommandQueue::Graphics);

        // Transition both resources to CopySource
        Barrier barriers[2] = {Barrier::Transition(resource1, resourceState1, ResourceState::CopySource),
                               Barrier::Transition(resource2, resourceState2, ResourceState::CopySource)};
        ResourceBarrier(pCmdList, 2, barriers);

        // Copy MV resource
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout1       = {0};
        uint32_t                           numRows1        = 0;
        UINT64                             rowSizeInBytes1 = 0;
        UINT64                             uploadHeapSize1 = 0;
        GetDevice()->GetImpl()->DX12Device()->GetCopyableFootprints(&fromDesc1, 0, 1, 0, &layout1, &numRows1, &rowSizeInBytes1, &uploadHeapSize1);

        CD3DX12_TEXTURE_COPY_LOCATION copyDest1(pResourceReadBack1, layout1);
        CD3DX12_TEXTURE_COPY_LOCATION copySrc1(resource1->GetImpl()->DX12Resource(), 0);
        pCmdList->GetImpl()->DX12CmdList()->CopyTextureRegion(&copyDest1, 0, 0, 0, &copySrc1, nullptr);

        // Copy Depth resource
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout2         = {0};
        uint32_t                           numRows2        = 0;
        UINT64                             rowSizeInBytes2 = 0;
        UINT64                             uploadHeapSize2 = 0;
        GetDevice()->GetImpl()->DX12Device()->GetCopyableFootprints(
            &fromDesc2, 0, 1, 0, &layout2, &numRows2, &rowSizeInBytes2, &uploadHeapSize2);

        CD3DX12_TEXTURE_COPY_LOCATION copyDest2(pResourceReadBack2, layout2);
        CD3DX12_TEXTURE_COPY_LOCATION copySrc2(resource2->GetImpl()->DX12Resource(), 0);
        pCmdList->GetImpl()->DX12CmdList()->CopyTextureRegion(&copyDest2, 0, 0, 0, &copySrc2, nullptr);

        // Transition both resources back to their original states
        Barrier barriersBack[2] = {Barrier::Transition(resource1, ResourceState::CopySource, resourceState1),
                                   Barrier::Transition(resource2, ResourceState::CopySource, resourceState2)};
        ResourceBarrier(pCmdList, 1, barriersBack);

        ID3D12Fence* pFence;
        CauldronThrowOnFail(GetDevice()->GetImpl()->DX12Device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&pFence)));
        CauldronThrowOnFail(GetDevice()->GetImpl()->DX12CmdQueue(CommandQueue::Graphics)->Signal(pFence, 1));
        CauldronThrowOnFail(pCmdList->GetImpl()->DX12CmdList()->Close());

        ID3D12CommandList* CmdListList[] = {pCmdList->GetImpl()->DX12CmdList()};
        GetDevice()->GetImpl()->DX12CmdQueue(CommandQueue::Graphics)->ExecuteCommandLists(1, CmdListList);

        // Wait for fence
        HANDLE mHandleFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        pFence->SetEventOnCompletion(1, mHandleFenceEvent);
        WaitForSingleObject(mHandleFenceEvent, INFINITE);
        CloseHandle(mHandleFenceEvent);
        pFence->Release();

        // Map both buffers and save to EXR
        UINT64* pData1 = nullptr;
        UINT64* pData2 = nullptr;

        D3D12_RANGE range1 = {0, uploadHeapSize1};
        pResourceReadBack1->Map(0, &range1, reinterpret_cast<void**>(&pData1));

        D3D12_RANGE range2 = {0, uploadHeapSize2};
        pResourceReadBack2->Map(0, &range2, reinterpret_cast<void**>(&pData2));

        // Blend the data to {X0, Y0, X1, Y1, ...}
        uint32_t             elementSize = 0;
        ResourceFormat       blendedFormat(ResourceFormat::Unknown);
        if (textureDesc1.Format == ResourceFormat::R16_FLOAT) {
            elementSize = 2;  // uint16_t
            blendedFormat = ResourceFormat::RG16_FLOAT;
        }
        else if (textureDesc1.Format == ResourceFormat::R32_FLOAT ||
                 textureDesc1.Format == ResourceFormat::R32_UINT) {
            elementSize = 4;  // float
            blendedFormat = ResourceFormat::RG32_FLOAT;
        }
        else
        {
            CauldronError(L"ExportDebugFrame2Inputs only supports R16_FLOAT, R32_UINT, and R32_FLOAT for now.");
            return false;
        }
        std::vector<uint8_t> blendedData(textureDesc1.Width * textureDesc1.Height * 2 * elementSize);
        {
            const uint8_t* data1Bytes   = reinterpret_cast<const uint8_t*>(pData1);
            const uint8_t* data2Bytes   = reinterpret_cast<const uint8_t*>(pData2);
            uint8_t*       blendedBytes = blendedData.data();

            for (size_t y = 0; y < textureDesc1.Height; y++)
            {
                const uint8_t* srcRowX = data1Bytes + y * layout1.Footprint.RowPitch;
                const uint8_t* srcRowY = data2Bytes + y * layout1.Footprint.RowPitch;
                uint8_t*       dstRow  = blendedBytes + y * (textureDesc1.Width * 2 * elementSize);

                for (size_t x = 0; x < textureDesc1.Width; x++)
                {
                    // Copy X component
                    memcpy(dstRow + (x * 2) * elementSize, srcRowX + x * elementSize, elementSize);
                    // Copy Y component
                    memcpy(dstRow + (x * 2 + 1) * elementSize, srcRowY + x * elementSize, elementSize);
                }
            }
        }

        ExportInfo info = {
            textureDesc1.Width, textureDesc1.Height, 
            textureDesc1.Width * 2 * elementSize, 
            NumChannelsFromFormat(textureDesc1.Format) * 2,
            customName == "" ? hackOptions.identifier : customName,
            outputPath.string()
        };
        UpdateExportInfo(info);

        bool success = DispatchTemplateExport(blendedFormat, blendedData.data(), info);
        CauldronAssert(ASSERT_CRITICAL, success, L"Failed to export FSR frame to %ls", outputPath.c_str());


        pResourceReadBack1->Unmap(0, NULL);
        pResourceReadBack2->Unmap(0, NULL);

        GetDevice()->FlushAllCommandQueues();

        // Release resources
        pResourceReadBack1->Release();
        pResourceReadBack2->Release();
        delete pCmdList;
    }

    delete resource1;
    delete resource2;
    return true;
}

void FSRRenderModule::UpdateExportInfo(ExportInfo& info) {
    using namespace SRV_debug;
    if (Uint32MVs.find(info.name) != Uint32MVs.end())
    {
        info.unpackMode = ExportInfo::BitUnpackMode::MV_Uint32;
        CauldronAssert(ASSERT_CRITICAL, info.numSourceChannels == 2, L"MV_Uint32 export requires 2 channels.");
    }
    else if (FP16MVs.find(info.name) != FP16MVs.end())
    {
        info.unpackMode = ExportInfo::BitUnpackMode::MV_Remap;
        CauldronAssert(ASSERT_CRITICAL, info.numSourceChannels == 2, L"MV_Remap export requires 2 channels.");
    }
    else if (info.name == OFlowSCD.first)
    {
        info.unpackMode = ExportInfo::BitUnpackMode::LogSCD;
    }
    else if (info.name == InputDF.first)
    {
        info.unpackMode = ExportInfo::BitUnpackMode::LogDF;
    }
}

void FSRRenderModule::SwitchUpscaler(int32_t newUpscaler)
{
    // Flush everything out of the pipe before disabling/enabling things
    GetDevice()->FlushAllCommandQueues();

    if (ModuleEnabled())
        EnableModule(false);

    // 0 = native, 1 = FFXAPI
    SetFilter(newUpscaler);
    switch (newUpscaler)
    {
    case 0:
        m_pTAARenderModule->EnableModule(false);
        m_pToneMappingRenderModule->EnableModule(true);
        m_EnableMaskOptions = false;
        break;
    case 1:
        ClearReInit();
        // Also disable TAA render module
        m_pTAARenderModule->EnableModule(false);
        m_pToneMappingRenderModule->EnableModule(true);
        m_EnableMaskOptions = true;
        break;
    default:
        CauldronCritical(L"Unsupported upscaler requested.");
        break;
    }

    m_EnableWaitCallbackModeUI = m_EnableMaskOptions && m_FrameInterpolationAvailable;

    m_UpscaleMethod = newUpscaler;

    // Enable the new one
    EnableModule(true);
    ClearReInit();
}

void FSRRenderModule::UpdatePreset(const int32_t* pOldPreset)
{
    switch (m_ScalePreset)
    {
    case FSRScalePreset::NativeAA:
        m_UpscaleRatio = 1.0f;
        break;
    case FSRScalePreset::Quality:
        m_UpscaleRatio = 1.5f;
        break;
    case FSRScalePreset::Balanced:
        m_UpscaleRatio = 1.7f;
        break;
    case FSRScalePreset::Performance:
        m_UpscaleRatio = 2.0f;
        break;
    case FSRScalePreset::UltraPerformance:
        m_UpscaleRatio = 3.0f;
        break;
    case FSRScalePreset::Custom:
        // TODO: if hack mode, compute m_UpscaleRatio to be display / render
        if (hackOptions.enableHack)
        {
            m_UpscaleRatio  = GetFramework()->GetResolutionInfo().GetDisplayWidthScaleRatio();
            break;
        }
    default:
        // Leave the upscale ratio at whatever it was
        break;
    }

    // Update whether we can update the custom scale slider
    m_UpscaleRatioEnabled = (m_ScalePreset == FSRScalePreset::Custom);

    // Update mip bias
    float oldValue = m_MipBias;
    if (m_ScalePreset != FSRScalePreset::Custom)
        m_MipBias = cMipBias[static_cast<uint32_t>(m_ScalePreset)];
    else
        m_MipBias = m_MipBias = CalculateMipBias(m_UpscaleRatio);
    UpdateMipBias(&oldValue);

    // Update resolution since rendering ratios have changed
    GetFramework()->EnableUpscaling(true, m_pUpdateFunc);

    GetFramework()->EnableFrameInterpolation(m_FrameInterpolation);
}

void FSRRenderModule::UpdateUpscaleRatio(const float* pOldRatio)
{
    // Disable/Enable FSR since resolution ratios have changed
    GetFramework()->EnableUpscaling(true, m_pUpdateFunc);
}

void FSRRenderModule::UpdateMipBias(const float* pOldBias)
{
    // Update the scene MipLODBias to use
    GetScene()->SetMipLODBias(m_MipBias);
}

void FSRRenderModule::FfxMsgCallback(uint32_t type, const wchar_t* message)
{
    if (type == FFX_API_MESSAGE_TYPE_ERROR)
    {
        CauldronError(L"FSR_API_DEBUG_ERROR: %ls", message);
    }
    else if (type == FFX_API_MESSAGE_TYPE_WARNING)
    {
        CauldronWarning(L"FSR_API_DEBUG_WARNING: %ls", message);
    }
}

ffxReturnCode_t FSRRenderModule::UiCompositionCallback(ffxCallbackDescFrameGenerationPresent* params)
{
    if (s_uiRenderMode != 2)
        return FFX_API_RETURN_ERROR_PARAMETER;

    // Get a handle to the UIRenderModule for UI composition (only do this once as there's a search cost involved)
    if (!m_pUIRenderModule)
    {
        m_pUIRenderModule = static_cast<UIRenderModule*>(GetFramework()->GetRenderModule("UIRenderModule"));
        CauldronAssert(ASSERT_CRITICAL, m_pUIRenderModule, L"Could not get a handle to the UIRenderModule.");
    }
    
    // Wrap everything in cauldron wrappers to allow backend agnostic execution of UI render.
    CommandList* pCmdList = CommandList::GetWrappedCmdListFromSDK(L"UI_CommandList", CommandQueue::Graphics, params->commandList);
    SetAllResourceViewHeaps(pCmdList); // Set the resource view heaps to the wrapped command list

    ResourceState rtResourceState = SDKWrapper::GetFrameworkState((FfxResourceStates)params->outputSwapChainBuffer.state);
    ResourceState bbResourceState = SDKWrapper::GetFrameworkState((FfxResourceStates)params->currentBackBuffer.state);

    TextureDesc rtDesc = SDKWrapper::GetFrameworkTextureDescription(params->outputSwapChainBuffer.description);
    TextureDesc bbDesc = SDKWrapper::GetFrameworkTextureDescription(params->currentBackBuffer.description);

    GPUResource* pRTResource = GPUResource::GetWrappedResourceFromSDK(L"UI_RenderTarget", params->outputSwapChainBuffer.resource, &rtDesc, rtResourceState);
    GPUResource* pBBResource = GPUResource::GetWrappedResourceFromSDK(L"BackBuffer", params->currentBackBuffer.resource, &bbDesc, bbResourceState);
    
    std::vector<Barrier> barriers;
    barriers = {Barrier::Transition(pRTResource, rtResourceState, ResourceState::CopyDest),
                Barrier::Transition(pBBResource, bbResourceState, ResourceState::CopySource)};
    ResourceBarrier(pCmdList, (uint32_t)barriers.size(), barriers.data());
        
    TextureCopyDesc copyDesc(pBBResource, pRTResource);
    CopyTextureRegion(pCmdList, &copyDesc);
    
    barriers[0].SourceState = barriers[0].DestState;
    barriers[0].DestState   = ResourceState::RenderTargetResource;
    swap(barriers[1].SourceState, barriers[1].DestState);
    ResourceBarrier(pCmdList, (uint32_t)barriers.size(), barriers.data());

    // Create and set RTV, required for async UI render.
    FfxApiResourceDescription rdesc = params->outputSwapChainBuffer.description;

    TextureDesc rtResourceDesc = TextureDesc::Tex2D(L"UI_RenderTarget",
                                                    SDKWrapper::GetFrameworkSurfaceFormat((FfxSurfaceFormat)rdesc.format),
                                                    rdesc.width,
                                                    rdesc.height,
                                                    rdesc.depth,
                                                    rdesc.mipCount,
                                                    ResourceFlags::AllowRenderTarget);
    m_pRTResourceView->BindTextureResource(pRTResource, rtResourceDesc, ResourceViewType::RTV, ViewDimension::Texture2D, 0, 1, 0);
    
    m_pUIRenderModule->ExecuteAsync(pCmdList, &m_pRTResourceView->GetViewInfo(0));

    ResourceBarrier(pCmdList, 1, &Barrier::Transition(pRTResource, ResourceState::RenderTargetResource, rtResourceState));

    // Clean up wrapped resources for the frame
    delete pBBResource;
    delete pRTResource;
    delete pCmdList;

    return FFX_API_RETURN_OK;
}

void FSRRenderModule::UpdateFSRContext(bool enabled)
{
    if (enabled)
    {
        const ResolutionInfo& resInfo = GetFramework()->GetResolutionInfo();
        static bool s_InvertedDepth = GetConfig()->InvertedDepth;
        // Backend creation (for both FFXAPI contexts, FG and Upscale)
#if defined(FFX_API_DX12)
        ffx::CreateBackendDX12Desc backendDesc{};
        backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backendDesc.device = GetDevice()->GetImpl()->DX12Device();
#elif defined(FFX_API_VK)
        DeviceInternal *device = GetDevice()->GetImpl();
        ffx::CreateBackendVKDesc backendDesc{};
        backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        backendDesc.vkDevice = device->VKDevice();
        backendDesc.vkPhysicalDevice = device->VKPhysicalDevice();
        backendDesc.vkDeviceProcAddr = vkGetDeviceProcAddr;
#endif  // defined(FFX_API_DX12)

        if (m_UpscaleMethod == Upscaler_FSRAPI)
        {
            ffx::CreateContextDescUpscale createFsr{};
            
            createFsr.maxUpscaleSize = {resInfo.DisplayWidth, resInfo.DisplayHeight};
            createFsr.maxRenderSize  = {resInfo.DisplayWidth, resInfo.DisplayHeight};
            createFsr.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
            if (s_InvertedDepth)
            {
                createFsr.flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
            }
            createFsr.flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
            createFsr.fpMessage = nullptr;

            if (m_GlobalDebugCheckerMode != FSRDebugCheckerMode::Disabled)
            {
                createFsr.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
            }

            // Create the FSR context
            {
                ffx::ReturnCode retCode;
                // lifetime of this must last until after CreateContext call!
                ffx::CreateContextDescOverrideVersion versionOverride{};
                if (m_FsrVersionIndex < m_FsrVersionIds.size() && m_overrideVersion)
                {
                    versionOverride.versionId = m_FsrVersionIds[m_FsrVersionIndex];
                    retCode = ffx::CreateContext(m_UpscalingContext, nullptr, createFsr, backendDesc, versionOverride);
                }
                else
                {
                    retCode = ffx::CreateContext(m_UpscalingContext, nullptr, createFsr, backendDesc);
                }

                CauldronAssert(ASSERT_CRITICAL, retCode == ffx::ReturnCode::Ok, L"Couldn't create the ffxapi upscaling context: %d", (uint32_t)retCode);
            }
            
            //Query created version
            ffxQueryGetProviderVersion getVersion = {0};
            getVersion.header.type                = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;

            ffx::Query(m_UpscalingContext, getVersion);
            m_currentUpscaleContextVersionId = getVersion.versionId;
            m_currentUpscaleContextVersionName = getVersion.versionName;

            CAUDRON_LOG_INFO(L"Upscaler Context versionid 0x%016llx, %S", m_currentUpscaleContextVersionId, m_currentUpscaleContextVersionName);

            for (uint32_t i = 0; i < m_FsrVersionIds.size(); i++)
            {
                if (m_FsrVersionIds[i] == m_currentUpscaleContextVersionId)
                {
                    m_FsrVersionIndex = i;
                }
            }

            FfxApiEffectMemoryUsage gpuMemoryUsageUpscaler;
            ffx::QueryDescUpscaleGetGPUMemoryUsage upscalerGetGPUMemoryUsage{};
            upscalerGetGPUMemoryUsage.gpuMemoryUsageUpscaler = &gpuMemoryUsageUpscaler;

            ffx::Query(m_UpscalingContext, upscalerGetGPUMemoryUsage);

            CAUDRON_LOG_INFO(L"Upscaler Context VRAM totalUsageInBytes %f MB aliasableUsageInBytes %f MB", gpuMemoryUsageUpscaler.totalUsageInBytes / 1048576.f, gpuMemoryUsageUpscaler.aliasableUsageInBytes / 1048576.f);

        }

        // Create the FrameGen context
        if (m_FrameInterpolationAvailable)
        {
            ffx::CreateContextDescFrameGeneration createFg{};
            createFg.displaySize = { resInfo.DisplayWidth, resInfo.DisplayHeight };
            createFg.maxRenderSize = { resInfo.DisplayWidth, resInfo.DisplayHeight };
            if (s_InvertedDepth)
                createFg.flags |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED | FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;
            createFg.flags |= FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE;

            m_EnableAsyncCompute = m_PendingEnableAsyncCompute;
            if (m_EnableAsyncCompute)
            {
                createFg.flags |= FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT;
            }
            if (m_GlobalDebugCheckerMode != FSRDebugCheckerMode::Disabled)
            {
                createFg.flags |= FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;
            }

            createFg.backBufferFormat = SDKWrapper::GetFfxSurfaceFormat(GetFramework()->GetSwapChain()->GetSwapChainFormat());
            ffx::ReturnCode retCode;
            if (s_uiRenderMode == 3)
            {
                ffx::CreateContextDescFrameGenerationHudless createFgHudless{};
                createFgHudless.hudlessBackBufferFormat = SDKWrapper::GetFfxSurfaceFormat(m_pHudLessTexture[0]->GetResource()->GetTextureResource()->GetFormat());
                retCode = ffx::CreateContext(m_FrameGenContext, nullptr, createFg, backendDesc, createFgHudless);
            }
            else
            {
                retCode = ffx::CreateContext(m_FrameGenContext, nullptr, createFg, backendDesc);
            }

            CauldronAssert(ASSERT_CRITICAL, retCode == ffx::ReturnCode::Ok, L"Couldn't create the ffxapi framegen context: %d", (uint32_t)retCode);

            void* ffxSwapChain;
#if defined(FFX_API_DX12)
            ffxSwapChain = GetSwapChain()->GetImpl()->DX12SwapChain();
#elif defined(FFX_API_VK)
            ffxSwapChain = GetSwapChain()->GetImpl()->VKSwapChain();
#endif  // defined(FFX_API_DX12)

            // Configure frame generation
            FfxApiResource hudLessResource = SDKWrapper::ffxGetResourceApi(m_pHudLessTexture[m_curUiTextureIndex]->GetResource(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);

            m_FrameGenerationConfig.frameGenerationEnabled  = false;
            m_FrameGenerationConfig.frameGenerationCallback = [](ffxDispatchDescFrameGeneration* params, void* pUserCtx) -> ffxReturnCode_t
            {
                return ffxDispatch(reinterpret_cast<ffxContext*>(pUserCtx), &params->header);
            };
            m_FrameGenerationConfig.frameGenerationCallbackUserContext = &m_FrameGenContext;
            if (s_uiRenderMode == 2)
            {
                m_FrameGenerationConfig.presentCallback = [](ffxCallbackDescFrameGenerationPresent* params, void* self) -> auto { return reinterpret_cast<FSRRenderModule*>(self)->UiCompositionCallback(params); };
                m_FrameGenerationConfig.presentCallbackUserContext = this;
            }
            else
            {
                m_FrameGenerationConfig.presentCallback = nullptr;
                m_FrameGenerationConfig.presentCallbackUserContext = nullptr;
            }
            m_FrameGenerationConfig.swapChain               = ffxSwapChain;
            m_FrameGenerationConfig.HUDLessColor            = (s_uiRenderMode == 3) ? hudLessResource : FfxApiResource({});

            m_FrameGenerationConfig.frameID = m_FrameID;

            retCode = ffx::Configure(m_FrameGenContext, m_FrameGenerationConfig);
            CauldronAssert(ASSERT_CRITICAL, retCode == ffx::ReturnCode::Ok, L"Couldn't create the ffxapi upscaling context: %d", (uint32_t)retCode);
            
            FfxApiEffectMemoryUsage gpuMemoryUsageFrameGeneration;
            ffx::QueryDescFrameGenerationGetGPUMemoryUsage frameGenGetGPUMemoryUsage{};
            frameGenGetGPUMemoryUsage.gpuMemoryUsageFrameGeneration = &gpuMemoryUsageFrameGeneration;
            ffx::Query(m_FrameGenContext, frameGenGetGPUMemoryUsage);

            CAUDRON_LOG_INFO(L"FrameGeneration Context VRAM totalUsageInBytes %f MB aliasableUsageInBytes %f MB", gpuMemoryUsageFrameGeneration.totalUsageInBytes / 1048576.f, gpuMemoryUsageFrameGeneration.aliasableUsageInBytes / 1048576.f);


            FfxApiEffectMemoryUsage gpuMemoryUsageFrameGenerationSwapchain;
#if defined(FFX_API_DX12)
            ffx::QueryFrameGenerationSwapChainGetGPUMemoryUsageDX12 frameGenSwapchainGetGPUMemoryUsage{};
            frameGenSwapchainGetGPUMemoryUsage.gpuMemoryUsageFrameGenerationSwapchain = &gpuMemoryUsageFrameGenerationSwapchain;
            ffx::Query(m_SwapChainContext, frameGenSwapchainGetGPUMemoryUsage);
#elif defined(FFX_API_VK)
            ffx::QueryFrameGenerationSwapChainGetGPUMemoryUsageVK frameGenSwapchainGetGPUMemoryUsage{};
            frameGenSwapchainGetGPUMemoryUsage.gpuMemoryUsageFrameGenerationSwapchain = &gpuMemoryUsageFrameGenerationSwapchain;
            ffx::Query(m_SwapChainContext, frameGenSwapchainGetGPUMemoryUsage);
#endif  // defined(FFX_API_DX12)
            CAUDRON_LOG_INFO(L"Swapchain Context VRAM totalUsageInBytes %f MB aliasableUsageInBytes %f MB", gpuMemoryUsageFrameGenerationSwapchain.totalUsageInBytes / 1048576.f, gpuMemoryUsageFrameGenerationSwapchain.aliasableUsageInBytes / 1048576.f);
        }
    }
    else
    {
        if (m_FrameInterpolationAvailable)
        {
            void* ffxSwapChain;
#if defined(FFX_API_DX12)
            ffxSwapChain = GetSwapChain()->GetImpl()->DX12SwapChain();
#elif defined(FFX_API_VK)
            ffxSwapChain = GetSwapChain()->GetImpl()->VKSwapChain();
#endif  // defined(FFX_API_DX12)

            // disable frame generation before destroying context
            // also unset present callback, HUDLessColor and UiTexture to have the swapchain only present the backbuffer
            m_FrameGenerationConfig.frameGenerationEnabled = false;
            m_FrameGenerationConfig.swapChain = ffxSwapChain;
            m_FrameGenerationConfig.presentCallback = nullptr;
            m_FrameGenerationConfig.HUDLessColor = FfxApiResource({});
            ffx::Configure(m_FrameGenContext, m_FrameGenerationConfig);

#if defined(FFX_API_DX12)
            ffx::ConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 uiConfig{};
            uiConfig.uiResource = {};
            uiConfig.flags = 0;
            ffx::Configure(m_SwapChainContext, uiConfig);
#elif defined(FFX_API_VK)
            ffx::ConfigureDescFrameGenerationSwapChainRegisterUiResourceVK uiConfig{};
            uiConfig.uiResource = {};
            ffx::Configure(m_SwapChainContext, uiConfig);
#endif  // defined(FFX_API_DX12)
            
            ffx::DestroyContext(m_FrameGenContext);
            m_FrameGenContext = nullptr;
        }

        if (m_UpscalingContext)
        {
            ffx::DestroyContext(m_UpscalingContext);
            m_UpscalingContext = nullptr;
        }
    }
    
}

void FSRRenderModule::SetUpscaleConstantBuffer(uint64_t key, float value)
{
    ffx::ConfigureDescUpscaleKeyValue m_upscalerKeyValueConfig{};
    m_upscalerKeyValueConfig.key = key;
    m_upscalerKeyValueConfig.ptr = &value;
    ffx::Configure(m_UpscalingContext, m_upscalerKeyValueConfig);
}

void FSRRenderModule::SetGlobalDebugCheckerMode(FSRDebugCheckerMode mode, bool recreate)
{
    if (m_UpscalingContext)
    {
        ffx::ConfigureDescGlobalDebug1 m_GlobalDebugConfig{};
        m_GlobalDebugConfig.debugLevel = 0; //not implemented. Value doesn't matter.
        if (mode == FSRDebugCheckerMode::Disabled || mode == FSRDebugCheckerMode::EnabledNoMessageCallback)
        {
            m_GlobalDebugConfig.fpMessage = nullptr;
        }
        else if (mode == FSRDebugCheckerMode::EnabledWithMessageCallback)
        {
            m_GlobalDebugConfig.fpMessage = &FSRRenderModule::FfxMsgCallback;
        }
        if (recreate)
        {
            // Ask main loop to re-initialize.
            m_NeedReInit = true;
        }

        ffx::ReturnCode retCode = ffx::Configure(m_UpscalingContext, m_GlobalDebugConfig);
        CauldronAssert(ASSERT_ERROR, retCode == ffx::ReturnCode::Ok, L"Couldn't configure global debug config on the ffxapi upscaling context: %d", (uint32_t)retCode);
    }
    if (m_FrameGenContext)
    {
        ffx::ConfigureDescGlobalDebug1 m_GlobalDebugConfig{};
        m_GlobalDebugConfig.debugLevel = 0; //not implemented. Value doesn't matter.
        if (mode == FSRDebugCheckerMode::Disabled || mode == FSRDebugCheckerMode::EnabledNoMessageCallback)
        {
            m_GlobalDebugConfig.fpMessage = nullptr;
        }
        else if (mode == FSRDebugCheckerMode::EnabledWithMessageCallback)
        {
            m_GlobalDebugConfig.fpMessage = &FSRRenderModule::FfxMsgCallback;
        }
        if (recreate)
        {
            // Ask main loop to re-initialize.
            m_NeedReInit = true;
        }

        ffx::ReturnCode retCode = ffx::Configure(m_FrameGenContext, m_GlobalDebugConfig);
        CauldronAssert(ASSERT_ERROR, retCode == ffx::ReturnCode::Ok, L"Couldn't configure global debug config on the ffxapi frame generation context: %d", (uint32_t)retCode);
    }
}

ResolutionInfo FSRRenderModule::UpdateResolution(uint32_t displayWidth, uint32_t displayHeight)
{
    return {static_cast<uint32_t>((float)displayWidth / m_UpscaleRatio * m_LetterboxRatio),
            static_cast<uint32_t>((float)displayHeight / m_UpscaleRatio * m_LetterboxRatio),
            static_cast<uint32_t>((float)displayWidth * m_LetterboxRatio),
            static_cast<uint32_t>((float)displayHeight * m_LetterboxRatio),
            displayWidth, displayHeight };
}

void FSRRenderModule::OnPreFrame()
{
    if (NeedsReInit())
    {
        GetDevice()->FlushAllCommandQueues();
        if (m_FrameInterpolationAvailable)
        {
#if defined(FFX_API_DX12)
            ffx::DispatchDescFrameGenerationSwapChainWaitForPresentsDX12 waitForPresentsDesc;
#elif defined(FFX_API_VK)
            ffx::DispatchDescFrameGenerationSwapChainWaitForPresentsVK waitForPresentsDesc;
#endif
            ffx::ReturnCode errorCode = ffx::Dispatch(m_SwapChainContext, waitForPresentsDesc);
            CauldronAssert(ASSERT_CRITICAL, errorCode == ffx::ReturnCode::Ok, L"OnPreFrame FrameGenerationSwapChain WaitForPresents failed: %d", (uint32_t)errorCode);
        }

        // Need to recreate the FSR context
        EnableModule(false);
        s_uiRenderMode = s_uiRenderModeNextFrame;
        EnableModule(true);

        ClearReInit();
    }
}

bool FSRRenderModule::DebugCheck(std::string marker)
{
    if (m_pHackColors.empty())
        return true;

    auto origFrameID = m_FrameID;
    m_FrameID         = 15;
    auto hackTexture  = m_pHackColors.front()->GetResource();
    auto currentState = hackTexture->GetCurrentResourceState();
    bool success = ExportDebugFrame(SDKWrapper::ffxGetResourceApi(hackTexture, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ), m_kSkipFramesInput, marker);
    m_FrameID = origFrameID;

    return success;
}

void FSRRenderModule::OnResize(const ResolutionInfo& resInfo)
{
    if (!ModuleEnabled())
        return;

    // Need to recreate the FSR context on resource resize
    UpdateFSRContext(false);   // Destroy
    UpdateFSRContext(true);    // Re-create

    // Reset jitter index
    m_JitterIndex = 0;
}

void FSRRenderModule::Execute(double deltaTime, CommandList* pCmdList)
{
    constexpr float CustomFPS = 1000.f / 60;
    constexpr float Radian90  = 1.578f;
    //std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    if (!ModuleReady())
        return;
    if (m_pHudLessTexture[m_curUiTextureIndex]->GetResource()->GetCurrentResourceState() != ResourceState::NonPixelShaderResource)
    {
        Barrier barrier = Barrier::Transition(m_pHudLessTexture[m_curUiTextureIndex]->GetResource(),
                                              m_pHudLessTexture[m_curUiTextureIndex]->GetResource()->GetCurrentResourceState(),
                                              ResourceState::NonPixelShaderResource);
        ResourceBarrier(pCmdList, 1, &barrier);
    }

    if (m_pUiTexture[m_curUiTextureIndex]->GetResource()->GetCurrentResourceState() != ResourceState::ShaderResource)
    {
        std::vector<Barrier> barriers = {Barrier::Transition(m_pUiTexture[m_curUiTextureIndex]->GetResource(),
                                                             m_pUiTexture[m_curUiTextureIndex]->GetResource()->GetCurrentResourceState(),
                                                             ResourceState::ShaderResource)};
        ResourceBarrier(pCmdList, (uint32_t)barriers.size(), barriers.data());
    }

    GPUScopedProfileCapture sampleMarker(pCmdList, L"FFX API FSR Upscaler");
    const ResolutionInfo&   resInfo = GetFramework()->GetResolutionInfo();
    CameraComponent*        pCamera = GetScene()->GetCurrentCamera();

    GPUResource* pSwapchainBackbuffer = GetFramework()->GetSwapChain()->GetBackBufferRT()->GetCurrentResource();
    FfxApiResource backbuffer            = SDKWrapper::ffxGetResourceApi(pSwapchainBackbuffer, FFX_API_RESOURCE_STATE_PRESENT);
    
    // copy input source to temp so that the input and output texture of the upscalers is different 
    auto hasSameSize = [](const TextureDesc& desc1, const TextureDesc& desc2) {
        return desc1.Width == desc2.Width && desc1.Height == desc2.Height;
    };
    const auto& colorDesc = m_pColorTarget->GetDesc();
    const auto& tempDesc  = m_pTempTexture->GetDesc();
    const auto& mvDesc    = m_pMotionVectors->GetDesc();
    const auto& depthDesc = m_pDepthTarget->GetDesc();
    if (hackOptions.enableHack)
    {
        const auto& hackColorDesc = m_pHackColors[0]->GetDesc();
        const auto& hackMVDesc    = m_pHackMVs[0]->GetDesc();
        const auto& hackDepthDesc = m_pHackDepths[0]->GetDesc();
        
        bool check = hasSameSize(colorDesc, hackColorDesc) && hasSameSize(mvDesc, hackMVDesc) && hasSameSize(depthDesc, hackDepthDesc);
    }
    {
        std::vector<Barrier> barriers;
        barriers.push_back(Barrier::Transition(
            m_pTempTexture->GetResource(), ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource, ResourceState::CopyDest));
        barriers.push_back(Barrier::Transition(
            m_pColorTarget->GetResource(), ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource, ResourceState::CopySource));
        ResourceBarrier(pCmdList, static_cast<uint32_t>(barriers.size()), barriers.data());
    }

    {
        GPUScopedProfileCapture sampleMarker(pCmdList, L"CopyToTemp");

        TextureCopyDesc desc(m_pColorTarget->GetResource(), m_pTempTexture->GetResource());
        CopyTextureRegion(pCmdList, &desc);
    }

    {
        std::vector<Barrier> barriers;
        barriers.push_back(Barrier::Transition(
            m_pTempTexture->GetResource(), ResourceState::CopyDest, ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource));
        barriers.push_back(Barrier::Transition(
            m_pColorTarget->GetResource(), ResourceState::CopySource, ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource));
        ResourceBarrier(pCmdList, static_cast<uint32_t>(barriers.size()), barriers.data());
    }

    // Note, inverted depth and display mode are currently handled statically for the run of the sample.
    // If they become changeable at runtime, we'll need to modify how this information is queried
    static bool s_InvertedDepth = GetConfig()->InvertedDepth;

    // use this to temporarily define export-related variables when hack is off but you want export.
    if (!hackOptions.enableHack)
    {
        const_cast<HackOptionDef&>(hackOptions).storeOutput    = false;
        const_cast<HackOptionDef&>(hackOptions).outputMaxCount = 15;
        const_cast<HackOptionDef&>(hackOptions).identifier     = "DefaultSceneBB";
        const_cast<HackOptionDef&>(hackOptions).outPath        = L"../media/EmptySanityCheck/Horizontal/outputs";
    }
    uint64_t hackIdx = 0;
    if (hackOptions.enableHack)
    {
        // loop thru hacking textures depending on frameID
        hackIdx          = m_FrameID % m_pHackColors.size();
        if (hackOptions.parseJitter)
        {
            m_JitterX = m_pHackJitterXY[hackIdx].first;
            m_JitterY = m_pHackJitterXY[hackIdx].second;
        }
    }

    /// Before any FSR dispatch, we can export any FSR input.
    if (false)  // manually turn on/off
    {
        auto hackTexture   = m_pHackColors[m_FrameID % m_pHackColors.size()]->GetResource();
        //auto hackTexture  = m_pTempTexture->GetResource();
        auto currentState = hackTexture->GetCurrentResourceState();
        bool exportSuccess =
            ExportDebugFrame(SDKWrapper::ffxGetResourceApi(hackTexture, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ), m_kSkipFramesInput, "ColorHack");
        goto finishup;
    }
    //goto finishup;

    // Upscale the scene first
    if (m_UpscaleMethod == Upscaler_Native)
    {
        // Native, nothing to do

        if (hackOptions.enableHack)
        {
            // copy hackColor to colorTarget
            std::vector<Barrier> barriers;
            barriers.push_back(Barrier::Transition(
                m_pColorTarget->GetResource(), ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource, ResourceState::CopyDest));
            barriers.push_back(Barrier::Transition(
                m_pHackColors[hackIdx]->GetResource(), ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource, ResourceState::CopySource));
            ResourceBarrier(pCmdList, static_cast<uint32_t>(barriers.size()), barriers.data());

            TextureCopyDesc desc(m_pHackColors[hackIdx]->GetResource(), m_pColorTarget->GetResource());
            CopyTextureRegion(pCmdList, &desc);

            barriers.clear();
            barriers.push_back(Barrier::Transition(
                m_pColorTarget->GetResource(), ResourceState::CopyDest, ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource));
            barriers.push_back(Barrier::Transition(
                m_pHackColors[hackIdx]->GetResource(), ResourceState::CopySource, ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource));
            ResourceBarrier(pCmdList, static_cast<uint32_t>(barriers.size()), barriers.data());
        }

    }

    if (m_UpscaleMethod == Upscaler_FSRAPI)
    {
        // FFXAPI
        // All cauldron resources come into a render module in a generic read state (ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource)
        ffx::DispatchDescUpscale dispatchUpscale{};

#if defined(FFX_API_DX12)
        dispatchUpscale.commandList = pCmdList->GetImpl()->DX12CmdList();
#elif defined(FFX_API_VK)
        dispatchUpscale.commandList = pCmdList->GetImpl()->VKCmdBuffer();
#endif  // defined(FFX_API_DX12)

        if (hackOptions.enableHack)
        {
            dispatchUpscale.color         = SDKWrapper::ffxGetResourceApi(m_pHackColors[hackIdx]->GetResource(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
            dispatchUpscale.depth         = SDKWrapper::ffxGetResourceApi(m_pHackDepths[hackIdx]->GetResource(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
            dispatchUpscale.motionVectors = SDKWrapper::ffxGetResourceApi(m_pHackMVs[hackIdx]->GetResource(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        }
        else
        {
            dispatchUpscale.color         = SDKWrapper::ffxGetResourceApi(m_pTempTexture->GetResource(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
            dispatchUpscale.depth         = SDKWrapper::ffxGetResourceApi(m_pDepthTarget->GetResource(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
            dispatchUpscale.motionVectors = SDKWrapper::ffxGetResourceApi(m_pMotionVectors->GetResource(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        }
        dispatchUpscale.exposure = SDKWrapper::ffxGetResourceApi(nullptr, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        dispatchUpscale.output = SDKWrapper::ffxGetResourceApi(m_pColorTarget->GetResource(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);

        if (m_MaskMode != FSRMaskMode::Disabled && !hackOptions.enableHack)
        {
            dispatchUpscale.reactive = SDKWrapper::ffxGetResourceApi(m_pReactiveMask->GetResource(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        }
        else
        {
            dispatchUpscale.reactive = SDKWrapper::ffxGetResourceApi(nullptr, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        }

        if (m_UseMask && !hackOptions.enableHack)
        {
            dispatchUpscale.transparencyAndComposition =
                SDKWrapper::ffxGetResourceApi(m_pCompositionMask->GetResource(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        }
        else
        {
            dispatchUpscale.transparencyAndComposition = SDKWrapper::ffxGetResourceApi(nullptr, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        }

        // Jitter is calculated earlier in the frame using a callback from the camera update
        dispatchUpscale.jitterOffset.x = -m_JitterX;
        dispatchUpscale.jitterOffset.y = -m_JitterY;
        dispatchUpscale.motionVectorScale.x = resInfo.fRenderWidth();
        dispatchUpscale.motionVectorScale.y = resInfo.fRenderHeight();
        dispatchUpscale.reset = m_ResetUpscale || GetScene()->GetCurrentCamera()->WasCameraReset();
        dispatchUpscale.enableSharpening = m_RCASSharpen;
        dispatchUpscale.sharpness = m_Sharpness;

        // Cauldron keeps time in seconds, but FSR expects milliseconds
        dispatchUpscale.frameTimeDelta = hackOptions.enableHack? CustomFPS : static_cast<float>(deltaTime * 1000.f);

        dispatchUpscale.preExposure = hackOptions.enableHack ? 1.0f : GetScene()->GetSceneExposure();
        dispatchUpscale.renderSize.width = resInfo.RenderWidth;
        dispatchUpscale.renderSize.height = resInfo.RenderHeight;
        dispatchUpscale.upscaleSize.width = resInfo.UpscaleWidth;
        dispatchUpscale.upscaleSize.height = resInfo.UpscaleHeight;

        // Setup camera params as required
        dispatchUpscale.cameraFovAngleVertical = hackOptions.enableHack ? Radian90 : pCamera->GetFovY();

        if (s_InvertedDepth)
        {
            dispatchUpscale.cameraFar  = pCamera->GetNearPlane();
            dispatchUpscale.cameraNear = FLT_MAX;
        }
        else
        {
            dispatchUpscale.cameraFar  = pCamera->GetFarPlane();
            dispatchUpscale.cameraNear = pCamera->GetNearPlane();
        }

        dispatchUpscale.flags = 0;
        dispatchUpscale.flags |= m_DrawUpscalerDebugView ? FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW : 0;

        ffx::ReturnCode retCode = ffx::Dispatch(m_UpscalingContext, dispatchUpscale);
        CauldronAssert(ASSERT_CRITICAL, !!retCode, L"Dispatching FSR upscaling failed: %d", (uint32_t)retCode);
    
        // After SR but before FG, we can look at SR output.
        if (false) // manually turn on/off
        {
            //bool exportSuccess = ExportDebugFrame(dispatchUpscale.motionVectors, m_kSkipFramesInput, "OriginalMV");
            bool exportSuccess = ExportDebugFrame(dispatchUpscale.output, m_kSkipFramesSR, "SR_Outputs");
            CauldronAssert(ASSERT_ERROR, exportSuccess, L"export MV and Depth failed");
        }
    }

    if (m_FrameInterpolationAvailable)
    {
        ffx::DispatchDescFrameGenerationPrepare dispatchFgPrep{};

#if defined(FFX_API_DX12)
        dispatchFgPrep.commandList = pCmdList->GetImpl()->DX12CmdList();
#elif defined(FFX_API_VK)
        dispatchFgPrep.commandList = pCmdList->GetImpl()->VKCmdBuffer();
#endif  // defined(FFX_API_DX12)

        if (hackOptions.enableHack)
        {
            dispatchFgPrep.depth         = SDKWrapper::ffxGetResourceApi(m_pHackDepths[hackIdx]->GetResource(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
            dispatchFgPrep.motionVectors = SDKWrapper::ffxGetResourceApi(m_pHackMVs[hackIdx]->GetResource(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        }
        else
        {
            dispatchFgPrep.depth         = SDKWrapper::ffxGetResourceApi(m_pDepthTarget->GetResource(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
            dispatchFgPrep.motionVectors = SDKWrapper::ffxGetResourceApi(m_pMotionVectors->GetResource(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        }
        dispatchFgPrep.flags         = 0;

        dispatchFgPrep.jitterOffset.x      = -m_JitterX;
        dispatchFgPrep.jitterOffset.y      = -m_JitterY;
        dispatchFgPrep.motionVectorScale.x = resInfo.fRenderWidth();
        dispatchFgPrep.motionVectorScale.y = resInfo.fRenderHeight();

        // Cauldron keeps time in seconds, but FSR expects milliseconds
        dispatchFgPrep.frameTimeDelta = hackOptions.enableHack ? CustomFPS : static_cast<float>(deltaTime * 1000.f);

        dispatchFgPrep.renderSize.width       = resInfo.RenderWidth;
        dispatchFgPrep.renderSize.height      = resInfo.RenderHeight;
        dispatchFgPrep.cameraFovAngleVertical = hackOptions.enableHack ? Radian90 : pCamera->GetFovY();

        if (s_InvertedDepth)
        {
            dispatchFgPrep.cameraFar  = pCamera->GetNearPlane();
            dispatchFgPrep.cameraNear = FLT_MAX;
        }
        else
        {
            dispatchFgPrep.cameraFar  = pCamera->GetFarPlane();
            dispatchFgPrep.cameraNear = pCamera->GetNearPlane();
        }
        dispatchFgPrep.viewSpaceToMetersFactor = 0.f;
        dispatchFgPrep.frameID                 = m_FrameID;

        // Update frame generation config
        FfxApiResource hudLessResource =
            SDKWrapper::ffxGetResourceApi(m_pHudLessTexture[m_curUiTextureIndex]->GetResource(), FFX_API_RESOURCE_STATE_COMPUTE_READ);

        m_FrameGenerationConfig.frameGenerationEnabled = m_FrameInterpolation;
        m_FrameGenerationConfig.flags = 0u;
        m_FrameGenerationConfig.flags |= m_DrawFrameGenerationDebugTearLines ? FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES : 0;
        m_FrameGenerationConfig.flags |= m_DrawFrameGenerationDebugResetIndicators ? FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_RESET_INDICATORS : 0;
        m_FrameGenerationConfig.flags |= m_DrawFrameGenerationDebugPacingLines ? FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_PACING_LINES : 0;
        m_FrameGenerationConfig.flags |= m_DrawFrameGenerationDebugView ? FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW : 0;
        dispatchFgPrep.flags = m_FrameGenerationConfig.flags;  // TODO: maybe these should be distinct flags?
        m_FrameGenerationConfig.HUDLessColor        = (s_uiRenderMode == 3) ? hudLessResource : FfxApiResource({});
        m_FrameGenerationConfig.allowAsyncWorkloads = m_AllowAsyncCompute && m_EnableAsyncCompute;
        // assume symmetric letterbox
        m_FrameGenerationConfig.generationRect.left   = (resInfo.DisplayWidth - resInfo.UpscaleWidth) / 2;
        m_FrameGenerationConfig.generationRect.top    = (resInfo.DisplayHeight - resInfo.UpscaleHeight) / 2;
        m_FrameGenerationConfig.generationRect.width  = resInfo.UpscaleWidth;
        m_FrameGenerationConfig.generationRect.height = resInfo.UpscaleHeight;
        if (m_UseCallback)
        {
            m_FrameGenerationConfig.frameGenerationCallback = [](ffxDispatchDescFrameGeneration* params, void* pUserCtx) -> ffxReturnCode_t {
                return ffxDispatch(reinterpret_cast<ffxContext*>(pUserCtx), &params->header);
            };
            m_FrameGenerationConfig.frameGenerationCallbackUserContext = &m_FrameGenContext;
        }
        else
        {
            m_FrameGenerationConfig.frameGenerationCallback            = nullptr;
            m_FrameGenerationConfig.frameGenerationCallbackUserContext = nullptr;
        }
        m_FrameGenerationConfig.onlyPresentGenerated = m_PresentInterpolatedOnly;
        m_FrameGenerationConfig.frameID              = m_FrameID;

        void* ffxSwapChain;
#if defined(FFX_API_DX12)
        ffxSwapChain = GetSwapChain()->GetImpl()->DX12SwapChain();
#elif defined(FFX_API_VK)
        ffxSwapChain = GetSwapChain()->GetImpl()->VKSwapChain();
#endif  // defined(FFX_API_DX12)
        m_FrameGenerationConfig.swapChain = ffxSwapChain;
        ffx::ReturnCode retCode           = ffx::ReturnCode::ErrorParameter;

        if (m_UseDistortionField)
        {
            ffx::ConfigureDescFrameGenerationRegisterDistortionFieldResource dfConfig{};
            dfConfig.distortionField =
                SDKWrapper::ffxGetResourceApi(m_pDistortionField[m_curUiTextureIndex]->GetResource(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
            retCode = ffx::Configure(m_FrameGenContext, m_FrameGenerationConfig, dfConfig);
        }
        else
        {
            retCode = ffx::Configure(m_FrameGenContext, m_FrameGenerationConfig);
        }

        CauldronAssert(ASSERT_CRITICAL, !!retCode, L"Configuring FSR FG failed: %d", (uint32_t)retCode);

        ffx::DispatchDescFrameGenerationPrepareCameraInfo cameraConfig{};
        /// Tracing the shader, no camera data are used other than near and far (for depth)
        memcpy(cameraConfig.cameraPosition, &pCamera->GetCameraPos(), 3 * sizeof(float));
        memcpy(cameraConfig.cameraUp, &pCamera->GetCameraUp(), 3 * sizeof(float));
        memcpy(cameraConfig.cameraRight, &pCamera->GetCameraRight(), 3 * sizeof(float));
        memcpy(cameraConfig.cameraForward, &(pCamera->GetDirection().getXYZ()), 3 * sizeof(float));

        retCode = ffx::Dispatch(m_FrameGenContext, dispatchFgPrep, cameraConfig);

        CauldronAssert(ASSERT_CRITICAL, !!retCode, L"Dispatching FSR FG (upscaling data) failed: %d", (uint32_t)retCode);

        FfxApiResource uiColor =
            (s_uiRenderMode == 1) ? SDKWrapper::ffxGetResourceApi(m_pUiTexture[m_curUiTextureIndex]->GetResource(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ)
                                  : FfxApiResource({});

#if defined(FFX_API_DX12)
        ffx::ConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 uiConfig{};
        uiConfig.uiResource = uiColor;
        uiConfig.flags      = m_DoublebufferInSwapchain ? FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING : 0;
        ffx::Configure(m_SwapChainContext, uiConfig);
#elif defined(FFX_API_VK)
        ffx::ConfigureDescFrameGenerationSwapChainRegisterUiResourceVK uiConfig{};
        uiConfig.uiResource = uiColor;
        uiConfig.flags      = m_DoublebufferInSwapchain ? FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING : 0;
        ffx::Configure(m_SwapChainContext, uiConfig);
#endif  // defined(FFX_API_DX12)
    }

    // Dispatch frame generation, if not using the callback
    if (m_FrameInterpolation && !m_UseCallback)
    {
        ffx::DispatchDescFrameGeneration dispatchFg{};

        /// Tricky Issue:
        /// Here, what gets binded to FG input is swapchain backbuffer instead of m_pColorTarget, 
        /// which is output of SR and makes more sense to be FG input.
        /// This is because FG outputs are binded to swapchain (see ffx::Query below) and swapchain BB stores
        /// post-tonemapped data while m_pColorTarget stores pre-tonemapped data (see ToneMappingRenderModule::Init
        /// in tonemappingrendermodule.cpp).
        /// 
        /// If using m_pColorTarget as inputs, there will be obvious brightness/exposure difference b/w 
        /// SR output and SR+FG output because:
        /// 1. m_pColorTarget stores pre-tonemapped SR output, and post-tonemapped SR output in swapchain BB is displayed.
        /// 2. If using m_pColorTarget as FG input, FG output is also pre-tonemapped, but it is stored directly
        ///    to swapchain BB (likely in another slot) and thus gets displayed in pre-tonemapped.
        /// 
        /// The fix is to let swapchain BB stores the pre-tonemapped data consistent with m_pColorTarget.
        /// This is done by directly texture copy and skip everything normally done in tonemapping RM.
        /// We should only do this when we want to export pre-tonemapped FG output.
        dispatchFg.presentColor = backbuffer;
        //dispatchFg.presentColor = SDKWrapper::ffxGetResourceApi(m_pColorTarget->GetResource(), FFX_API_RESOURCE_STATE_PRESENT);

        dispatchFg.numGeneratedFrames = 1;
        //if (hackOptions.enableHack)
        //    dispatchFg.backbufferTransferFunction = static_cast<uint32_t>(FfxBackbufferTransferFunction::FFX_BACKBUFFER_TRANSFER_FUNCTION_SCRGB);

        // assume symmetric letterbox
        dispatchFg.generationRect.left = (resInfo.DisplayWidth - resInfo.UpscaleWidth) / 2;
        dispatchFg.generationRect.top = (resInfo.DisplayHeight - resInfo.UpscaleHeight) / 2;
        dispatchFg.generationRect.width = resInfo.UpscaleWidth;
        dispatchFg.generationRect.height = resInfo.UpscaleHeight;

#if defined(FFX_API_DX12)
        ffx::QueryDescFrameGenerationSwapChainInterpolationCommandListDX12 queryCmdList{};
        queryCmdList.pOutCommandList = &dispatchFg.commandList;
        ffx::Query(m_SwapChainContext, queryCmdList);

        ffx::QueryDescFrameGenerationSwapChainInterpolationTextureDX12 queryFiTexture{};
        queryFiTexture.pOutTexture = &dispatchFg.outputs[0];
        ffx::Query(m_SwapChainContext, queryFiTexture);
#elif defined(FFX_API_VK)
        ffx::QueryDescFrameGenerationSwapChainInterpolationCommandListVK queryCmdList{};
        queryCmdList.pOutCommandList = &dispatchFg.commandList;
        ffx::Query(m_SwapChainContext, queryCmdList);

        ffx::QueryDescFrameGenerationSwapChainInterpolationTextureVK queryFiTexture{};
        queryFiTexture.pOutTexture = &dispatchFg.outputs[0];
        ffx::Query(m_SwapChainContext, queryFiTexture);
#endif  // defined(FFX_API_DX12)

        dispatchFg.frameID = m_FrameID;
        dispatchFg.reset = m_ResetFrameInterpolation;

        ffx::ReturnCode retCode = ffx::Dispatch(m_FrameGenContext, dispatchFg);
        
        CauldronAssert(ASSERT_CRITICAL, !!retCode, L"Dispatching Frame Generation failed: %d", (uint32_t)retCode);
        if (hackOptions.storeOutput)
        {
            bool exportSuccess = ExportDebugFrame(dispatchFg.outputs[0], m_kSkipFramesFG);
            CauldronAssert(ASSERT_CRITICAL, exportSuccess, L"Export failed at frame %d", GetFramework()->GetFrameID());
        }

        if (hackOptions.storeOutput)
        {
            // frameID check is done in export function
            bool exportSuccess = ExportDebugFrame(dispatchFg.outputs[0], m_kSkipFramesFG);
        }

        // Other than saving FG frames, we can look at any resources used by FG, see SRV_debug at top of the file.
        if (false)  // manually turn on/off
        {
            bool exportSuccess = ExportDebugFrame(
                dispatchFg.DebugApiResources[SRV_debug::CurrInterpSource.second], 
                m_kSkipFramesFG, 
                SRV_debug::CurrInterpSource.first + ""
            );

            //bool exportSuccess = ExportDebugFrame(dispatchFg.presentColor, m_kSkipFramesFG, "inputFG");

            //bool exportSuccess = ExportDebugFrame2Inputs(
            //    dispatchFg.DebugApiResources[SRV_debug::OFlowMV_X.second],
            //    dispatchFg.DebugApiResources[SRV_debug::OFlowMV_Y.second], 
            //    m_kSkipFramesFG, 
            //    SRV_debug::OFlowMV_X.first);
            CauldronAssert(ASSERT_ERROR, exportSuccess, L"ExportDebugFrame() failed");
        }
    }

finishup:
    m_FrameID += uint64_t(1 + m_SimulatePresentSkip);
    m_SimulatePresentSkip = false;

    m_ResetUpscale = false;
    m_ResetFrameInterpolation = false;

    // FidelityFX contexts modify the set resource view heaps, so set the cauldron one back
    SetAllResourceViewHeaps(pCmdList);

    // We are now done with upscaling
    GetFramework()->SetUpscalingState(UpscalerState::PostUpscale);
}

void FSRRenderModule::PreTransCallback(double deltaTime, CommandList* pCmdList)
{
    GPUScopedProfileCapture sampleMarker(pCmdList, L"Pre-Trans (FSR)");

    std::vector<Barrier> barriers;
    barriers.push_back(Barrier::Transition(m_pReactiveMask->GetResource(), ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource, ResourceState::RenderTargetResource));
    barriers.push_back(Barrier::Transition(m_pCompositionMask->GetResource(), ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource, ResourceState::RenderTargetResource));
    ResourceBarrier(pCmdList, static_cast<uint32_t>(barriers.size()), barriers.data());

    // We need to clear the reactive and composition masks before any translucencies are rendered into them
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    ClearRenderTarget(pCmdList, &m_RasterViews[0]->GetResourceView(), clearColor);
    ClearRenderTarget(pCmdList, &m_RasterViews[1]->GetResourceView(), clearColor);

    barriers.clear();
    barriers.push_back(Barrier::Transition(m_pReactiveMask->GetResource(), ResourceState::RenderTargetResource, ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource));
    barriers.push_back(Barrier::Transition(m_pCompositionMask->GetResource(), ResourceState::RenderTargetResource, ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource));
    ResourceBarrier(pCmdList, static_cast<uint32_t>(barriers.size()), barriers.data());

    // update index for UI doublebuffering
    UIRenderModule* uimod = static_cast<UIRenderModule*>(GetFramework()->GetRenderModule("UIRenderModule"));
    m_curUiTextureIndex = (++m_curUiTextureIndex) & 1;
    uimod->SetUiSurfaceIndex(m_curUiTextureIndex);

    //update index for distortion texture doublebuffering
    m_pToneMappingRenderModule->SetDoubleBufferedTextureIndex(m_curUiTextureIndex);

    if (m_MaskMode != FSRMaskMode::Auto)
        return;

    barriers.clear();
    barriers.push_back(Barrier::Transition(m_pColorTarget->GetResource(), ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource, ResourceState::CopySource));
    barriers.push_back(Barrier::Transition(m_pOpaqueTexture->GetResource(), ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource, ResourceState::CopyDest));
    ResourceBarrier(pCmdList, static_cast<uint32_t>(barriers.size()), barriers.data());

    // Copy the color render target before we apply translucency
    TextureCopyDesc copyColor = TextureCopyDesc(m_pColorTarget->GetResource(), m_pOpaqueTexture->GetResource());
    CopyTextureRegion(pCmdList, &copyColor);

    barriers.clear();
    barriers.push_back(Barrier::Transition(m_pColorTarget->GetResource(), ResourceState::CopySource, ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource));
    barriers.push_back(Barrier::Transition(m_pOpaqueTexture->GetResource(), ResourceState::CopyDest, ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource));
    ResourceBarrier(pCmdList, static_cast<uint32_t>(barriers.size()), barriers.data());
}

void FSRRenderModule::PostTransCallback(double deltaTime, CommandList* pCmdList)
{
    if ((m_MaskMode != FSRMaskMode::Auto) || (m_UpscaleMethod != Upscaler_FSRAPI))
        return;

    GPUScopedProfileCapture sampleMarker(pCmdList, L"Gen Reactive Mask (FSR API)");

    ffx::DispatchDescUpscaleGenerateReactiveMask dispatchDesc{};
#if defined(FFX_API_DX12)
    dispatchDesc.commandList   = pCmdList->GetImpl()->DX12CmdList();
#elif defined(FFX_API_VK)
    dispatchDesc.commandList   = pCmdList->GetImpl()->VKCmdBuffer();
#endif  // defined(FFX_API_DX12)
    dispatchDesc.colorOpaqueOnly = SDKWrapper::ffxGetResourceApi(m_pOpaqueTexture->GetResource(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dispatchDesc.colorPreUpscale = SDKWrapper::ffxGetResourceApi(m_pColorTarget->GetResource(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dispatchDesc.outReactive = SDKWrapper::ffxGetResourceApi(m_pReactiveMask->GetResource(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);

    const ResolutionInfo& resInfo = GetFramework()->GetResolutionInfo();
    dispatchDesc.renderSize.width = resInfo.RenderWidth;
    dispatchDesc.renderSize.height = resInfo.RenderHeight;

    // The following are all hard-coded in the original FSR2 sample. Should these be exposed?
    dispatchDesc.scale = 1.f;
    dispatchDesc.cutoffThreshold = 0.2f;
    dispatchDesc.binaryValue = 0.9f;
    dispatchDesc.flags = FFX_UPSCALE_AUTOREACTIVEFLAGS_APPLY_TONEMAP |
                         FFX_UPSCALE_AUTOREACTIVEFLAGS_APPLY_THRESHOLD |
                         FFX_UPSCALE_AUTOREACTIVEFLAGS_USE_COMPONENTS_MAX;

    ffx::ReturnCode retCode = ffx::Dispatch(m_UpscalingContext, dispatchDesc);
    CauldronAssert(ASSERT_ERROR, retCode == ffx::ReturnCode::Ok, L"ffxDispatch(FSR_GENERATEREACTIVEMASK) failed with %d", (uint32_t)retCode);

    // FidelityFX contexts modify the set resource view heaps, so set the cauldron one back
    SetAllResourceViewHeaps(pCmdList);
}

// Copy of ffxRestoreApplicationSwapChain from backend_interface, which is not built for this sample.
#if defined(FFX_API_DX12)
void RestoreApplicationSwapChain(bool recreateSwapchain)
{
    cauldron::SwapChain* pSwapchain  = cauldron::GetSwapChain();
    IDXGISwapChain4*     pSwapChain4 = pSwapchain->GetImpl()->DX12SwapChain();
    pSwapChain4->AddRef();

    IDXGIFactory7*      factory   = nullptr;
    ID3D12CommandQueue* pCmdQueue = cauldron::GetDevice()->GetImpl()->DX12CmdQueue(cauldron::CommandQueue::Graphics);

    // Setup a new swapchain for HWND and set it to cauldron
    IDXGISwapChain1* pSwapChain1 = nullptr;
    if (SUCCEEDED(pSwapChain4->GetParent(IID_PPV_ARGS(&factory))))
    {
        cauldron::GetSwapChain()->GetImpl()->SetDXGISwapChain(nullptr);

        // safe data since release will destroy the swapchain (and we need it distroyed before we can create the new one)
        HWND windowHandle = pSwapchain->GetImpl()->DX12SwapChainDesc().OutputWindow;
        DXGI_SWAP_CHAIN_DESC1 desc1 = pSwapchain->GetImpl()->DX12SwapChainDesc1();
        DXGI_SWAP_CHAIN_FULLSCREEN_DESC  fsDesc = pSwapchain->GetImpl()->DX12SwapChainFullScreenDesc();

        pSwapChain4->Release();

        // check if window is still valid or if app is shutting down bc window was closed
        if (recreateSwapchain && IsWindow(windowHandle))
        {
            if (SUCCEEDED(factory->CreateSwapChainForHwnd(pCmdQueue, windowHandle, &desc1, &fsDesc, nullptr, &pSwapChain1)))
            {
                if (SUCCEEDED(pSwapChain1->QueryInterface(IID_PPV_ARGS(&pSwapChain4))))
                {
                    cauldron::GetSwapChain()->GetImpl()->SetDXGISwapChain(pSwapChain4);
                    pSwapChain4->Release();
                }
                pSwapChain1->Release();
            }
            factory->MakeWindowAssociation(cauldron::GetFramework()->GetImpl()->GetHWND(), DXGI_MWA_NO_WINDOW_CHANGES);
        }

        factory->Release();
    }
    return;
}

#elif defined(FFX_API_VK)
void RestoreApplicationSwapChain(bool recreateSwapchain)
{
    const VkSwapchainCreateInfoKHR* pCreateInfo = cauldron::GetSwapChain()->GetImpl()->GetCreateInfo();
    VkSwapchainKHR                  swapchain   = cauldron::GetSwapChain()->GetImpl()->VKSwapChain();
    cauldron::GetSwapChain()->GetImpl()->SetVKSwapChain(VK_NULL_HANDLE);
    cauldron::GetDevice()->GetImpl()->DestroySwapchainKHR(swapchain, nullptr);
    cauldron::GetDevice()->GetImpl()->SetSwapchainMethodsAndContext();  // reset all
    swapchain = VK_NULL_HANDLE;
    if (recreateSwapchain)
    {
        VkResult res = cauldron::GetDevice()->GetImpl()->CreateSwapchainKHR(pCreateInfo, nullptr, &swapchain);
        if (res == VK_SUCCESS)
        {
            // Swapchain creation can fail when this function is called when closing the application. In that case, just exit silently
            cauldron::GetSwapChain()->GetImpl()->SetVKSwapChain(swapchain);
        }
    }
}
#endif

#if defined(FFX_API_DX12)
#endif // FFX_API_DX12
