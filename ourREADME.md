# AMD Super Resolution and Frame Generation Performance Test

## Overview

This repo is cloned from AMD's [FidelityFX SDK version 1.1.4](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55). Note that this is an older version SDK with commit date May 8, 2025. The latest version
as of August 2025 is version 2.0.0, which fundamentally changed the project structure and hide a large portion of source code.

The project builds a full power sample app which consists of many Render Modules (RM, Nvidia calls them Render Passes). Relevant changes were
made in only 2 locations:

- FSR API Render Module: located in [**samples/fsrapi**](samples/fsrapi/CMakeLists.txt)
- Texture Loader: located in [**framework/cauldron/framework/[inc | src]/core/loaders**](framework/cauldron/framework/inc/core/loaders/textureloader.h)

The FSR API RM exclusively manages super resolution (SR, AMD calls it upscaling) and frame generation (FG, AMD calls it frame interpolation).
This project reads in our test inputs, including frame capture, motion vectors, and Gbuffer data of 60 congituous frame, and hacks them into the corresponding buffer in the FSR API RM. The app will execute the traditional rendering pipeline as before (that is, load and render the same Sponza Palace scene), but using our hack inputs for SR and FG, and displays results of our inputs in the end.

## How to Build

You may follow the same instruction as specified in the FidelityFX README. For the interactive options when running the build-solution script, we recommend the following:

- Support runtime shader recompile? No
- Build the SDK as DLL? No
- Enter numbers of which samples to build. 1 (All), though 8 (FSR) should suffice

Note that [**UpdateMedia.bat**](UpdateMedia.bat) needs to be run first, which simply downloads a large (around 5GB) media folder containing all scene descriptions. It is still needed by the parts of the pipeline we didn't change before SR and FG.

Finally, you get a VS solution in **\<Project Root\>/build**

## How to Run

### Prepare Test Inputs

Ask TODO杨博文 for our test inputs. It should be a folder (or many folders, each being a test scene) with 5 subfolders: **MVD_JI, NPP_GT, NPP_JI, YPP_GT, YPP_GI**. Put it under media or other designated locations.

### Build Solution in VS

