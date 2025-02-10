/*
This is free and unencumbered software released into the public domain.

Anyone is free to copy, modify, publish, use, compile, sell, or
distribute this software, either in source code form or as a compiled
binary, for any purpose, commercial or non-commercial, and by any
means.

In jurisdictions that recognize copyright laws, the author or authors
of this software dedicate any and all copyright interest in the
software to the public domain. We make this dedication for the benefit
of the public at large and to the detriment of our heirs and
successors. We intend this dedication to be an overt act of
relinquishment in perpetuity of all present and future rights to this
software under copyright law.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.

For more information, please refer to <https://unlicense.org>
*/

//
// https://github.com/madame-rachelle/IsHandheld
//

#include <cstdlib>
#include <string>
#include <iostream>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.Profile.h>
#endif

bool IsHandheld()
{
	// First test the environment variable
	const char* steamDeckEnv = std::getenv("SteamDeck");
	if (steamDeckEnv && std::strcmp(steamDeckEnv, "1") == 0)
		return true;

	const char* steamDeckEnvAlt = std::getenv("STEAM_DECK");
	if (steamDeckEnvAlt && std::strcmp(steamDeckEnvAlt, "1") == 0)
		return true;

#ifdef _WIN32
	try
	{
		using namespace winrt::Windows::System::Profile;

		// Initialize WinRT
		winrt::init_apartment();

		// Check if the device is in tablet mode
		auto currentMode = AnalyticsInfo::VersionInfo().DeviceFamily();
		if (currentMode == L"Windows.Tablet")
		{
			return true;
		}
	}
	catch (...)
	{
		// If the WinRT API is unavailable or fails, assume not in tablet mode
		return false;
	}
#endif

	return false; // Default to desktop mode for non-Windows platforms
}
