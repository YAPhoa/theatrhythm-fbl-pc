// SPDX-License-Identifier: GPL-2.0-or-later
// One DLC folder -> one add-on pack folder (<label>/), the layout the runner mounts from <user>/addcont/.
//
// What the game reads (README.md, "How a build maps onto the game"): dlcparam.json; catalog.json (keys ->
// {DLC.LoadPath} bundles, CRC 0); 10 tables at Data/<PackageName>/<Table>.bytes; 5 text sources at
// Text/<PackageName>/<source>.prefab; per song Music/ (charts, music.wav, music_preview.wav), Jacket/,
// Textures/EMSThumbnail/ assets; per series Textures/SeriesLogo/ assets; event videos as plain files in
// StreamingAssets/Movie/EMS/.
//
// Pack bundles are clones of base bundles with fresh names and new content: every clone gets its own
// CAB name, AssetBundle name and container id (from a seed), with its .resS/.resource streams
// re-pointed (two bundles sharing names confuse the game's asset caches). Objects named after the
// source (MUSICnnnnnnn_jp_256, SERIESnnnnnnn) are renamed: loaders look up sub-assets by the new
// song's or series' name.
#pragma once

#include "game.h"
#include "project.h"

#include <functional>
#include <string>

namespace dlcmaker::build {

using Log = std::function<void(const std::string&)>;

// Decoded audio (and event video) for a song: fills song.frames, length_s and movie_out.
void prepare_song(project::Song& song, const std::string& ffmpeg, const fs::path& cache, const Log& log, bool video = true);
fs::path build(Game& game, const project::Dlc& dlc, const project::Ids& ids, const fs::path& out_root, const Log& log);

} // namespace dlcmaker::build
