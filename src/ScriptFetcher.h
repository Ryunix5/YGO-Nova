#pragma once
#include <cstdint>
#include <vector>

namespace edo {

// Download missing card scripts (c<code>.lua) from the ProjectIgnis
// CardScripts GitHub into assets/scripts/. Called at duel start with the
// handful of codes whose script isn't on disk (usually none), so players get
// cards newer than their install without re-downloading anything in bulk —
// the script-side twin of the on-demand card-art fetcher.
//
// Blocking but parallel (small worker pool) with short timeouts; a code the
// repo doesn't know (alt art, token) is skipped silently. Returns how many
// scripts were fetched and written. Windows-only (returns 0 elsewhere).
int fetchCardScripts(const std::vector<uint32_t>& codes);

} // namespace edo
