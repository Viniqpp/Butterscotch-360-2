// The XDK compiler defines _WIN32, so shared/vendor code takes its
// `#include <windows.h>` branch. On Xbox 360 the Win32 subset lives in xtl.h.
// src/xbox360-xdk is on the project include path ahead of the SDK headers.
#pragma once
#include <xtl.h>
