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

#include <filesystem>

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
        std::filesystem::path TextureFile;                    ///< Path to the texture to load.
        bool                                SRGB = true;                    ///< If we need this to be in SRGB format.
        float                               AlphaThreshold = 1.f;           ///< Alpha threshold for alpha generation.
        ResourceFlags                       Flags = ResourceFlags::None;    ///< <c><i>ResourceFlags</i></c> for the loaded <c><i>Texture</i></c>.

        TextureLoadInfo(std::filesystem::path file, bool srgb = true, float alphaThreshold = 1.f, ResourceFlags flags = ResourceFlags::None) : TextureFile(file), SRGB(srgb), AlphaThreshold(alphaThreshold), Flags(flags) {};
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

    struct ExportInfo
    {
        uint32_t    width;
        uint32_t    height;
        uint32_t    rowPitch;           // Row pitch in bytes (stride per row)
        uint32_t    numSourceChannels;  // Number of channels per pixel in the source data (e.g., 4 for RGBA)
        std::string name;               // see name_map in fsrapirendermodule.cpp
        std::string filename;           // Output EXR filename

        // First 2 bits belong to MV mode. Others reserved for future use.
        enum class BitUnpackMode : uint32_t {
            // As it, float as FP32, uint32_t as FP16, etc.
            Normal = 0,

            // RGBA = { 0.5 + fMV * displaySize * 0.1， 0.5， 0.5 }
            MV_Remap = 1 << 0,
            // Unpack X&Y values from lower 16 bits;
            Unpack_Lower16 = 1 << 1,
            MV_Uint32 = MV_Remap | Unpack_Lower16,

            // Directly print (instead of export to EXR) 
            LogSCD = 1 << 2, // SCD has 3 values.
            LogDF  = 1 << 3, // Distortion Field has 2 values packed in RG8_UNORM.

        }           unpackMode = BitUnpackMode::Normal;
    };

    inline bool isRemap(ExportInfo::BitUnpackMode mode) {
        return (static_cast<uint32_t>(mode) & static_cast<uint32_t>(ExportInfo::BitUnpackMode::MV_Remap)) != 0;
    }

    inline bool isUnpack(ExportInfo::BitUnpackMode mode) {
        return (static_cast<uint32_t>(mode) & static_cast<uint32_t>(ExportInfo::BitUnpackMode::Unpack_Lower16)) != 0;
    }

    constexpr uint32_t NumChannelsFromFormat(ResourceFormat format) {
        switch (format)
        {
        case ResourceFormat::R8_UNORM:
        case ResourceFormat::R16_FLOAT:
        case ResourceFormat::R32_FLOAT:
        case ResourceFormat::R32_UINT:
            return 1;
        case ResourceFormat::RG8_UNORM:
        case ResourceFormat::RG16_FLOAT:
        case ResourceFormat::RG16_SINT:
        case ResourceFormat::RG32_FLOAT:
            return 2;
        case ResourceFormat::RGBA8_UNORM:
        case ResourceFormat::RGBA16_FLOAT:
        case ResourceFormat::RGBA32_FLOAT:
            return 4;
        default:
            CauldronError(false, "Unsupported format.");
        }
    }

    
    template <typename T>
    uint16_t convertToFP16(T value, uint32_t scale, ExportInfo::BitUnpackMode mode);

    bool DispatchTemplateExport(ResourceFormat format, const void* data, const ExportInfo& info);

    template <typename T>
    bool SaveDataWithFormatToEXR(const void* data, const ExportInfo& info);

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
        virtual bool LoadTextureData(std::filesystem::path& textureFile, float alphaThreshold, TextureDesc& texDesc) = 0;

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
        virtual bool LoadTextureData(std::filesystem::path& textureFile, float alphaThreshold, TextureDesc& texDesc) override;

        
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
        static constexpr size_t Width1K  = 1920;
        static constexpr size_t Height1K = 1080;
        static constexpr size_t PixelCount1K = Width1K * Height1K;
        enum class SpecialChannelType : int
        {
            ColorRGB = 0,       // format handled by SetResourceFormat()
            MotionVectors = 1,  // RG16_FLOAT format
            Depth = 2           // R32_FLOAT format
        };


        EXRTextureDataBlock()
            : TextureDataBlock() {};
        virtual ~EXRTextureDataBlock();

        virtual bool LoadTextureData(std::filesystem::path& textureFile, float alphaThreshold, TextureDesc& texDesc) override;

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
        bool LoadJitterData1K(std::filesystem::path& textureFile, 
                              float alphaThreshold, 
                              TextureDesc& texDesc,
                              SpecialChannelType channelType);

        /**
         * @brief   Sets the resource format to that in the swapchain.
         * @param format Should be one of the following, already be set by SwapChain creation:
         * RGBA8_UNORM, RGB10A2_UNORM, or RGBA16_FLOAT.
         * 
         */
        void SetResourceFormat(ResourceFormat format) { m_Format = format; }

        /**
         * @brief Given a list of all exr file paths, extract jitter from filename and store to output vector
         * 
         * @param exrPaths const ref to a vector of input exr.
         * @param jitterXY Store data here.
         * 
         * @return Success or not
         */
        static bool ParseJitter(const std::vector<std::filesystem::path>& exrPaths,
                                std::vector<std::pair<float, float>>&                   jitterXY);


    private:
        char*  m_pData    = nullptr;  // EXR uses float*, but it will cause error
        // Necessary because we supports 3 formats, must be known before LoadTextureData()
        ResourceFormat m_Format = ResourceFormat::Unknown;
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
        virtual bool LoadTextureData(std::filesystem::path& textureFile, float alphaThreshold, TextureDesc& texDesc) override;

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
        virtual bool LoadTextureData(std::filesystem::path& textureFile, float alphaThreshold, TextureDesc& texDesc) override;

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
