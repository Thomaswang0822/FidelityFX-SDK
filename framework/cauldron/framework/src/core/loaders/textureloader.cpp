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

#include "core/loaders/textureloader.h"
#include "core/contentmanager.h"
#include "core/taskmanager.h"
#include "core/framework.h"
#include "misc/assert.h"
#include "misc/fileio.h"
#include "render/device.h"
#include "render/gpuresource.h"

// Needed for EXR loading
#ifndef TINYEXR_IMPLEMENTATION
#define TINYEXR_USE_MINIZ    0
#define TINYEXR_USE_STB_ZLIB 1
#define TINYEXR_IMPLEMENTATION
#include "tinyexr.h"
#endif  // !TINYEXR_IMPLEMENTATION

using namespace std::experimental;

namespace cauldron
{
    template <typename T>
    uint16_t convertToFP16(T value, uint32_t scale, ExportInfo::BitUnpackMode mode)
    {
        // Whether to remap MV values and whether to unpack from lower 16 bits
        const bool shouldRemap  = isRemap(mode);
        const bool shouldUnpack = isUnpack(mode);

        if constexpr (std::is_same_v<T, uint16_t>) {
            CauldronAssert(ASSERT_CRITICAL, !shouldUnpack, L"Unpack_Lower16 mode is not applicable for uint16_t data.");
            if (shouldRemap) {
            // Convert from FP16 to FP32 for remapping
                tinyexr::FP16 fp16;
                fp16.u = value;
                tinyexr::FP32 fp32 = half_to_float(fp16);
                // Remap
                fp32.f = 0.5f + fp32.f * 0.1f * static_cast<float>(scale);
                // Convert back to FP16
                return float_to_half_full(fp32).u;
            }
            else
                return value;
        }
        else if constexpr (std::is_same_v<T, float>) {
            tinyexr::FP32 fp32;
            if (shouldUnpack)
            {
                // Then we must know the bit pattern
                uint32_t intValue = *reinterpret_cast<uint32_t*>(&value);
                tinyexr::FP16 fp16;
                fp16.u = static_cast<uint16_t>(intValue & 0xFFFF);
                fp32 = half_to_float(fp16);
            }
            fp32.f = shouldRemap ? (0.5f + fp32.f * 0.1f * scale) : value;
            return float_to_half_full(fp32).u;
        }
        else if constexpr (std::is_same_v<T, uint8_t>)
        {
            CauldronAssert(ASSERT_CRITICAL, !shouldUnpack && !shouldRemap, L"uint8_t data must be in normal mode.");
            // Convert from 0-255 UNORM to FP16
            float         fValue = static_cast<float>(value) / 255.0f;
            tinyexr::FP32 f32;
            f32.f = fValue;
            return float_to_half_full(f32).u;
        }
        else
        {
            CauldronError(L"Unsupported type for EXR export");
            return 0;
        }
    }
    /// Since <T> appears in the function arg, we don't need to specialize the function itself.

    template <typename T>
    bool SaveDataWithFormatToEXR(const void* data, const ExportInfo& info)
    {
        EXRHeader header;
        InitEXRHeader(&header);
        EXRImage exrImage;
        InitEXRImage(&exrImage);

        /// We always export to a generic RGB image even when we only have float2 (MV) or float (Depth) to store.
        /// And to enable image diff, we should be consistent with our inputs, which has BGR channel order. 
        //const char* channelNames[3]  = {"X", "Y", "Z"};
        const char* channelNames[3]  = {"B", "G", "R"};
        header.num_channels          = 3;
        header.channels              = new EXRChannelInfo[header.num_channels];
        header.pixel_types           = new int[header.num_channels];
        header.requested_pixel_types = new int[header.num_channels];

        // Allocate planar arrays for 3 channels
        std::vector<std::vector<uint16_t>> channelData(header.num_channels);
        // Prepare channel pointers for EXR
        std::vector<unsigned char*> imagePtrs(header.num_channels);

        for (int i = 0; i < header.num_channels; i++)
        {
            strncpy(header.channels[i].name, channelNames[i], 255);

            header.pixel_types[i]           = TINYEXR_PIXELTYPE_HALF;
            header.requested_pixel_types[i] = TINYEXR_PIXELTYPE_HALF;

            channelData[i].resize(info.width * info.height);
            imagePtrs[i] = reinterpret_cast<unsigned char*>(channelData[i].data());
        }
        header.compression_type = TINYEXR_COMPRESSIONTYPE_NONE;

        // Configure EXR image
        exrImage.num_channels = header.num_channels;
        exrImage.width        = info.width;
        exrImage.height       = info.height;
        exrImage.images       = imagePtrs.data();

        // Calculate row pitch in terms of uint16_t elements
        const size_t rowPitchElements = info.rowPitch / sizeof(T);
        const T*     typeTData        = reinterpret_cast<const T*>(data);

        const uint16_t FP16_Point5 = []() {
            tinyexr::FP32 fp32;
            fp32.f = 0.5f;
            return float_to_half_full(fp32).u;
        }();

        // We may get RGBA data, but only save RGB channels
        uint32_t numActiveChannels = std::min(info.numSourceChannels, 3u);
        // Deinterleave pixel data into planar format
        for (uint32_t y = 0; y < info.height; y++) {
            const T* srcRow = typeTData + y * rowPitchElements;

            for (uint32_t x = 0; x < info.width; x++) {
                const uint32_t dstIdx  = y * info.width + x;
                const uint32_t srcIdx  = x * info.numSourceChannels;

                // convert X and Y according to RGBA = { 0.5 + fMV * displaySize * 0.1， 0.5， 0.5 }
                if (numActiveChannels > 0)
                    channelData[2][dstIdx] = convertToFP16<T>(srcRow[srcIdx + 0], info.width, info.unpackMode);
                if (numActiveChannels > 1)
                    channelData[1][dstIdx] = convertToFP16<T>(srcRow[srcIdx + 1], info.height, info.unpackMode);
                // Channel B/Z: Set Z to 0.5 in FP16 if remap
                if (numActiveChannels > 2)
                    channelData[0][dstIdx] = isRemap(info.unpackMode) ? FP16_Point5 : 
                        convertToFP16<T>(srcRow[srcIdx + 2], 0 /* shouldn't be used */, info.unpackMode);
                // We don't have A/W channel
            }
        }

        // Save EXR file
        const char* err     = nullptr;
        int         ret     = SaveEXRImageToFile(&exrImage, &header, info.filename.c_str(), &err);
        bool        success = (ret == TINYEXR_SUCCESS);

        // Cleanup
        delete[] header.channels;
        delete[] header.pixel_types;
        delete[] header.requested_pixel_types;

        if (!success && err)
        {
            // Handle error (log or throw)
            FreeEXRErrorMessage(err);
            return false;
        }

        return success;
    }

