#pragma once
#include "../../../include/imgui/imconfig.h"
// Headless widget validation uses stb fonts; production keeps FreeType.
#undef IMGUI_ENABLE_FREETYPE
#define IMGUI_ENABLE_STB_TRUETYPE
