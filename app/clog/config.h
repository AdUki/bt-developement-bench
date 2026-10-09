#pragma once

#include <map>
#include <stack>
#include <vector>
#include <string>

#include "parser.h"

namespace Config {

enum class Feature { yes, no, autodetect };

extern bool Debug;

extern std::vector<std::string> ConfigFilepaths; // check these filepaths for configuration files

extern Feature ExtendedColors; // enabled for 256 colors, disabled for ANSI colors
extern bool ParseJson;
extern bool WhiteBackground;
extern bool BoldColorSet;
extern bool UnderlineColorSet;
extern bool BackgroundColorSet; // only for ANSI colors

extern bool PrintElapsed;
extern unsigned MinElapsedSeconds;
extern unsigned MaxElapsedNewLines;
extern unsigned ElapsedNewLineSeconds;

extern Feature UsePager;
extern std::vector<std::string> PagerBin; // input is passed by stdin

extern std::vector<std::string> TailBin; // files to follow are appended to list

extern std::string SelectedGroup;

std::vector<Parser> parseConfigFiles(const std::string &filename, bool listGroups);
}