    /// Explicitly instantiate what I support
    template bool SaveDataWithFormatToEXR<uint8_t>(const void*, const ExportInfo&);
    template bool SaveDataWithFormatToEXR<uint16_t>(const void*, const ExportInfo&);
    template bool SaveDataWithFormatToEXR<float>(const void*, const ExportInfo&);

    bool DispatchTemplateExport(ResourceFormat format, const void* data, const ExportInfo& info)
    {
        // Check format and call appropriate template instantiation
        switch (format)
        {
        case ResourceFormat::R8_UNORM:
        case ResourceFormat::RG8_UNORM:
        case ResourceFormat::RGBA8_UNORM:
            return SaveDataWithFormatToEXR<uint8_t>(data, info);
            break;
        case ResourceFormat::R16_FLOAT:
        case ResourceFormat::RG16_FLOAT:
        case ResourceFormat::RG16_SINT:
        case ResourceFormat::RGBA16_FLOAT:
            return SaveDataWithFormatToEXR<uint16_t>(data, info);
            break;
        case ResourceFormat::R32_UINT:
        case ResourceFormat::R32_FLOAT:
        case ResourceFormat::RG32_FLOAT:
        case ResourceFormat::RGBA32_FLOAT:
            return SaveDataWithFormatToEXR<float>(data, info);
            break;
        default:
            throw std::runtime_error("Unsupported format for FSR3 buffer export");
        }
        return false;
    }

    void TextureLoader::LoadAsync(void* pLoadParams)
    {
        // Validate there is at least one param instance
        TextureLoadParams* pParams = reinterpret_cast<TextureLoadParams*>(pLoadParams);
        if (pParams->LoadInfo.size() != 1)
        {
            CauldronError(L"Calling TextureLoader::LoadAsync with num LoadInfo != 1. Aborting read.");
            return;
        }

        // Copy the load parameters to use when loading textures
        TextureLoadParams* pTexLoadData = new TextureLoadParams();
        *pTexLoadData = *pParams;

        // Create a task completion callback to call in order to add textures to the content
        // manager once fully initialized and call the requester' callback
        TaskCompletionCallback* pLoadCompleteCallback = new TaskCompletionCallback(Task(&TextureLoader::AsyncLoadCompleteCallback, pTexLoadData), 1);

        // Enqueue the task to load content
        Task loadingTask(&TextureLoader::LoadTextureContent, &pTexLoadData->LoadInfo[0], pLoadCompleteCallback);
        GetTaskManager()->AddTask(loadingTask);
    }

    void TextureLoader::LoadMultipleAsync(void* pLoadParams)
    {
        TextureLoadParams* pParams = reinterpret_cast<TextureLoadParams*>(pLoadParams);

        // Copy the load parameters to use when loading textures
        TextureLoadParams* pTexLoadData = new TextureLoadParams();
        *pTexLoadData = *pParams;

        // Create a task completion callback to call in order to call the requester' callback
        TaskCompletionCallback* pLoadCompleteCallback = new TaskCompletionCallback(Task(&TextureLoader::AsyncLoadCompleteCallback, pTexLoadData), static_cast<uint32_t>(pTexLoadData->LoadInfo.size()));

        // Enqueue a task per texture to load the content
        std::queue<Task> taskList;
        for (size_t i = 0; i < pTexLoadData->LoadInfo.size(); ++i)
        {
            taskList.push(Task(&TextureLoader::LoadTextureContent, &pTexLoadData->LoadInfo[i], pLoadCompleteCallback));
        }
        GetTaskManager()->AddTaskList(taskList);
    }

    // Handler to load texture resources
    void TextureLoader::LoadTextureContent(void* pParam)
    {
        TextureLoadInfo& loadInfo = *reinterpret_cast<TextureLoadInfo*>(pParam);

        bool fileExists = filesystem::exists(loadInfo.TextureFile);
        CauldronAssert(ASSERT_ERROR, fileExists, L"Could not find texture file %ls. Please run ClearMediaCache.bat followed by UpdateMedia.bat to sync to latest media.", loadInfo.TextureFile.c_str());

        if (fileExists)
        {
            TextureDesc texDesc = {};

            // Figure fp16Data how to load this texture (whether it's a DDS or other)
            bool ddsFile = loadInfo.TextureFile.extension() == L".dds" || loadInfo.TextureFile.extension() == L".DDS";
            bool exrFile = loadInfo.TextureFile.extension() == L".exr" || loadInfo.TextureFile.extension() == L".EXR";
            TextureDataBlock* pTextureData;

            if (ddsFile)
            {
                pTextureData = new DDSTextureDataBlock();
            }
            else if (exrFile)
            {
                pTextureData = new EXRTextureDataBlock();
            }
            else
            {
                pTextureData = new WICTextureDataBlock();
            }
                
            bool loaded = pTextureData->LoadTextureData(loadInfo.TextureFile, loadInfo.AlphaThreshold, texDesc);

            CauldronAssert(ASSERT_ERROR, loaded, L"Could not load texture %ls (TextureDataBlock::LoadTextureData() failed)", loadInfo.TextureFile.c_str());
            if (loaded)
            {
                // We will use the relative path as the name of the asset since it's guaranteed to be unique
                texDesc.Name = loadInfo.TextureFile.c_str();

                // Pass along resource flags
                texDesc.Flags = static_cast<ResourceFlags>(loadInfo.Flags);

                // If SRGB was requested, apply format conversion
                if (loadInfo.SRGB)
                    texDesc.Format = ToGamma(texDesc.Format);

                Texture* pNewTexture = Texture::CreateContentTexture(&texDesc);
                CauldronAssert(ASSERT_ERROR, pNewTexture != nullptr, L"Could not create the texture %ls", texDesc.Name.c_str());
                if (pNewTexture != nullptr)
                {
                    pNewTexture->CopyData(pTextureData);

                    // Start managing the texture at this point
                    bool emplaced = GetContentManager()->StartManagingContent(texDesc.Name, pNewTexture);

                    // If it was emplaced, need to queue it up for a transition during the first graphics cmd list
                    if (emplaced)
                    {
                        // Now that the resource is ready, queue the resource change on the graphics queue for the next time it executes
                        Barrier textureTransition = Barrier::Transition(pNewTexture->GetResource(), ResourceState::CopyDest, ResourceState::PixelShaderResource | ResourceState::NonPixelShaderResource);
                        GetDevice()->ExecuteResourceTransitionImmediate(1, &textureTransition);
                    }

                    // if it wasn't emplaced, it's a duplicate and we can just delete it (callback will be called with previously loaded asset)
                    else
                    {
                        delete pNewTexture;
                    }
                }
            }

            delete pTextureData;
        }
    }

