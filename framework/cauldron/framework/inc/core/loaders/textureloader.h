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

#include "core/contentloader.h"
#include "misc/helpers.h"
#include "render/texture.h"

#define _SILENCE_EXPERIMENTAL_FILESYSTEM_DEPRECATION_WARNING    // To avoid receiving deprecation error since we are using C++11 only
#include <experimental/filesystem>

#include <functional>
#include <shaders/shadercommon.h>

namespace cauldron
{
    /**
     * @typedef TextureLoadCompletionCallbackFn
     *
     * Convenience type for texture load completion callbacks.
     *
     * @ingroup CauldronLoaders
     */
    typedef std::function<void(const std::vector<const Texture*>&, void* pParam)> TextureLoadCompletionCallbackFn;

    /**
     * @struct TextureLoadInfo
     *
     * Description struct for <c><i>Texture</i></c> loading job.
     *
     * @ingroup CauldronLoaders
     */
    struct TextureLoadInfo
    {
        std::experimental::filesystem::path TextureFile;                    ///< Path to the texture to load.
        bool                                SRGB = true;                    ///< If we need this to be in SRGB format.
        float                               AlphaThreshold = 1.f;           ///< Alpha threshold for alpha generation.
        ResourceFlags                       Flags = ResourceFlags::None;    ///< <c><i>ResourceFlags</i></c> for the loaded <c><i>Texture</i></c>.

        TextureLoadInfo(std::experimental::filesystem::path file, bool srgb = true, float alphaThreshold = 1.f, ResourceFlags flags = ResourceFlags::None) : TextureFile(file), SRGB(srgb), AlphaThreshold(alphaThreshold), Flags(flags) {};
    };

    /**
     * @struct TextureLoadParams
     *
     * Parameter struct for both LoadAsync and LoadMultipleAsync.
     *
     * @ingroup CauldronLoaders
     */
    struct TextureLoadParams
    {
        std::vector<TextureLoadInfo>                                    LoadInfo = {};                  ///< <c><i>TextureLoadInfo</i></c> for the loading job.
        TextureLoadCompletionCallbackFn                                 LoadCompleteCallback = nullptr; ///< Completion callback to be called once the texture has been loaded.
        void* AdditionalParams = nullptr;                                                               ///< Additional parameters needed for the load completion callback. This memory is owned by the calling process.
    };

    /**
     * @class TextureDataBlock
     *
     * Base data block representation for loading various texture types.
     *
     * @ingroup CauldronLoaders
     */
    class TextureDataBlock
    {
    public:
        TextureDataBlock() {}
        virtual ~TextureDataBlock() = default;

        /**
         * @brief   Loads the texture data to memory according to the DataBlock type.
         */
        virtual bool LoadTextureData(std::experimental::filesystem::path& textureFile, float alphaThreshold, TextureDesc& texDesc) = 0;

        /**
         * @brief   Copies the texture data to the resource's backing memory.
         */
        virtual void CopyTextureData(void* pDest, uint32_t stride, uint32_t widthStride, uint32_t height, uint32_t sliceOffset) = 0;
    
    private:
        NO_COPY(TextureDataBlock)
        NO_MOVE(TextureDataBlock)
    };

    /**
     * @class WICTextureDataBlock
     *
     * Data block loader for STB image loads.
     * Textures loaded by STB loader will generate their own mip-chain and have options for
     * alpha generation.
     *
     * @ingroup CauldronLoaders
     */
    class WICTextureDataBlock : public TextureDataBlock
    {
    public:
        WICTextureDataBlock() : TextureDataBlock() {}
        virtual ~WICTextureDataBlock();

        /**
         * @brief   Loads the texture data to memory according to the DataBlock type.
         */
        virtual bool LoadTextureData(std::experimental::filesystem::path& textureFile, float alphaThreshold, TextureDesc& texDesc) override;

        
        /**
         * @brief   Copies the texture data to the resource's backing memory. Will also generate mip-chain.
         */
        virtual void CopyTextureData(void* pDest, uint32_t stride, uint32_t widthStride, uint32_t height, uint32_t sliceOffset) override;

    private:
        float GetAlphaCoverage(uint32_t width, uint32_t height, float scale, uint32_t alphaThreshold) const;
        void ScaleAlpha(uint32_t width, uint32_t height, float scale);
        void MipImage(uint32_t width, uint32_t height);

        char* m_pData = nullptr;

        float m_AlphaTestCoverage = 1.f;
        float m_AlphaThreshold = 1.f;
    };

    /**
     * @class EXRTextureDataBlock
     *
     * Data block loader for OpenEXR image loads.
     * Handles HDR textures with float16/float32 channels.
     *
     * @ingroup CauldronLoaders
     */
    class EXRTextureDataBlock : public TextureDataBlock
    {
    public:
        enum class SpecialChannelType : int
        {
            ColorRGB = 0,       // format handled by SetResourceFormat()
            MotionVectors = 1,  // RG16_FLOAT format
            Depth = 2           // R32_FLOAT format
        };


        EXRTextureDataBlock(float scaleFactor = 1.0f)
            : TextureDataBlock()
            , m_UpscaleRatio(scaleFactor)
        {}
        virtual ~EXRTextureDataBlock();

