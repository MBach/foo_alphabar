#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <foobar2000/SDK/foobar2000.h>

// Must sit next to foobar2000/ and pfc/: it includes them through ../
#include <columns_ui-sdk/ui_extension.h>
