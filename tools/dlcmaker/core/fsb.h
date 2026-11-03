// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "util.h"

namespace dlcmaker::fsb {

// Writes the FSB5 Vorbis sample `data` as an Ogg Vorbis file `out`; returns its length in s.
double to_ogg(std::string_view data, const fs::path& eboot, const fs::path& out);

} // namespace dlcmaker::fsb