    // Completion handler to call when texture loads are all complete
    void TextureLoader::AsyncLoadCompleteCallback(void* pParam)
    {
        TextureLoadParams* pLoadParams = reinterpret_cast<TextureLoadParams*>(pParam);

        // If there was no callback, skip this work
        if (pLoadParams->LoadCompleteCallback)
        {
            // Build up a list of texture pointers to pass to the texture load callback function
            std::vector<const Texture*>   loadedTextures;
            std::vector<TextureLoadInfo>::iterator iter = pLoadParams->LoadInfo.begin();
            ContentManager* pContentManager = GetContentManager();
            while (iter != pLoadParams->LoadInfo.end())
            {
                loadedTextures.push_back(pContentManager->GetTexture(iter->TextureFile.c_str()));
                ++iter;
            }

            // Lastly, call the callback
            pLoadParams->LoadCompleteCallback(loadedTextures, pLoadParams->AdditionalParams);
        }

        // Delete the load parameters we allocated for the duration of the loading job
        delete pLoadParams;
    }


    // WICTextureDataBlock Implementation (Uses stb image loader)
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

    WICTextureDataBlock::~WICTextureDataBlock()
    {
        if (m_pData)
            free(m_pData);
    }

    float WICTextureDataBlock::GetAlphaCoverage(uint32_t width, uint32_t height, float scale, uint32_t alphaThreshold) const
    {
        double value = 0.0;

        uint32_t* pImgData = reinterpret_cast<uint32_t*>(m_pData);

        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                uint8_t* pPixel = reinterpret_cast<uint8_t*>(pImgData++);
                uint32_t alpha = static_cast<uint32_t>(scale * (float)pPixel[3]);
                if (alpha > 255)
                    alpha = 255;
                if (alpha <= alphaThreshold)
                    continue;

                value += alpha;
            }
        }

        return static_cast<float>(value / (height * width * 255));
    }

    void WICTextureDataBlock::ScaleAlpha(uint32_t width, uint32_t height, float scale)
    {
        uint32_t* pImgData = reinterpret_cast<uint32_t*>(m_pData);

        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                uint8_t* pPixel = reinterpret_cast<uint8_t*>(pImgData++);

                int32_t alpha = (int)(scale * (float)pPixel[3]);
                if (alpha > 255)
                    alpha = 255;

                pPixel[3] = alpha;
            }
        }
    }

    void WICTextureDataBlock::MipImage(uint32_t width, uint32_t height)
    {
        //compute mip so next call gets the lower mip
        int32_t offsetsX[] = { 0,1,0,1 };
        int32_t offsetsY[] = { 0,0,1,1 };

        uint32_t* pImgData = reinterpret_cast<uint32_t*>(m_pData);

#define GetByte(color, component) (((color) >> (8 * (component))) & 0xff)
#define GetColor(ptr, x,y) (ptr[(x)+(y)*width])
#define SetColor(ptr, x,y, col) ptr[(x)+(y)*width/2]=col;

        for (uint32_t y = 0; y < height; y += 2)
        {
            for (uint32_t x = 0; x < width; x += 2)
            {
                uint32_t ccc = 0;
                for (uint32_t c = 0; c < 4; ++c)
                {
                    uint32_t cc = 0;
                    for (uint32_t i = 0; i < 4; ++i)
                        cc += GetByte(GetColor(pImgData, x + offsetsX[i], y + offsetsY[i]), 3 - c);

                    ccc = (ccc << 8) | (cc / 4);
                }
                SetColor(pImgData, x / 2, y / 2, ccc);
            }
        }


        // For cutouts we need to scale the alpha channel to match the coverage of the top MIP map
        // otherwise cutouts seem to get thinner when smaller mips are used
        // Credits: http://www.ludicon.com/castano/blog/articles/computing-alpha-mipmaps/
        if (m_AlphaTestCoverage < 1.0)
        {
            float ini = 0;
            float fin = 10;
            float mid;
            float alphaPercentage;
            int iter = 0;
            for (; iter < 50; iter++)
            {
                mid = (ini + fin) / 2;
                alphaPercentage = GetAlphaCoverage(width / 2, height / 2, mid, (int)(m_AlphaThreshold * 255));

                if (fabs(alphaPercentage - m_AlphaTestCoverage) < .001)
                    break;

                if (alphaPercentage > m_AlphaTestCoverage)
                    fin = mid;
                if (alphaPercentage < m_AlphaTestCoverage)
                    ini = mid;
            }
            ScaleAlpha(width / 2, height / 2, mid);
        }

    }

    bool WICTextureDataBlock::LoadTextureData(filesystem::path& textureFile, float alphaThreshold, TextureDesc& texDesc)
    {
        std::string fileName = textureFile.u8string();

        int32_t channels;
        m_pData = reinterpret_cast<char*>(stbi_load(fileName.c_str(), reinterpret_cast<int32_t*>(&texDesc.Width), reinterpret_cast<int32_t*>(&texDesc.Height), &channels, STBI_rgb_alpha));

        if (!m_pData)
            return false;

        // Compute number of mips
        uint32_t mipWidth = texDesc.Width;
        uint32_t mipHeight = texDesc.Height;
        texDesc.MipLevels = 0;
        for (;;)
        {
            ++texDesc.MipLevels;
            if (mipWidth > 1)
                mipWidth >>= 1;
            if (mipHeight > 1)
                mipHeight >>= 1;
            if (mipWidth == 1 && mipHeight == 1)
                break;
        }

        // Fill in remaining texture information
        texDesc.DepthOrArraySize = 1;
        texDesc.Format = ResourceFormat::RGBA8_UNORM;
        texDesc.Dimension = TextureDimension::Texture2D;

        // If there is an alpha threshold, compute the alpha test coverage of the top mip
        // Mip generation will try to match this value so objects don't get thinner as they use lower mips
        m_AlphaThreshold = alphaThreshold;
        if (m_AlphaThreshold < 1.0f)
            m_AlphaTestCoverage = GetAlphaCoverage(texDesc.Width, texDesc.Height, 1.0f, (uint32_t)(255 * m_AlphaThreshold));
        else
            m_AlphaTestCoverage = 1.0f;

        return true;
    }

    void WICTextureDataBlock::CopyTextureData(void* pDest, uint32_t stride, uint32_t bytesWidth, uint32_t height, uint32_t readOffset)
    {
        for (uint32_t y = 0; y < height; ++y)
            memcpy((char*)pDest + y * stride, m_pData + y * bytesWidth, bytesWidth);

        // Generate the next mip in the chain to read
        MipImage(bytesWidth / 4, height);
    }

    // EXR Loader Implementation (Uses TinyEXR)
    EXRTextureDataBlock::~EXRTextureDataBlock()
    {
       delete [] m_pData;
    }

    bool EXRTextureDataBlock::LoadTextureData(filesystem::path& textureFile, float alphaThreshold, TextureDesc& texDesc)
    {
        // Set texture format at the very beginning
        texDesc.Format = this->m_Format;

        textureName          = textureFile.wstring();
        std::string fileName = textureFile.u8string();

        // Modern TinyEXR API
        const char* err = nullptr;
        EXRHeader   header;
        EXRImage    image;
        InitEXRHeader(&header);
        InitEXRImage(&image);

        // 1. Parse version
        EXRVersion version;
        int        ret = ParseEXRVersionFromFile(&version, fileName.c_str());
        if (ret != TINYEXR_SUCCESS)
        {
            CauldronError(L"Invalid EXR version: %s", fileName.c_str());
            return false;
        }

        // 2. Parse header
        ret = ParseEXRHeaderFromFile(&header, &version, fileName.c_str(), &err);
        if (ret != TINYEXR_SUCCESS)
        {
            if (err)
            {
                CauldronError(L"EXR header error: %s", err);
            }
            return false;
        }

        // 3. Ensure tinyexr read as FP16 according to the spec
        for (int i = 0; i < header.num_channels; i++)
        {
            CauldronAssert(ASSERT_WARNING, header.requested_pixel_types[i] == TINYEXR_PIXELTYPE_HALF, L"Input spec says each RGB channel is 16 bits.");
        }

        // 4. Load image data
        ret = LoadEXRImageFromFile(&image, &header, fileName.c_str(), &err);
        if (ret != TINYEXR_SUCCESS)
        {
            if (err)
            {
                CauldronError(L"EXR load error: %s", err);
            }
            return false;
        }

        // 5. Find RGB channels (assume first 3 channels are RGB)
        int idxR = -1, idxG = -1, idxB = -1, idxA = -1;
        for (int c = 0; c < header.num_channels; c++)
        {
            if (strcmp(header.channels[c].name, "R") == 0)
                idxR = c;
            else if (strcmp(header.channels[c].name, "G") == 0)
                idxG = c;
            else if (strcmp(header.channels[c].name, "B") == 0)
                idxB = c;
            else if (strcmp(header.channels[c].name, "A") == 0)
                idxA = c;
        }

        // Default to first 3 channels if not found
        CauldronAssert(ASSERT_CRITICAL, idxR != -1 && idxG != -1 && idxB != -1, 
            L"EXR file %ls has missing (idx = -1) RGB channels: idxR = %d, idxG = %d, idxB = %d", 
            fileName.c_str(), idxR, idxG, idxB);

        // 6. Convert to target format
        const size_t pixelCount    = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);

        // Get channel pointers; tinyexr use uint16_t = unsigned short for FP16
        uint16_t* r = idxR != -1 ? reinterpret_cast<uint16_t*>(image.images[idxR]) : nullptr;
        uint16_t* g = idxG != -1 ? reinterpret_cast<uint16_t*>(image.images[idxG]) : nullptr;
        uint16_t* b = idxB != -1 ? reinterpret_cast<uint16_t*>(image.images[idxB]) : nullptr;
        uint16_t* a = idxA >=  0 ? reinterpret_cast<uint16_t*>(image.images[idxA]) : nullptr;
        CauldronAssert(ASSERT_CRITICAL, r != nullptr && g != nullptr && b != nullptr, 
            L"EXR file %ls has null channel pointers when converting to uint16_t: r = %p, g = %p, b = %p", 
            fileName.c_str(), r, g, b);

        // prepare FP16 1.0f constant
        tinyexr::FP32 fp32_ONE; fp32_ONE.f = 1.0f;
        const uint16_t      fp16_ONE = tinyexr::float_to_half_full(fp32_ONE).u;

        // first malloc byte array depending on format and upscale factor
        m_BytesPerPixel     = (m_Format == ResourceFormat::RGBA16_FLOAT) ? 8 : 4;
        char* finalCharData = static_cast<char*>(malloc(pixelCount * m_BytesPerPixel * m_UpscaleRatio * m_UpscaleRatio));
        std::memset(finalCharData, 0, pixelCount * m_BytesPerPixel * m_UpscaleRatio * m_UpscaleRatio);
        if (! finalCharData)
        {
           CauldronError(L"Failed to allocate memory for EXR texture data.");
            return false;
        }

        /// Texture should be stored "2D". 
        /// E.g. for 2x upscaling, the 1k texture will be stored in the top-left 0.5x0.5 area
        /// of the 4k render target buffer, instead of the top 0.25 x 1 area.
        switch (m_Format)
        {
        ///< 4-Component (RGBA) 32-bit (unsigned normalized) type.
        case ResourceFormat::RGBA8_UNORM:
        {
            // Convert FLOAT to RGBA8; Use uint32_t to pack bits
            uint32_t* fp32Data = reinterpret_cast<uint32_t*>(finalCharData);
            CauldronAssert(ASSERT_CRITICAL, finalCharData != nullptr && fp32Data != nullptr, L"Failed to reinterpret_cast for RGBA8_UNORM EXR texture.");
            
            size_t   idxSrc, idxDst;
            for (size_t i = 0; i < image.height; ++i)
            {
                for (size_t j = 0; j < image.width; ++j)
                {
                    idxSrc = i * image.width + j;
                    idxDst = i * image.width * m_UpscaleRatio + j;  // only fill in the top-left area

                    fp32Data[idxDst] = PackRGBA8(r[idxSrc], g[idxSrc], b[idxSrc], a ? a[idxSrc] : fp16_ONE);
                }
                // remaining (m_UpscaleRatio - 1.0) * width ratio will not be used thus no need to memset.
            }
            break;
        }
        ///< 4-Component (RGBA) 32-bit (unsigned normalized) type.
        /// TODO: use linear to PQ conversion instead of simple tone mapping
        case ResourceFormat::RGB10A2_UNORM:
        {
            // Convert FLOAT to RGBA8; Use uint32_t to pack bits
            uint32_t* fp32Data = reinterpret_cast<uint32_t*>(finalCharData);
            CauldronAssert(ASSERT_CRITICAL, finalCharData != nullptr && fp32Data != nullptr, L"Failed to reinterpret_cast for RGB10A2_UNORM EXR texture.");

            size_t idxSrc, idxDst;
            for (size_t i = 0; i < image.height; ++i)
            {
                for (size_t j = 0; j < image.width; ++j)
                {
                    idxSrc = i * image.width + j;
                    idxDst = i * image.width * m_UpscaleRatio + j;  // only fill in the top-left area

                    fp32Data[idxDst] = PackRGB10A2(r[idxSrc], g[idxSrc], b[idxSrc], a ? a[idxSrc] : fp16_ONE);
                }
                // remaining (m_UpscaleRatio - 1.0) * width ratio will not be used thus no need to memset.
            }
            break;
        }

        case ResourceFormat::RGBA16_FLOAT:
        {
            // Can directly use FP16
            uint16_t* fp16Data = reinterpret_cast<uint16_t*>(finalCharData);
            CauldronAssert(ASSERT_CRITICAL, finalCharData != nullptr && fp16Data != nullptr, 
                L"Failed to reinterpret_cast for RGBA16_FLOAT EXR texture.");

            size_t idxSrc, idxDst;
            for (size_t i = 0; i < image.height; ++i)
            {
                for (size_t j = 0; j < image.width; ++j)
                {
                    idxSrc = i * image.width + j;
                    idxDst = i * image.width * m_UpscaleRatio + j;  // only fill in the top-left area

                    fp16Data[4 * idxDst + 0] = r[idxSrc];
                    fp16Data[4 * idxDst + 1] = g[idxSrc];
                    fp16Data[4 * idxDst + 2] = b[idxSrc];
                    fp16Data[4 * idxDst + 3] = a ? a[idxSrc] : fp16_ONE;
                }
                // remaining (m_UpscaleRatio - 1.0) * width ratio will not be used thus no need to memset.
            }
            break;
        }

        default:
        {
            CauldronError(L"Invalid texture format: %s", GetResourceFormatString(this->m_Format));
            return false;
        }
        }  // end of switch

        // 7. Update texture data
        m_Width  = image.width;
        m_Height = image.height;

        // Store to member char* in the end in order not to pollute memory.
        if (m_pData)
            free(m_pData);
        m_pData = finalCharData;

        // 8. Set texture description
        texDesc.Width            = m_Width;
        texDesc.Height           = m_Height;
        texDesc.MipLevels        = 1;
        texDesc.DepthOrArraySize = 1;
        texDesc.Dimension        = TextureDimension::Texture2D;

        return true;
    }

    void EXRTextureDataBlock::CopyTextureData(void* pDest, uint32_t stride, uint32_t bytesWidth, uint32_t height, uint32_t readOffset)
    {
        for (uint32_t y = 0; y < height; ++y)
            memcpy((char*)pDest + y * stride, m_pData + y * bytesWidth, bytesWidth);
    }

    bool EXRTextureDataBlock::LoadJitterData1K(
        std::experimental::filesystem::path& textureFile, float alphaThreshold, TextureDesc& texDesc, SpecialChannelType channelType)
    {
        CauldronAssert(ASSERT_ERROR, channelType != SpecialChannelType::ColorRGB, L"RGB color should not be read by this function.");

        // Set the texture format based on channel type
        if (channelType == SpecialChannelType::MotionVectors)
        {
            texDesc.Format  = ResourceFormat::RG16_FLOAT;
            m_BytesPerPixel = 4;  // 2 channels × 2 bytes each
        }
        else
        {  // Depth
            texDesc.Format  = ResourceFormat::R32_FLOAT;
            m_BytesPerPixel = 4;  // 1 channel × 4 bytes
        }

        // Initialize EXR structures
        EXRVersion version;
        EXRHeader  header;
        EXRImage   image;
        InitEXRHeader(&header);
        InitEXRImage(&image);
        const char* err = nullptr;

        // Parse EXR version
        std::string fileName = textureFile.u8string();
        int         ret      = ParseEXRVersionFromFile(&version, fileName.c_str());
        if (ret != TINYEXR_SUCCESS)
        {
            CauldronError(L"Invalid EXR version: %s", fileName.c_str());
            return false;
        }

        // Parse EXR header
        ret = ParseEXRHeaderFromFile(&header, &version, fileName.c_str(), &err);
        if (ret != TINYEXR_SUCCESS)
        {
            if (err)
            {
                CauldronError(L"EXR header error: %s", err);
            }
            return false;
        }

        // Ensure tinyexr read as FP16 according to the spec
        for (int i = 0; i < header.num_channels; i++)
        {
            CauldronAssert(ASSERT_WARNING, header.requested_pixel_types[i] == TINYEXR_PIXELTYPE_HALF, L"Input spec says each RGB channel is 16 bits.");
        }

        // Load EXR image
        ret = LoadEXRImageFromFile(&image, &header, fileName.c_str(), &err);
        if (ret != TINYEXR_SUCCESS)
        {
            if (err)
            {
                CauldronError(L"EXR load error: %s", err);
            }
            return false;
        }

        // Find channel indices (R=motionX, G=motionY, B=depth)
        int idxR = -1, idxG = -1, idxB = -1;
        for (int c = 0; c < header.num_channels; c++)
        {
            if (strcmp(header.channels[c].name, "R") == 0)
                idxR = c;
            else if (strcmp(header.channels[c].name, "G") == 0)
                idxG = c;
            else if (strcmp(header.channels[c].name, "B") == 0)
                idxB = c;
        }

        // Validate required channels
        if (channelType == SpecialChannelType::MotionVectors && (idxR == -1 || idxG == -1))
        {
            CauldronError(L"Motion vectors require R and G channels in %ls", textureFile.c_str());
            return false;
        }
        if (channelType == SpecialChannelType::Depth && idxB == -1)
        {
            CauldronError(L"Depth requires B channel in %ls", textureFile.c_str());
            return false;
        }

        // Get channel pointers; tinyexr use uint16_t = unsigned short for FP16
        uint16_t* r = idxR != -1 ? reinterpret_cast<uint16_t*>(image.images[idxR]) : nullptr;
        uint16_t* g = idxG != -1 ? reinterpret_cast<uint16_t*>(image.images[idxG]) : nullptr;
        uint16_t* b = idxB != -1 ? reinterpret_cast<uint16_t*>(image.images[idxB]) : nullptr;
        CauldronAssert(ASSERT_CRITICAL, r != nullptr && g != nullptr && b != nullptr, 
            L"EXR file %ls has null channel pointers when converting to uint16_t: r = %p, g = %p, b = %p", 
            fileName.c_str(), r, g, b);
        
        /// Set input and output data size.
        /// Input is always 1k. 
        /// Output = 1k * m_UpscaleRatio = display resolution. This is how big to malloc.
        const size_t inputWidth  = static_cast<size_t>(image.width);
        const size_t inputHeight = static_cast<size_t>(image.height);
        CauldronAssert(ASSERT_ERROR, inputWidth == 1920 && inputHeight == 1080, L"Jitter EXR input must be 1k resolution.");
        const size_t outputWidth  = inputWidth * m_UpscaleRatio;
        const size_t outputHeight   = inputHeight * m_UpscaleRatio;

        // Allocate raw bytes array first, then reinterpret_cast to FP16 or FP32
        char* charData = static_cast<char*>(malloc(outputWidth * outputHeight * m_BytesPerPixel));
        if (!charData)
        {
            CauldronError(L"Memory allocation failed for %ls", textureFile.c_str());
            return false;
        }

        /// NOTE: jitter data is 1k fixed
        size_t idxSrc, idxDst;
        // used only when interpolation needed.
        float  x_orig, y_orig;
        size_t x0, x1, y0, y1;
        float  weight_x, weight_y;
        if (channelType == SpecialChannelType::MotionVectors)
        {
            uint16_t* fp16Data  = reinterpret_cast<uint16_t*>(charData);
            auto      scaleMV  = [](uint16_t value, float ratio) -> uint16_t {
                tinyexr::FP16 half;
                half.u            = value;
                tinyexr::FP32 flt = half_to_float(half);
                flt.f *= ratio;
                return float_to_half_full(flt).u;
            };

            for (int y = 0; y < inputHeight; y++)
            {
                for (int x = 0; x < inputWidth; x++)
                {
                    idxSrc = y * inputWidth + x;
                    idxDst = (y * outputWidth + x) * 2;  // each mv stored as 2 fp16

                    // ROOT cause of ghosting finally found: should scale by 0.5
                    fp16Data[idxDst]     = scaleMV(r[idxSrc], -0.5f);  // mv.X
                    fp16Data[idxDst + 1] = scaleMV(g[idxSrc], +0.5f);  // mv.Y
                }
            } // end iterating the image
        }
        else  // Depth processing
        {
            float* fp32Data = reinterpret_cast<float*>(charData);
            for (int y = 0; y < inputHeight; y++)
            {
                for (int x = 0; x < inputWidth; x++)
                {
                                    
                    idxSrc = y * inputWidth + x;
                    idxDst = y * outputWidth + x;  // each depth stored as 1 fp32

                    // no interpolation needed
                    tinyexr::FP16 depth16 = {b[idxSrc]};
                    fp32Data[idxDst]      = tinyexr::half_to_float(depth16).f;
                }
            } // end iterating the image
        }

        // Set class members
        if (m_pData)
            free(m_pData);
        m_pData = charData;  // Store as char*

        m_Width     = outputWidth * m_UpscaleRatio;
        m_Height    = outputHeight * m_UpscaleRatio;
        textureName = textureFile.wstring();

        // Fill texture description
        texDesc.Width            = outputWidth;
        texDesc.Height           = outputHeight;
        texDesc.MipLevels        = 1;
        texDesc.DepthOrArraySize = 1;
        texDesc.Dimension        = TextureDimension::Texture2D;

        return true;
    }

    bool EXRTextureDataBlock::CreateDebugCoordinateTexture(TextureDesc& texDesc)
    {
        const int width  = 3840;
        const int height = 2160;

        // USE THE WORKING FORMAT - RGBA8_UNORM
        texDesc.Format         = ResourceFormat::RGBA8_UNORM;
        size_t m_BytesPerPixel = 4;  // 1 byte per channel × 4 channels

        const size_t pixelCount = static_cast<size_t>(width) * height;
        char*        out        = static_cast<char*>(malloc(pixelCount * m_BytesPerPixel));

        // Set entire texture to white (255 in all channels)
        memset(out, 255, pixelCount * m_BytesPerPixel);

        // Create a distinct red region in top-left (200x200 pixels)
        const int markerSize = 50;
        for (int y = 0; y < markerSize; y++)
        {
            for (int x = 0; x < markerSize * 4; x++)
            {
                const int idx = (y * width + x) * 4;
                out[idx]      = 255;  // R - full intensity
                out[idx + 1]  = 0;    // G - none
                out[idx + 2]  = 0;    // B - none
                out[idx + 3]  = 255;  // A - full opacity
            }
        }

        // Set class members - USE CHAR* COMPATIBLE TYPE
        m_pData = out;

        // Fill texture description
        texDesc.Width            = width;
        texDesc.Height           = height;
        texDesc.MipLevels        = 1;
        texDesc.DepthOrArraySize = 1;
        texDesc.Dimension        = TextureDimension::Texture2D;
        texDesc.Format           = ResourceFormat::RGBA8_UNORM;

        return true;
    }

    size_t EXRTextureDataBlock::TraverseFolder(std::wstring                          folderPath,
                                               std::vector<filesystem::path>&        outPaths,
                                               bool                                  extractJitter,
                                               std::vector<std::pair<float, float>>& jitterXY)
    {
        outPaths.clear();
        for (const auto& entry : filesystem::directory_iterator(folderPath))
        {
            if (entry.path().extension() == ".exr")
            {
                outPaths.push_back(entry.path());
            }
        }
        // Sort files to ensure proper frame order (assuming filenames contain frame numbers)
        std::sort(outPaths.begin(), outPaths.end());

        // Then iterate the sorted list to keey the jitter order consistent
        if (extractJitter)
        {
            // this func is also called when reading MV and Depths, so we clear conditionally.
            jitterXY.clear();
            for (const auto& entry : outPaths)
            {
                /// Example: NPP_beauty_2472_0000_0_-0.40563965_-0.35599041
                /// NOTE: both XY are .8f with range in [-0.5, 0.5]
                try
                {
                    std::string pathStr = entry.stem().generic_string();

                    size_t lastDelim       = pathStr.find_last_of('_');
                    size_t secondLastDelim = pathStr.find_last_of('_', lastDelim - 1);
                    CauldronAssert(ASSERT_ERROR,
                                   lastDelim != std::string::npos && secondLastDelim != std::string::npos,
                                   L"EXR jitter filename %ls does not have expected number of underscores.",
                                   pathStr);

                    // 2nd-last X, last Y
                    jitterXY.push_back(
                        {std::stof(pathStr.substr(secondLastDelim + 1, lastDelim - secondLastDelim - 1)), std::stof(pathStr.substr(lastDelim + 1))});
                }
                catch (const std::exception& e)
                {
                    CauldronError(L"%s", e.what());
                }
            }
        }

        return outPaths.size();
    }


