# AMD Super Resolution and Frame Generation Performance Test

## Overview

This repo is cloned from AMD's [FidelityFX SDK version 1.1.4](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55). Note that this is an older version SDK with commit date May 8, 2025. The latest version
as of August 2025 is version 2.0.0, which fundamentally changed the project structure and hide a large portion of source code.

***In the future*** we will rebase our code onto latest FSR release.

The project builds a full power sample app which consists of many Render Modules (RM, Nvidia calls them Render Passes). Relevant changes were
made mostly in 2 locations:

- FSR API Render Module: located in [**samples/fsrapi**](samples/fsrapi/CMakeLists.txt)
- Texture Loader: located in [**framework/cauldron/framework/[inc | src]/core/loaders**](framework/cauldron/framework/inc/core/loaders/textureloader.h)

The FSR API RM exclusively manages super resolution (SR, AMD calls it upscaling) and frame generation (FG, AMD calls it frame interpolation).
This project reads in our test inputs, including frame capture, motion vectors, and Gbuffer data of 60 congituous frame, and hacks them into the corresponding buffer in the FSR API RM. The app will execute the traditional rendering pipeline as before (that is, load and render the same Sponza Palace scene), but using our hack inputs for SR and FG, and displays results of our inputs in the end.

## How to Build

For ease of deveoplment, we created a [**BuildFullSolutionDX12.bat** script](BuildFullSolutionDX12.bat) which

- Auto select the correct build options for you.
- Include the entire SDK (that is, +`ffx-api` code) into the generated VS solution, such that you can read, modify, and build your changes to these low-evel api files all in VS. Originally, they are built as .dll or .lib in CMake stage and directly included into VS solution.

Note that [**UpdateMedia.bat**](UpdateMedia.bat) needs to be run first, which simply downloads a large (around 5GB) media folder containing all scene descriptions. It is still needed by the parts of the pipeline we didn't change before SR and FG (traditional rendering steps).

Finally, you get a VS solution in **\<Project Root\>/build**

## How to Run

### Prepare Test Inputs

Ask <bowen.yang> for our test inputs. It should be a folder (or many folders, each being a test scene) with at least 3 subfolders: **MVD_JI, NPP_GT, NPP_JI**. Put it under media or other designated locations.

### Build Solution in VS

Make sure the startup project is "FFX_API_FSR": it should be **bold** in the Solution Explorer. Then to debug, use VS button "Local Windows Debugger". But make sure you read the following section and properly set up the cmdline args or json config file before debugging.

### Runtime Options Setup

We inherit both ways from the SDK to set up runtime options: through json configuration files and through cmdline args. Our hack/test related options lives in [**cauldronconfig.json**](framework/cauldron/framework/config/cauldronconfig.json) under `HackOptions` field.

We have the following options:

