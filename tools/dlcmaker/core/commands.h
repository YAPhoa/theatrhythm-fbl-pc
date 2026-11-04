// SPDX-License-Identifier: GPL-2.0-or-later
// The DLC maker's command line: the window's exe runs it when its first argument is a command.
//
//     theatrhythm-dlcmaker new FOLDER            start a DLC folder with an example song
//     theatrhythm-dlcmaker check PATH            parse a DLC folder (or a workspace of them) and validate it
//     theatrhythm-dlcmaker build PATH --out DIR  build each DLC into DIR/<label>/
//     theatrhythm-dlcmaker install PATH --user USER   build into USER/addcont/ (the runner's save folder)
//     theatrhythm-dlcmaker extract GAMESONG FOLDER --game DIR   a game song as a song folder (private tests)
//
// The window also uses info, set, add-file, new-dlc, new-song and stage-data (records on stdout).
#pragma once

#include <string>
#include <vector>

namespace dlcmaker {

bool is_command(const std::string& arg);
int cli_main(const std::vector<std::string>& args); // args after the program name, UTF-8; the exit code

} // namespace dlcmaker