        virtual bool LoadTextureData(std::experimental::filesystem::path& textureFile, float alphaThreshold, TextureDesc& texDesc) override;

        virtual void CopyTextureData(void* pDest, uint32_t stride, uint32_t widthStride, uint32_t height, uint32_t sliceOffset) override;

        /**
         * @brief   Loads motion vectors or depth from a FIXED 1k jitter EXR file (RG=motion vectors, B=depth)
         * 
         * @param textureFile    Path to the EXR file
         * @param alphaThreshold Unused parameter (retained for signature compatibility)
         * @param texDesc        Output texture description
         * @param channelType    Specifies whether to load motion vectors or depth
         * 
         * @return               If loading succeeded
         */
        bool LoadJitterData1K(std::experimental::filesystem::path& textureFile, 
                              float alphaThreshold, 
                              TextureDesc& texDesc,
                              SpecialChannelType channelType);

        /**
         * @brief   Sets the resource format to that in the swapchain.
         * @param format Should be one of the following, already be set by SwapChain creation:
         * RGBA8_UNORM, RGB10A2_UNORM, or RGBA16_FLOAT.
         * 
         */
        void SetResourceFormat(ResourceFormat format) { 
            m_Format = format; 
        }

        /**
         * @brief Creates a debug texture with four distinct regions for coordinate verification
         * 
         * @param texDesc Texture description to populate
         */
        bool CreateDebugCoordinateTexture(TextureDesc& texDesc);

        /**
         * @brief Given a folder path, find all exr file paths. Also, optionally store jitter xy float2 from filenames.
         * 
         * @param outPaths Ref to a vector to save the paths.
         * @param extractJitter Whether to extract jitter xy from filenames. Now we only have them in 1k inputs
         * @param jitterXY If extractJitter, store data here.
         */
        static size_t TraverseFolder(std::wstring                                      folderPath,
                                     std::vector<std::experimental::filesystem::path>& outPaths,
                                     bool                                              extractJitter,
                                     std::vector<std::pair<float, float>>&             jitterXY);


    private:
        std::wstring textureName   = L"uninitialized";
        char*  m_pData    = nullptr;  // EXR uses float*, but it will cause error
        int    m_Width    = 0;
        int    m_Height   = 0;
        int    m_Channels = 0;
        // depending on display mode LDR or HDR, see setter
        ResourceFormat m_Format = ResourceFormat::Unknown;
        uint32_t       m_BytesPerPixel = 4;
        float          m_UpscaleRatio  = 1.0f;
    };

    /**
     * @class DDSTextureDataBlock
     *
     * Data block loader for DDS image loads.
     *
     * @ingroup CauldronLoaders
     */
    class DDSTextureDataBlock : public TextureDataBlock
    {
    public:
        DDSTextureDataBlock() : TextureDataBlock() {}
        virtual ~DDSTextureDataBlock();

        /**
         * @brief   Loads the texture data to memory according to the DataBlock type.
         */
        virtual bool LoadTextureData(std::experimental::filesystem::path& textureFile, float alphaThreshold, TextureDesc& texDesc) override;

        /**
         * @brief   Copies the texture data to the resource's backing memory.
         */
        virtual void CopyTextureData(void* pDest, uint32_t stride, uint32_t widthStride, uint32_t height, uint32_t sliceOffset) override;

    private:
        char* m_pData = nullptr;
    };

    /**
     * @class MemTextureDataBlock
     *
     * Data block loader for memory based texture load.
     *
     * @ingroup CauldronLoaders
     */
    class MemTextureDataBlock : public TextureDataBlock
    {
    public:
        MemTextureDataBlock(char* pMemory) : TextureDataBlock(), m_pData(pMemory) {}
        virtual ~MemTextureDataBlock();

        /**
         * @brief   As the MemTextureDataBlock is backed by memory already, LoadTextureData does nothing and should not be called.
         *          This function will assert if called.
         */
        virtual bool LoadTextureData(std::experimental::filesystem::path& textureFile, float alphaThreshold, TextureDesc& texDesc) override;

        /**
         * @brief   Copies the texture data to the resource's backing memory.
         */
        virtual void CopyTextureData(void* pDest, uint32_t stride, uint32_t widthStride, uint32_t height, uint32_t sliceOffset) override;

    private:
        MemTextureDataBlock() = delete;

        char* m_pData = nullptr;
    };

    /**
     * @class TextureLoader
     *
     * Texture loader class. Handles asynchronous texture loading.
     *
     * @ingroup CauldronLoaders
     */
    class TextureLoader : public ContentLoader
    {
    public:

        /**
         * @brief   Constructor with default behavior.
         */
        TextureLoader() = default;

        /**
         * @brief   Destructor with default behavior.
         */
        virtual ~TextureLoader() = default;

        /**
         * @brief   Loads a single <c><i>Texture</i></c> asynchronously.
         */
        virtual void LoadAsync(void* pLoadParams) override;

        /**
         * @brief   Loads multiple <c><i>Texture</i></c>s asynchronously.
         */
        virtual void LoadMultipleAsync(void* pLoadParams) override;

    private:
        static void LoadTextureContent(void* pParam);
        static void AsyncLoadCompleteCallback(void* pParam);
    };

} // namespace cauldron