- [Global Switch] EnableHack: default false. If false, the app will run in its original behavior, rendering Sponza Palace.
- DisplayResolution: accepts 2 formats. See [this section](#resolution-system-in-fsr) for more details. TL;DR: User specifies display resolution here, render resolution is auto determined by the pre-scaled input image size, and upscale ratio is internally decided.
  - `-DisplayResolution <width> <height>`. This is the general option and gives full flexibility. i.e. Except for extreme resolutions like 10x7, 19200x10800, users can choose any value, like 2880x1620. ***NOTE: this pair-format will not be parsed in the json config file under "HackOptions". Instead, set "Width" and "Height" under "Presentation" AND REMOVE the alias-format "DisplayResolution" in the file, as it will overwrite "Presentation" field.***
  - `-DisplayResolution <alias>`, where `<alias>` accepts valid values 1, 2, and 4. This is a handy alternative for the common 1K, 2K, and 4K settings. Alias-format is accepted in both json config file and cmdline.
- [Optional] ParseJitter: whether to parse and use the jitter data from input filenames, default false. Ensure filenames have it before setting it to true. (Hint: currently only NPP_JI files have it.)
- HackPaths: accepts 2 formats.
  - A single relative path to the scene testdata root, e.g. *"../media/TEST_SCENE"*. The paths to frame color data and MVD data (encoded MVs and Depths) will be constructed automatically by appending "NPP_JI" and "MVD_JI" respectively, which are the defaults when UE generates testdata.
  - A pair of relative paths to the color data and MVD data. In json config file they are a list `"HackPaths": [<color path>, <MVD path>]`. In cmdline they are spaced: `-HackPaths <color path> <MVD path>`.
- [Optional] StoreOutput: whether to export SR and SR+FG output frames to .exr files, default false.
- [Optional] OutputFrameCount: number of frames to load and run on. When not given or the given value is larger than total .exr file count in color input path, default to this value to run on all frames. ***It is used for debug ONLY**
- [Optional **if StoreOutput not given**] OutputPath: a relative path to the exported frames folder, e.g. *"../media/TEST_SCENE/outputs"*. We recommend using an output folder in the testdata root.
- [Optional] AlignFilename: if set, export filenames will be in order when put together with the reference result (should be in *NPP_GT*). A (reference, SR output, FG output) triplet would look like (*NPP_beauty_2472.exr, NPP_beauty_2472_Quality.exr, NPP_beauty_2472_Quality_fg.exr*). This helps looking at them in "chronological order" with image viewer. And after copying *NPP_GT* reference images to *OutputPath*, it would be convenient to compare results frame by frame in an image viewer. When false, the frameID in the export filenames will start from 0000.
- [Optional] Identifier: a custom identifier for export filenames. If not given or AlignFilename is set to true, it will be the common prefix of all color data, e.g. *NPP_beauty*.

Cmdline parsing of those hack-related runtime options is also provided. It would become useful when you have multiple launches of different scenes with a script, otherwise you need to change the input and output paths in the config file for each scene.

All bool options should be a single **-OptionName**, and other options should be a **-OptionName OptionValue** pair. A typical full command looks like:

```shell
# cd to <Project Root>/bin

./FFX_API_FSR_DX12D.exe -EnableHack \
                        -DisplayResolution 2560 1440 \  # or 2
                        -ParseJitter \
                        -HackPaths "../media/TEST_SCENE" \
                        -StoreOutput \
                        -OutputPath "../media/TEST_SCENE/outputs" \
                        -AlignFilename
```

Note that when the global flag **-EnableHack** is set, the Cmdline parser, which works after the JSON parser, will first reset all options to default values then parse. Otherwise, nothing will be parsed from Cmdline.

## Several Things to Note

### Resolution System in FSR

Both DLSS and AMD's FSR **internally** determine render resolution based on user-selected display resolution + SR mode (DLSS calls it DLSS mode, FSR calls it scale preset). FSR gives explicit upscale ratio of these modes.

```cpp
    enum class FSRScalePreset : uint32_t
    {
        NativeAA = 0,       // 1.0f
        Quality,            // 1.5f
        Balanced,           // 1.7f
        Performance,        // 2.f
        UltraPerformance,   // 3.f
        Custom              // 1.f - 3.f range
    };
```

But we want to specify both render and display resolution, and stop FSR from resizing render resolution to the value it computes. Thus, the hack is to force the mode to `Custom` and compute the custom ratio b/w display resolution (from "-DisplayResolution")
and render resolution (from the input image resolution). It is valid because this `FSRScalePreset` is solely used to compute the upscale ratio. In DLSS, this is a bit more complicated because a similar `enum class DLSSMode` is tightly binded to the pipeline and thus we have to set it carefully.

BTW, for FSR3 don't worry about not having 4k monitor when you want to test 4k, as the app window will scale properly and you will get 4K export.

### Inputs format

The detailed specification of the inputs can be found on [Our Confluence page: MTSS UE Dataset](https://confluence.mthreads.com/display/SWPM/MTSS+UE+Dataset). In short, each frame has 2 input exr files: one is the frame capture, the other is encoded motion vectors (2d, in RG channel) and Gbuffer depths (1d, in B channel).

If in the future, we need new inputs, make sure to double check the loading functions.

- For exr files, it's in `EXRTextureDataBlock`
- For common LDR formats (like jpg, png, etc.) that can be handled by **stb_image**, use `WICTextureDataBlock`.
- For other formats not supported yet, like .hdr, similar class extending `TextureDataBlock` like the 2 above is needed.

Please be very careful on other input format details, like whether tone mapping, normalization, etc. have been applied, and change the loader code accordingly.

## Key Concepts

### Render Texture vs Content Texture

The namings by AMD created quite some ambiguity. The content texture is clear: it's the commonsense textures read from texture files (this project uses .dds). Things like the texture of bricks are all content textures.

The render texture is not "textures used for texture sampling in rendering" (content textures are, and that's why it's confusing). It's general-purpose texture buffers. Or, simply think of them as CPU handles of GPU buffers. Render pipeline will read from and write to various GPU buffers, like computed RGB colors, motion vectors, GBuffer depths, etc. The CPU handles of the 3 aforementioned buffers are our hacking targets.

In **samples/fsrapi** code, you will also see "render target" or "RT", which is a much clearer name of render textures.

### How and What We "Hack"

For now, our test inputs for each frame consist of frame capture (float3 RGB), motion vectors (float2) and GBuffer depths (float). To avoid messing up existing render targets and breaking the entire pipeline, we created 3 additional render targets **for each frame**.

We hack the following 3 render targets that hold them:

```cpp
if (GetFramework()->GetConfig()->HackOptions.enableHack)
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
```

As shown in the code above, by "hack" we didn't mean to overwrite the data managed by `m_pTempTexture`, `m_pDepthTarget`, or `m_pMotionVectors`. Instead, when we bind them as input to the Upscale dispatch, we use our targets instead. This ensures our hacking is non-destructive.

If we have more supportive data to be used, we just follow the same logic: create additional render targets to store inputs, replace appropriate targets with our own at dispatch binding time. Here are all existing render targets and our additional targets stored in [**fsrapirendermodule.h**](samples/fsrapi/fsrapirendermodule.h). The vector size will be the frame number in the test scene.

```cpp
// FSR resources
const cauldron::Texture*  m_pColorTarget           = nullptr;
const cauldron::Texture*  m_pTonemappedColorTarget = nullptr;
const cauldron::Texture*  m_pTempTexture           = nullptr;
const cauldron::Texture*  m_pDepthTarget           = nullptr;
const cauldron::Texture*  m_pMotionVectors         = nullptr;
const cauldron::Texture*  m_pReactiveMask          = nullptr;
const cauldron::Texture*  m_pCompositionMask       = nullptr;
const cauldron::Texture*  m_pOpaqueTexture         = nullptr;
// and our hacking textures: frame_t_color, motion vectors, depth
std::vector<cauldron::Texture*> m_pHackColors = {};
std::vector<cauldron::Texture*> m_pHackMVs    = {};
std::vector<cauldron::Texture*> m_pHackDepths = {};
std::vector<std::pair<float, float>> m_pHackJitterXY    = {};
```

### Texture format

This is not [inputs format](#inputs-format), but **how the SDK stores texture/buffer data in bytes**. There is a big `enum class ResourceFormat` in [**renderdefines.h**](framework/cauldron/framework/inc/render/renderdefines.h#208) defining this carefully. See the comments there for details.

Thus, when changing or adding to the texture loader code, make your the byte arrangement consistent to the texture format.
