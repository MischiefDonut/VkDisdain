/*
** sdlglvideo.cpp
**
**---------------------------------------------------------------------------
** Copyright 2005-2016 Christoph Oelckers et.al.
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

// HEADER FILES ------------------------------------------------------------

#include "i_module.h"
#include "i_soundinternal.h"
#include "i_system.h"
#include "i_video.h"
#include "i_interface.h"
#include "m_argv.h"
#include "v_video.h"
#include "version.h"
#include "c_console.h"
#include "c_dispatch.h"
#include "printf.h"

#include "hardware.h"

#ifdef HAVE_VULKAN
#include "vulkan/vk_renderdevice.h"
#include <zvulkan/vulkaninstance.h>
#include <zvulkan/vulkansurface.h>
#include <zvulkan/vulkandevice.h>
#include <zvulkan/vulkanbuilders.h>
#endif

// MACROS ------------------------------------------------------------------

#if defined HAVE_VULKAN
#include <SDL_vulkan.h>
#endif // HAVE_VULKAN

// TYPES -------------------------------------------------------------------

// PUBLIC FUNCTION PROTOTYPES ----------------------------------------------

// PRIVATE FUNCTION PROTOTYPES ---------------------------------------------

// EXTERNAL DATA DECLARATIONS ----------------------------------------------
extern IVideo *Video;

EXTERN_CVAR (Int, vid_adapter)
EXTERN_CVAR (Int, vid_displaybits)
EXTERN_CVAR (Int, vid_defwidth)
EXTERN_CVAR (Int, vid_defheight)
EXTERN_CVAR (Bool, cl_capfps)
EXTERN_CVAR(Bool, vk_debug)

// PUBLIC DATA DEFINITIONS -------------------------------------------------

CUSTOM_CVAR(Bool, gl_debug, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("This won't take effect until " GAMENAME " is restarted.\n");
}
CUSTOM_CVAR(Bool, gl_es, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("This won't take effect until " GAMENAME " is restarted.\n");
}

CUSTOM_CVAR(String, vid_sdl_render_driver, "", CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("This won't take effect until " GAMENAME " is restarted.\n");
}

CCMD(vid_list_sdl_render_drivers)
{
	for (int i = 0; i < SDL_GetNumRenderDrivers(); ++i)
	{
		SDL_RendererInfo info;
		if (SDL_GetRenderDriverInfo(i, &info) == 0)
			Printf("%s\n", info.name);
	}
}

// PRIVATE DATA DEFINITIONS ------------------------------------------------

namespace Priv
{
	SDL_Window *window;
	bool softpolyEnabled;
	bool fullscreenSwitch;
	int numberOfDisplays;
	SDL_Rect* displayBounds = nullptr;

	void updateDisplayInfo()
	{
		Priv::numberOfDisplays = SDL_GetNumVideoDisplays();
		if (Priv::numberOfDisplays <= 0) {
			Printf("%sWrong number of displays detected.\n", TEXTCOLOR_BOLD);
			return;
		}
		Printf("Number of detected displays %d .\n", Priv::numberOfDisplays);

		if (Priv::displayBounds != nullptr) {
			free(Priv::displayBounds);
		}
		Priv::displayBounds = (SDL_Rect*) calloc(Priv::numberOfDisplays, sizeof(SDL_Rect));

		for (int i=0; i < Priv::numberOfDisplays; i++) {
			if (0 != SDL_GetDisplayBounds(i, &Priv::displayBounds[i])) {
				Printf("%sError getting display %d size: %s\n", TEXTCOLOR_BOLD, i, SDL_GetError());
				if (i == 0) {
					free(Priv::displayBounds);
					displayBounds = nullptr;
				}
				Priv::numberOfDisplays = i;
				return;
			}
		}
	}

	void CreateWindow(uint32_t extraFlags)
	{
		assert(Priv::window == nullptr);

		// Get displays and default display size
		updateDisplayInfo();

		// TODO control better when updateDisplayInfo fails
		SDL_Rect* bounds = &displayBounds[vid_adapter % numberOfDisplays];

		if (win_w <= 0 || win_h <= 0)
		{
			win_w = bounds->w * 8 / 10;
			win_h = bounds->h * 8 / 10;
		}

		int xWindowPos = (win_x <= 0) ? SDL_WINDOWPOS_CENTERED_DISPLAY(vid_adapter) : win_x;
		int yWindowPos = (win_y <= 0) ? SDL_WINDOWPOS_CENTERED_DISPLAY(vid_adapter) : win_y;
		Printf("Creating window [%dx%d] on adapter %d\n", (*win_w), (*win_h), (*vid_adapter));
		
		FString caption;
		caption.Format(GAMENAME " %s (%s)", GetVersionString(), GetGitTime());

		const uint32_t windowFlags = (win_maximized ? SDL_WINDOW_MAXIMIZED : 0) | SDL_WINDOW_RESIZABLE | extraFlags;
		Priv::window = SDL_CreateWindow(caption.GetChars(), xWindowPos, yWindowPos, win_w, win_h, windowFlags);

		if (Priv::window != nullptr)
		{
			// Enforce minimum size limit
			SDL_SetWindowMinimumSize(Priv::window, VID_MIN_WIDTH, VID_MIN_HEIGHT);
			// Tell SDL to start sending text input on Wayland.
			if (strncasecmp(SDL_GetCurrentVideoDriver(), "wayland", 7) == 0) SDL_StartTextInput();
		}
	}

	void DestroyWindow()
	{
		assert(Priv::window != nullptr);

		SDL_DestroyWindow(Priv::window);
		Priv::window = nullptr;

		if (Priv::displayBounds != nullptr) {
			free(Priv::displayBounds);
			Priv::displayBounds = nullptr;
		}
	}
}

CUSTOM_CVAR(Int, vid_adapter, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
  if (Priv::window != nullptr) {
		// Get displays and default display size
		Priv::updateDisplayInfo();

    int display = (*self) % Priv::numberOfDisplays;

		// TODO control better when updateDisplayInfo fails
		SDL_Rect* bounds = &Priv::displayBounds[vid_adapter % Priv::numberOfDisplays];

		if (win_w <= 0 || win_h <= 0)
		{
			win_w = bounds->w * 8 / 10;
			win_h = bounds->h * 8 / 10;
		}
		// Forces to set to the ini this vars to -1, so +vid_adapter keeps working the next time that the game it's launched
		win_x = -1;
		win_y = -1;

		if ((SDL_GetWindowFlags(Priv::window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0) {

			// TODO This not works. For some reason keeps stuck on the previus screen
			/*
			SDL_DisplayMode currentDisplayMode;
			SDL_GetWindowDisplayMode(Priv::window, &currentDisplayMode);
			currentDisplayMode.w = win_w;
			currentDisplayMode.h = win_h;
			if ( 0 != SDL_SetWindowDisplayMode(Priv::window, &currentDisplayMode)) {
				Printf("A problem occured trying to change of display %s\n", SDL_GetError());
			}
			*/

			// TODO This workaround also isn't working
			/*
			SDL_SetWindowFullscreen(Priv::window, 0);
			SDL_SetWindowSize(Priv::window, win_w, win_h);
			SDL_SetWindowPosition(Priv::window, bounds->x , bounds->y);
			SDL_SetWindowFullscreen(Priv::window, SDL_WINDOW_FULLSCREEN_DESKTOP);
			*/
			Printf("Changing adapter on fullscreen, isn't full supported by SDL. Instead try to switch to windowed mode, change the adapter and then switch again to fullscreen.\n");

		} else {
			SDL_SetWindowSize(Priv::window, win_w, win_h);
			SDL_SetWindowPosition(Priv::window, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
		}

    display = SDL_GetWindowDisplayIndex(Priv::window);
    if (display >= 0) {
			Printf("New display is %d\n", display );
		} else {
			Printf("A problem occured trying to change of display %s\n", SDL_GetError());
		}
  }
}

