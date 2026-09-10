#pragma once
#include <filesystem>
// Test-only module location; never touches installed game settings.
namespace Util
{
std::filesystem::path DllPath();
}