// Needed for DDS loading
#include <dxgiformat.h>

    struct DDS_PIXELFORMAT
    {
        UINT32 size;
        UINT32 flags;
        UINT32 fourCC;
        UINT32 bitCount;
        UINT32 bitMaskR;
        UINT32 bitMaskG;
        UINT32 bitMaskB;
        UINT32 bitMaskA;
    };

    struct DDS_HEADER
    {

        UINT32          dwSize;
        UINT32          dwHeaderFlags;
        UINT32          dwHeight;
        UINT32          dwWidth;
        UINT32          dwPitchOrLinearSize;
        UINT32          dwDepth;
        UINT32          dwMipMapCount;
        UINT32          dwReserved1[11];
        DDS_PIXELFORMAT ddspf;
        UINT32          dwSurfaceFlags;
        UINT32          dwCubemapFlags;
        UINT32          dwCaps3;
        UINT32          dwCaps4;
        UINT32          dwReserved2;
    };

    ResourceFormat DXGIToResourceFormat(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_UNKNOWN:
            return ResourceFormat::Unknown;
        case DXGI_FORMAT_R16_FLOAT:
            return ResourceFormat::R16_FLOAT;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            return ResourceFormat::RGBA8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_SNORM:
            return ResourceFormat::RGBA8_SNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return ResourceFormat::RGBA8_SRGB;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return ResourceFormat::RGB10A2_UNORM;
        case DXGI_FORMAT_R16G16_FLOAT:
            return ResourceFormat::RG16_FLOAT;
        case DXGI_FORMAT_R32_FLOAT:
            return ResourceFormat::R32_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_UNORM:
            return ResourceFormat::RGBA16_UNORM;
        case DXGI_FORMAT_R16G16B16A16_SNORM:
            return ResourceFormat::RGBA16_SNORM;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return ResourceFormat::RGBA16_FLOAT;
        case DXGI_FORMAT_R32G32_FLOAT:
            return ResourceFormat::RG32_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            return ResourceFormat::RGBA32_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
            return ResourceFormat::RGBA32_TYPELESS;
        case DXGI_FORMAT_D16_UNORM:
            return ResourceFormat::D16_UNORM;
        case DXGI_FORMAT_D32_FLOAT:
            return ResourceFormat::D32_FLOAT;
        case DXGI_FORMAT_BC1_UNORM:
            return ResourceFormat::BC1_UNORM;
        case DXGI_FORMAT_BC1_UNORM_SRGB:
            return ResourceFormat::BC1_SRGB;
        case DXGI_FORMAT_BC2_UNORM:
            return ResourceFormat::BC2_UNORM;
        case DXGI_FORMAT_BC2_UNORM_SRGB:
            return ResourceFormat::BC2_SRGB;
        case DXGI_FORMAT_BC3_UNORM:
            return ResourceFormat::BC3_UNORM;
        case DXGI_FORMAT_BC3_UNORM_SRGB:
            return ResourceFormat::BC3_SRGB;
        case DXGI_FORMAT_BC4_UNORM:
            return ResourceFormat::BC4_UNORM;
        case DXGI_FORMAT_BC4_SNORM:
            return ResourceFormat::BC4_SNORM;
        case DXGI_FORMAT_BC5_UNORM:
            return ResourceFormat::BC5_UNORM;
        case DXGI_FORMAT_BC5_SNORM:
            return ResourceFormat::BC5_SNORM;
        case DXGI_FORMAT_BC6H_UF16:
            return ResourceFormat::BC6_UNSIGNED;
        case DXGI_FORMAT_BC6H_SF16:
            return ResourceFormat::BC6_SIGNED;
        case DXGI_FORMAT_BC7_UNORM:
            return ResourceFormat::BC7_UNORM;
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return ResourceFormat::BC7_SRGB;
        default:
            CauldronCritical(L"Unsupported format detected in DXGIToResourceFormat(). Please file an issue for additional format support.");
            return ResourceFormat::Unknown;
        }
    }

    ResourceFormat GetResourceFormat(DDS_PIXELFORMAT pixelFmt)
    {
        if (pixelFmt.flags & 0x00000004)   //DDPF_FOURCC
        {
            // Check for D3DFORMAT enums being set here
            switch (pixelFmt.fourCC)
            {
            case '1TXD':         return ResourceFormat::BC1_UNORM;
            case '3TXD':         return ResourceFormat::BC2_UNORM;
            case '5TXD':         return ResourceFormat::BC3_UNORM;
            case 'U4CB':         return ResourceFormat::BC4_UNORM;
            case 'A4CB':         return ResourceFormat::BC4_SNORM;
            case '2ITA':         return ResourceFormat::BC5_UNORM;
            case 'S5CB':         return ResourceFormat::BC5_SNORM;
            case 36:             return ResourceFormat::RGBA16_UNORM;
            case 110:            return ResourceFormat::RGBA16_SNORM;
            case 111:            return ResourceFormat::R16_FLOAT;
            case 112:            return ResourceFormat::RG16_FLOAT;
            case 113:            return ResourceFormat::RGBA16_FLOAT;
            case 114:            return ResourceFormat::R32_FLOAT;
            case 115:            return ResourceFormat::RG32_FLOAT;
            case 116:            return ResourceFormat::RGBA32_FLOAT;
            default:
                CauldronError(L"Unsupported DDS_PIXELFORMAT requested. Please file an issue for additional format support.");
                return ResourceFormat::Unknown;
            }
        }
        else
        {
            switch (pixelFmt.bitMaskR)
            {
            case 0xff:        return ResourceFormat::RGBA8_UNORM;
            case 0x3ff:       return ResourceFormat::RGB10A2_UNORM;

            case 0:           //return DXGI_FORMAT_A8_UNORM;
            case 0x00ff0000:  return ResourceFormat::RGBA8_UNORM; // Temporarily modify to read atlas.dds
            case 0xffff:      //return DXGI_FORMAT_R16G16_UNORM;
            case 0x7c00:      //return DXGI_FORMAT_B5G5R5A1_UNORM;
            case 0xf800:      //return DXGI_FORMAT_B5G6R5_UNORM;
            default:          //return DXGI_FORMAT_UNKNOWN;
                CauldronError(L"Unsupported resource format requested. Please file an issue for additional format support.");
                return ResourceFormat::Unknown;
            };
        }
    }

    // DDSTextureDataBlock Implementation (Uses DDS loader)
    DDSTextureDataBlock::~DDSTextureDataBlock()
    {
        delete m_pData;
    }

    bool DDSTextureDataBlock::LoadTextureData(filesystem::path& textureFile, float alphaThreshold, TextureDesc& texDesc)
    {
        typedef enum RESOURCE_DIMENSION
        {
            RESOURCE_DIMENSION_UNKNOWN = 0,
            RESOURCE_DIMENSION_BUFFER = 1,
            RESOURCE_DIMENSION_TEXTURE1D = 2,
            RESOURCE_DIMENSION_TEXTURE2D = 3,
            RESOURCE_DIMENSION_TEXTURE3D = 4
        } RESOURCE_DIMENSION;

        typedef struct
        {
            DXGI_FORMAT         dxgiFormat;
            RESOURCE_DIMENSION  resourceDimension;
            UINT32              miscFlag;
            UINT32              arraySize;
            UINT32              reserved;
        } DDS_HEADER_DXT10;

        // Get the file size
        int64_t fileSize = GetFileSize(textureFile.c_str());
        int64_t rawTextureSize = fileSize;
        if (fileSize == -1)
        {
            CauldronError(L"Could not get file size of %ls", textureFile.c_str());
            return false;
        }

        // read the header
        constexpr int32_t c_HEADER_SIZE = 4 + sizeof(DDS_HEADER) + sizeof(DDS_HEADER_DXT10);
        char headerData[c_HEADER_SIZE];
        uint32_t bytesRead = 0;

        int64_t sizeRead = ReadFilePartial(textureFile.c_str(), headerData, c_HEADER_SIZE);
        if (sizeRead != c_HEADER_SIZE)
        {
            CauldronError(L"Error reading texture header data for file %ls", textureFile.c_str());
            return false;
        }

        char* pByteData = headerData;
        uint32_t magicNumber = *reinterpret_cast<uint32_t*>(pByteData);
        CauldronAssert(ASSERT_ERROR, magicNumber == ' SDD', L"DDSLoader could not find DDS indicator in header info");
        if (magicNumber != ' SDD')   // "DDS "
            return false;

        pByteData += 4;
        rawTextureSize -= 4;

        DDS_HEADER* pHeader = reinterpret_cast<DDS_HEADER*>(pByteData);
        pByteData += sizeof(DDS_HEADER);
        rawTextureSize -= sizeof(DDS_HEADER);

        texDesc.Width = pHeader->dwWidth;
        texDesc.Height = pHeader->dwHeight;
        texDesc.DepthOrArraySize = pHeader->dwDepth ? pHeader->dwDepth : 1;
        texDesc.MipLevels = pHeader->dwMipMapCount ? pHeader->dwMipMapCount : 1;
        texDesc.Dimension = TextureDimension::Texture2D;

        if (pHeader->ddspf.fourCC == '01XD')
        {
            DDS_HEADER_DXT10* pHeader10 = reinterpret_cast<DDS_HEADER_DXT10*>((char*)pHeader + sizeof(DDS_HEADER));
            rawTextureSize -= sizeof(DDS_HEADER_DXT10);

            // Surface format
            texDesc.Format = DXGIToResourceFormat(pHeader10->dxgiFormat);

            switch (pHeader10->resourceDimension)
            {
            case 2:
                texDesc.Dimension = TextureDimension::Texture1D;
                texDesc.Height = 1;
                break;
            case 3:
                // Is this a cube map
                if (pHeader10->miscFlag == 4)
                {
                    texDesc.Dimension = TextureDimension::CubeMap;
                    texDesc.DepthOrArraySize = pHeader10->arraySize * 6;
                }
                else
                {
                    texDesc.Dimension = TextureDimension::Texture2D;
                }
                break;
            case 4:
                texDesc.Dimension = TextureDimension::Texture3D;
                break;
            default:
                CauldronCritical(L"Unexpected Resource Dimension Encountered!");
                break;
            }            
        }
        else
        {
            if (pHeader->dwCubemapFlags == 0xfe00)
            {
                texDesc.DepthOrArraySize = 6;
                texDesc.Dimension = TextureDimension::CubeMap;
            }
            else
                texDesc.DepthOrArraySize = 1;

            texDesc.Format = GetResourceFormat(pHeader->ddspf);
        }

        // Read in the data representing the texture (remainder of the file after the header)
        m_pData = new char[rawTextureSize];
        sizeRead = ReadFilePartial(textureFile.c_str(), m_pData, rawTextureSize, fileSize - rawTextureSize);
        if (sizeRead != rawTextureSize)
        {
            delete[](m_pData);
            CauldronError(L"Error reading texture data for file %ls", textureFile.c_str());
            return false;
        }

        return true;
    }

    void DDSTextureDataBlock::CopyTextureData(void* pDest, uint32_t stride, uint32_t bytesWidth, uint32_t height, uint32_t readOffset)
    {
        for (uint32_t y = 0; y < height; ++y)
            memcpy((char*)pDest + y * stride, m_pData + readOffset + y * bytesWidth, bytesWidth);
    }

    // MemTextureDataBlock Implementation (Loads data to texture from memory)
    MemTextureDataBlock::~MemTextureDataBlock()
    {
        m_pData = nullptr;  // We don't own this data
    }

    bool MemTextureDataBlock::LoadTextureData(filesystem::path& textureFile, float alphaThreshold, TextureDesc& texDesc)
    {
        CauldronError(L"MemTextureDataBlock does not support calls to LoadTextureData.");
        return false;
    }

    void MemTextureDataBlock::CopyTextureData(void* pDest, uint32_t stride, uint32_t bytesWidth, uint32_t height, uint32_t readOffset)
    {
        for (uint32_t y = 0; y < height; ++y)
            memcpy((char*)pDest + y * stride, m_pData + readOffset + y * bytesWidth, bytesWidth);
    }    
} // namespace cauldron

