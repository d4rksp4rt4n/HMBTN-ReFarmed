/* Linux/Unix: load SDL2 at runtime with dlopen instead of linking it, so the executable
 * starts (and can run headless) on systems without SDL2 and can report a clear error. */
#pragma once
#include <dlfcn.h>

#define SDL_DYN_FUNCS(X) \
    X(SDL_ClearQueuedAudio) X(SDL_CloseAudioDevice) X(SDL_CreateRenderer) X(SDL_CreateTexture) \
    X(SDL_DestroyTexture) X(SDL_SetRenderTarget) X(SDL_SetTextureScaleMode) \
    X(SDL_CreateWindow) X(SDL_Delay) X(SDL_GameControllerClose) X(SDL_GameControllerGetAxis) \
    X(SDL_GameControllerGetButton) X(SDL_GameControllerGetButtonFromString) \
    X(SDL_GameControllerGetJoystick) X(SDL_GameControllerName) X(SDL_GameControllerOpen) X(SDL_GameControllerRumble) \
    X(SDL_GetCurrentAudioDriver) X(SDL_GetError) X(SDL_GetKeyboardState) X(SDL_GetModState) \
    X(SDL_GetNumAudioDevices) X(SDL_GetPerformanceCounter) X(SDL_GetPerformanceFrequency) \
    X(SDL_GetQueuedAudioSize) X(SDL_GetRendererInfo) X(SDL_GetRendererOutputSize) \
    X(SDL_GetScancodeFromName) X(SDL_Init) X(SDL_IsGameController) X(SDL_JoystickInstanceID) \
    X(SDL_NumJoysticks) X(SDL_OpenAudioDevice) X(SDL_PauseAudioDevice) X(SDL_PollEvent) \
    X(SDL_QueueAudio) X(SDL_Quit) X(SDL_RenderClear) X(SDL_RenderCopy) X(SDL_RenderCopyF) X(SDL_SetTextureBlendMode) X(SDL_RenderPresent) \
    X(SDL_SetHint) X(SDL_SetRenderDrawColor) X(SDL_SetWindowFullscreen) X(SDL_SetWindowTitle) \
    X(SDL_ShowSimpleMessageBox) X(SDL_UpdateTexture) X(SDL_memset) \
    X(SDL_CreateSemaphore) X(SDL_CreateThread) X(SDL_SemPost) X(SDL_SemTryWait) X(SDL_SemWait) \
    X(SDL_JoystickGetDeviceInstanceID) X(SDL_SetThreadPriority) X(SDL_GetDisplayUsableBounds) \
    X(SDL_RaiseWindow) X(SDL_SetWindowPosition) X(SDL_GetWindowID) \
    X(SDL_DestroyRenderer) X(SDL_GetWindowWMInfo) X(SDL_Vulkan_GetDrawableSize) \
    X(SDL_GetCurrentDisplayMode) X(SDL_GetWindowDisplayIndex)

#define SDL_DYN_DECL(f) static __typeof__(&f) dyn_##f;
SDL_DYN_FUNCS(SDL_DYN_DECL)

static const char *sdl_dyn_load(void) {
    static const char *names[] = { "libSDL2-2.0.so.0", "libSDL2-2.0.so", "libSDL2.so" };
    void *h = NULL;
    for (unsigned i = 0; i < sizeof names / sizeof *names && !h; i++) h = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL);
    if (!h) return "SDL2 was not found. Please install it (e.g. 'sudo apt install libsdl2-2.0-0', "
                   "'sudo dnf install SDL2' or 'sudo pacman -S sdl2').";
#define SDL_DYN_LOAD(f) if (!(dyn_##f = (__typeof__(dyn_##f))dlsym(h, #f))) return "SDL2 is too old: missing " #f;
    SDL_DYN_FUNCS(SDL_DYN_LOAD)
    return NULL;
}

