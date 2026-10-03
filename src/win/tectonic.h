// Resolve the pinned Tectonic executable embedded in the application, extracting it to LocalAppData on first use.
#pragma once

#include "platform.h"

namespace vs {
namespace win {

std::wstring tectonicPath(string* error = nullptr);

}  // namespace win
}  // namespace vs