class SDLVideo : public IVideo
{
public:
	SDLVideo ();
	~SDLVideo ();

	void DumpAdapters();
	
	DFrameBuffer *CreateFrameBuffer ();

private:
#ifdef HAVE_VULKAN
	std::shared_ptr<VulkanSurface> surface;
#endif
};

// CODE --------------------------------------------------------------------

#ifdef HAVE_VULKAN
void I_GetVulkanDrawableSize(int *width, int *height)
{
	assert(Priv::window != nullptr);
	SDL_Vulkan_GetDrawableSize(Priv::window, width, height);
}

bool I_GetVulkanPlatformExtensions(unsigned int *count, const char **names)
{
	assert(Priv::window != nullptr);
	return SDL_Vulkan_GetInstanceExtensions(Priv::window, count, names) == SDL_TRUE;
}

bool I_CreateVulkanSurface(VkInstance instance, VkSurfaceKHR *surface)
{
	assert(Priv::window != nullptr);
	return SDL_Vulkan_CreateSurface(Priv::window, instance, surface) == SDL_TRUE;
}
#endif


SDLVideo::SDLVideo ()
{
	if (SDL_Init(SDL_INIT_VIDEO) < 0)
	{
		fprintf(stderr, "Video initialization failed: %s\n", SDL_GetError());
		return;
	}

	// Fail gracefully if we somehow reach here after linking against a SDL2 library older than 2.0.6.
	if (!SDL_VERSION_ATLEAST(2, 0, 6))
	{
		I_FatalError("Only SDL 2.0.6 or later is supported.");
	}

#ifdef HAVE_VULKAN
	Priv::CreateWindow(SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN | (vid_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
#endif
}

SDLVideo::~SDLVideo ()
{
#ifdef HAVE_VULKAN
	surface.reset();
#endif
}

void SDLVideo::DumpAdapters()
{
	Priv::updateDisplayInfo();
  for (int i=0; i < Priv::numberOfDisplays; i++) {
    Printf("%s%d. [%dx%d @ (%d,%d)]\n",
        vid_adapter == i ? TEXTCOLOR_BOLD : "",
        i,
        Priv::displayBounds[i].w,
        Priv::displayBounds[i].h,
        Priv::displayBounds[i].x,
        Priv::displayBounds[i].y
      );
  }
}


DFrameBuffer *SDLVideo::CreateFrameBuffer ()
{
	SystemBaseFrameBuffer *fb = nullptr;

	// first try Vulkan, if that fails OpenGL
#ifdef HAVE_VULKAN

	unsigned int count = 64;
	const char* names[64];
	if (!RunningAsTool)
	{
		if (!I_GetVulkanPlatformExtensions(&count, names))
			VulkanError("I_GetVulkanPlatformExtensions failed");
	}

	VulkanInstanceBuilder builder;
	builder.DebugLayer(vk_debug);
	if (!RunningAsTool)
	{
		for (unsigned int i = 0; i < count; i++)
			builder.RequireExtension(names[i]);
	}
	auto instance = builder.Create();

	if (!RunningAsTool)
	{
		VkSurfaceKHR surfacehandle = nullptr;
		if (!I_CreateVulkanSurface(instance->Instance, &surfacehandle))
			VulkanError("I_CreateVulkanSurface failed");

		surface = std::make_shared<VulkanSurface>(instance, surfacehandle);
	}

	fb = new VulkanRenderDevice(nullptr, vid_fullscreen, instance, surface);
#endif

	return fb;
}


IVideo *gl_CreateVideo()
{
	return new SDLVideo();
}


// FrameBuffer Implementation -----------------------------------------------

SystemBaseFrameBuffer::SystemBaseFrameBuffer (void *, bool fullscreen)
: DFrameBuffer (vid_defwidth, vid_defheight)
{
	if (Priv::window != nullptr)
	{
		SDL_SetWindowFullscreen(Priv::window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
		SDL_ShowWindow(Priv::window);
	}
}

int SystemBaseFrameBuffer::GetClientWidth()
{
	int width = 0;


#ifdef HAVE_VULKAN
	SDL_Vulkan_GetDrawableSize(Priv::window, &width, nullptr);
#endif

	return width;
}

int SystemBaseFrameBuffer::GetClientHeight()
{
	int height = 0;

#ifdef HAVE_VULKAN
	SDL_Vulkan_GetDrawableSize(Priv::window, nullptr, &height);
#endif

	return height;
}

bool SystemBaseFrameBuffer::IsFullscreen ()
{
	return (SDL_GetWindowFlags(Priv::window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
}

void SystemBaseFrameBuffer::ToggleFullscreen(bool yes)
{
	SDL_SetWindowFullscreen(Priv::window, yes ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
	if ( !yes )
	{
		if ( !Priv::fullscreenSwitch )
		{
			Priv::fullscreenSwitch = true;
			vid_fullscreen = false;
		}
		else
		{
			Priv::fullscreenSwitch = false;
			SetWindowSize(win_w, win_h);
		}
	}
}

void SystemBaseFrameBuffer::SetWindowSize(int w, int h)
{
	if (w < VID_MIN_WIDTH || h < VID_MIN_HEIGHT)
	{
		w = VID_MIN_WIDTH;
		h = VID_MIN_HEIGHT;
	}
	win_w = w;
	win_h = h;
	if (vid_fullscreen)
	{
		vid_fullscreen = false;
	}
	else
	{
		win_maximized = false;
		SDL_SetWindowSize(Priv::window, w, h);
		SDL_SetWindowPosition(Priv::window, SDL_WINDOWPOS_CENTERED_DISPLAY(vid_adapter), SDL_WINDOWPOS_CENTERED_DISPLAY(vid_adapter));
		SetSize(GetClientWidth(), GetClientHeight());
		int x, y;
		SDL_GetWindowPosition(Priv::window, &x, &y);
		win_x = x;
		win_y = y;
		
	}
}


void ProcessSDLWindowEvent(const SDL_WindowEvent &event)
{
	switch (event.event)
	{
	extern bool AppActive;

	case SDL_WINDOWEVENT_FOCUS_GAINED:
		S_SetSoundPaused(1);
		AppActive = true;
		break;

	case SDL_WINDOWEVENT_FOCUS_LOST:
		S_SetSoundPaused(0);
		AppActive = false;
		break;

	case SDL_WINDOWEVENT_MOVED:
		if (!vid_fullscreen)
		{
			int top = 0, left = 0;
			SDL_GetWindowBordersSize(Priv::window, &top, &left, nullptr, nullptr);
			win_x = event.data1-left;
			win_y = event.data2-top;
		}
		break;

	case SDL_WINDOWEVENT_RESIZED:
		if (!vid_fullscreen && !Priv::fullscreenSwitch)
		{
			win_w = event.data1;
			win_h = event.data2;
		}
		break;

	case SDL_WINDOWEVENT_MAXIMIZED:
		win_maximized = true;
		break;

	case SDL_WINDOWEVENT_RESTORED:
		win_maximized = false;
		break;
	}
}


// each platform has its own specific version of this function.
void I_SetWindowTitle(const char* caption)
{
	if (caption)
	{
		SDL_SetWindowTitle(Priv::window, caption);
	}
	else
	{
		FString default_caption;
		default_caption.Format(GAMENAME " %s (%s)", GetVersionString(), GetGitTime());
		SDL_SetWindowTitle(Priv::window, default_caption.GetChars());
	}
}