Make sure the startup project is "FFX_API_FSR": it should be **bold** in the Solution Explorer. Then to debug, use VS button "Local Windows Debugger"; to run, use "Start Without Debugging" (Ctrl + F5) or simply click **FFX_API_FSR_DX12D.exe** in **bin/**. This assumes no Cmdline args are passed.

### Runtime Options Setup

We inherit the SDK's recommended way to set up runtime options: through json configuration files. Our hack/test related options are located in [**cauldronconfig.json**](framework/cauldron/framework/config/cauldronconfig.json) under `HackOptions` field.

We have the following options:

- EnableHack: a global switch, default false. If false, the app will run in its original behavior, rendering Sponza Palace.
- Identifier: a string that helps you identify this run, default "UNDEFINED". Usually set to scene name.
- RenderResolution: a int that controls the render resolution in K, default 1. Only accepted values are 1, 2, and 4.
- ParseJitter: whether to parse and use the jitter data from input filenames, default false. Currently we only have it in 1K inputs, so it will be forced to false it render resolution is not 1K.
- HackPaths: **a single folder relative path** to the input frame capture folder, default *"../media/TEST_SCENE/NPP_JI"*. The path to the encoded MVs and Depths will be constructed automatically by replacing "NPP_JI" to "MVD_JI".
- StoreOutput: whether to store output (screenshots), default false.
- OutputMaxCount: number of frames to take screenshots, default 0. When StoreOutput is true and OutputMaxCount is missing or bigger than number of input frames, it will default to capture all frames.
- OutputPath: **a single folder relative path** to the output screenshots folder, like *"../media/TEST_SCENE/outputs"*

Cmdline parsing of those hack-related runtime options is also provided. It would become useful when you have multiple launches of different scenes, (otherwise you likely need to change the input and output paths in the config file for each scene.)

All bool options should be a single **-OptionaName**, and other options should be a **-OptionaName OptionValue** pair. An example single-run full command looks like:

```shell
# cd to <Project Root>/bin

./FFX_API_FSR_DX12D.exe -EnableHack \
                        -Identifier "Cmdline_TEST" \
                        -RenderResolution 1 \
                        -ParseJitter \
                        -HackPaths "../media/TEST_SCENE/NPP_JI" \
                        -StoreOutput \
                        -OutputMaxCount 5 \
                        -OutputPath "../media/TEST_SCENE/outputs"
```

Note that when the global flag **-EnableHack** is set, the Cmdline parser, which works after the JSON parser, will first reset all options to default values then parse. Otherwise, nothing will be parsed from Cmdline.

## Several Things to Note

### Resolutions

Under the SR context, there are 2 resolutions: render resolution and display resolution, or pre- and post-SR resolution. The default choice is upscaling rendered output (which is input to SR) from 1k render resolution to 4k display resolution. Display resolution is what you set in the settings page of a game. And by choosing the "DLSS quality", which is essentially a ratio, the game knows at what resolution to render.

The following block gives easy control over turning hacking on/off and the render resolution. Nothing else should be changed in the header or source file.

```cpp
// ##### For Hacking use ONLY #####
#define HACK_TEXTURES

#ifdef HACK_TEXTURES
#define HACK_INPUT_RES 1     // set render (not display) resolution ?K: 1k, 2k, or 4K

// For now we only have jitter info in 1k inputs.
#if HACK_INPUT_RES == 1
#define PARSE_JITTER true
#else
#define PARSE_JITTER false
#endif

#endif  // HACK_TEXTURES
// ##########
```

The default choice is 1k->4k, as our 1k inputs have additional jitter data. 2k inputs are not in the existing inputs. If needed, we need to handle the creation of them. Should it be upscaled from 1k inputs or downscaled from 4k inputs?

Also, the display resolution is fixed at 4k. Don't worry about not having 4k monitor, as the app window will scale properly. If you want to try other display resolutions, which should work in theory but haven't been carefully tested, change it in the [config JSON file](framework/cauldron/framework/config/cauldronconfig.json) under "Presentation".

### Inputs format

The detailed specification of the inputs can be found on TODO. In short, each frame has 2 input exr files: one is the frame capture, the other is encoded motion vectors (2d, in RG channel) and Gbuffer depths (1d, in B channel).

If in the future, we need new inputs, make sure to double check the loading functions.

- For exr files, it's in `EXRTextureDataBlock`
- For common LDR formats (like jpg, png, etc.) that can be handled by **stb_image**, use `WICTextureDataBlock`.
- For other yet to be handled formats, like .hdr, similar class extending `TextureDataBlock` like the 2 above is needed.

Please be very careful on other input format details, like whether tone mapping, normalization, etc. have been applied, and change the loader code accordingly.

### View Results

When the App is up, there will be 3 imgui control panels. Use F2 and F3 to close 2 unrelated. In the F1 window, you can:

- "effectively" turn off SR, by setting TODO to **NativeAA**, which is 1.0, no upscale. We cannot really turn off SR, because we hack the input to SR and FG will use the original render output if SR is turned off completely.
- turn of FG, by unchecking the TODO box.
- view interploated/generated frame only, by checking the TODO box.
- view the debug views, by checking the TODO box. This is very helpful. The top-left region is motion vectors and top-mid region is depths.

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

As shown in the code above, by "hack" we didn't mean to overwrite the data managed by `m_pTempTexture`, `m_pDepthTarget`, and `m_pMotionVectors`. Instead, when we bind them as input to the Upscale dispatch, we use our targets instead. This ensures our hacking is non-destructive.

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

This is not [inputs format](#inputs-format), but **how the SDK stores texture/buffer data in bytes**. There is a big `enum ResourceFormat` in [**renderdefines.h**](framework/cauldron/framework/inc/render/renderdefines.h) defining this carefully. See the comments there for details.

Thus, when changing or adding to the texture loader code, make your the byte arrangement is consistent to the texture format.
