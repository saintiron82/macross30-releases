#pragma once
#include <string_view>

namespace macross30::battle_voice {
// Exact output of the title's filename formatter, e.g. po_rio/001390.
// Empty means no known subtitle. No allocation, disk access, or voice selection.
std::string_view Find(std::string_view selected_stem) noexcept;
}