/* route every SDL call in platform_sdl.c through the loaded pointers */
#define SDL_ClearQueuedAudio dyn_SDL_ClearQueuedAudio
#define SDL_JoystickGetDeviceInstanceID dyn_SDL_JoystickGetDeviceInstanceID
#define SDL_SetThreadPriority dyn_SDL_SetThreadPriority
#define SDL_GetDisplayUsableBounds dyn_SDL_GetDisplayUsableBounds
#define SDL_RaiseWindow dyn_SDL_RaiseWindow
#define SDL_SetWindowPosition dyn_SDL_SetWindowPosition
#define SDL_GetWindowID dyn_SDL_GetWindowID
#define SDL_DestroyRenderer dyn_SDL_DestroyRenderer
#define SDL_GetCurrentDisplayMode dyn_SDL_GetCurrentDisplayMode
#define SDL_GetWindowDisplayIndex dyn_SDL_GetWindowDisplayIndex
#define SDL_GetWindowWMInfo dyn_SDL_GetWindowWMInfo
#define SDL_Vulkan_GetDrawableSize dyn_SDL_Vulkan_GetDrawableSize
#define SDL_CloseAudioDevice dyn_SDL_CloseAudioDevice
#define SDL_CreateRenderer dyn_SDL_CreateRenderer
#define SDL_CreateTexture dyn_SDL_CreateTexture
#define SDL_CreateWindow dyn_SDL_CreateWindow
#define SDL_DestroyTexture dyn_SDL_DestroyTexture
#define SDL_SetRenderTarget dyn_SDL_SetRenderTarget
#define SDL_SetTextureScaleMode dyn_SDL_SetTextureScaleMode
#define SDL_Delay dyn_SDL_Delay
#define SDL_CreateSemaphore dyn_SDL_CreateSemaphore
#define SDL_CreateThread dyn_SDL_CreateThread
#define SDL_SemPost dyn_SDL_SemPost
#define SDL_SemTryWait dyn_SDL_SemTryWait
#define SDL_SemWait dyn_SDL_SemWait
#define SDL_GameControllerClose dyn_SDL_GameControllerClose
#define SDL_GameControllerGetAxis dyn_SDL_GameControllerGetAxis
#define SDL_GameControllerGetButton dyn_SDL_GameControllerGetButton
#define SDL_GameControllerGetButtonFromString dyn_SDL_GameControllerGetButtonFromString
#define SDL_GameControllerGetJoystick dyn_SDL_GameControllerGetJoystick
#define SDL_GameControllerName dyn_SDL_GameControllerName
#define SDL_GameControllerOpen dyn_SDL_GameControllerOpen
#define SDL_GameControllerRumble dyn_SDL_GameControllerRumble
#define SDL_GetCurrentAudioDriver dyn_SDL_GetCurrentAudioDriver
#define SDL_GetError dyn_SDL_GetError
#define SDL_GetKeyboardState dyn_SDL_GetKeyboardState
#define SDL_GetModState dyn_SDL_GetModState
#define SDL_GetNumAudioDevices dyn_SDL_GetNumAudioDevices
#define SDL_GetPerformanceCounter dyn_SDL_GetPerformanceCounter
#define SDL_GetPerformanceFrequency dyn_SDL_GetPerformanceFrequency
#define SDL_GetQueuedAudioSize dyn_SDL_GetQueuedAudioSize
#define SDL_GetRendererInfo dyn_SDL_GetRendererInfo
#define SDL_GetRendererOutputSize dyn_SDL_GetRendererOutputSize
#define SDL_GetScancodeFromName dyn_SDL_GetScancodeFromName
#define SDL_Init dyn_SDL_Init
#define SDL_IsGameController dyn_SDL_IsGameController
#define SDL_JoystickInstanceID dyn_SDL_JoystickInstanceID
#define SDL_NumJoysticks dyn_SDL_NumJoysticks
#define SDL_OpenAudioDevice dyn_SDL_OpenAudioDevice
#define SDL_PauseAudioDevice dyn_SDL_PauseAudioDevice
#define SDL_PollEvent dyn_SDL_PollEvent
#define SDL_QueueAudio dyn_SDL_QueueAudio
#define SDL_Quit dyn_SDL_Quit
#define SDL_RenderClear dyn_SDL_RenderClear
#define SDL_RenderCopy dyn_SDL_RenderCopy
#define SDL_RenderCopyF dyn_SDL_RenderCopyF
#define SDL_SetTextureBlendMode dyn_SDL_SetTextureBlendMode
#define SDL_RenderPresent dyn_SDL_RenderPresent
#define SDL_SetHint dyn_SDL_SetHint
#define SDL_SetRenderDrawColor dyn_SDL_SetRenderDrawColor
#define SDL_SetWindowFullscreen dyn_SDL_SetWindowFullscreen
#define SDL_SetWindowTitle dyn_SDL_SetWindowTitle
#define SDL_ShowSimpleMessageBox dyn_SDL_ShowSimpleMessageBox
#define SDL_UpdateTexture dyn_SDL_UpdateTexture
#undef SDL_memset
#define SDL_memset dyn_SDL_memset
