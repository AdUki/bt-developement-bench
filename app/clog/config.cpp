#include "config.h"
#include "optional.h"

#include <fstream>
#include <iostream>
#include <set>
#include <list>

#ifdef CONFIG_STRING_VAR
extern std::string CONFIG_STRING_VAR;
#endif

namespace Config {

bool Debug = false;

std::vector<std::string> ConfigFilepaths = {
	"/etc/clog.conf",
	"/usr/share/clog/clog.conf",
	"~/.clog",
};

Feature ExtendedColors = Feature::autodetect; // enabled for 256 colors, disabled for ANSI colors
bool ParseJson = true;
bool WhiteBackground = false;
bool BoldColorSet = true;
bool UnderlineColorSet = true;
bool BackgroundColorSet = false; // only for ANSI colors

bool PrintElapsed = true;
unsigned MinElapsedSeconds = 5;
unsigned MaxElapsedNewLines = 10;
unsigned ElapsedNewLineSeconds = 5;

Feature UsePager = Feature::autodetect;
std::vector<std::string> PagerBin = { "sh", "-c", "less -R || more" }; // input is passed by stdin

std::vector<std::string> TailBin = { "tail", "-F" }; // files to follow is appended to list

std::string SelectedGroup;

struct Group
{
	Clog::optional<Feature> ExtendedColors;
	Clog::optional<bool> ParseJson;
	Clog::optional<bool> WhiteBackground;
	Clog::optional<bool> BoldColorSet;
	Clog::optional<bool> UnderlineColorSet;
	Clog::optional<bool> BackgroundColorSet;
	Clog::optional<bool> PrintElapsed;
	Clog::optional<unsigned> MinElapsedSeconds;
	Clog::optional<unsigned> MaxElapsedNewLines;
	Clog::optional<unsigned> ElapsedNewLineSeconds;
	Clog::optional<Feature> UsePager;
	std::vector<std::string> PagerBin;
	std::vector<std::string> TailBin;
	std::vector<Parser> Parsers;
	std::vector<Clog::regex> FileRegexps;
	std::list<std::string> Includes = { "" };
};

std::string parseConfigValue(const std::string &str)
{
	std::stringstream ss;
	std::stringstream whitespace;
	bool empty = true;
	bool escape = false;
	bool comment = false;
	char quoteChar = 0;
	for (const char c : str) {
		switch (c) {

		// quote string
		case '"':
		case '\'':
			if (escape)
				escape = false;
			else if (!quoteChar) {
				whitespace.clear();
				quoteChar = c;
				continue;
			} else if (quoteChar == c) {
				quoteChar = 0;
				continue;
			}
			break;

		// escape
		case '\\':
			if (escape)
				escape = false;
			else {
				escape = true;
				continue;
			}
			break;

		// comment
		case '#':
			if (escape)
				escape = false;
			else if (!quoteChar)
				return ss.str();
			break;

		// whitespace
		case ' ':
		case '\t':
			if (escape)
				escape = false;
			else if (!quoteChar) {
				if (!empty)
					whitespace << c;
				continue;
			}
			break;

		default:
			if (escape) {
				escape = false;
				whitespace << '\\';
			}
			break;
		}

		ss << whitespace.str();
		whitespace.str(std::string());

		ss << c;
		empty = false;
	}
	return ss.str();
}

void getParsersFromGroup(std::vector<Parser> &parsers, std::set<std::string> &processedIncludes, const std::string &groupName, std::map<std::string, Group> &groups)
{
	if (processedIncludes.find(groupName) != processedIncludes.end())
		return;

	const Group &group = groups[groupName];

	processedIncludes.insert(groupName);
	parsers.insert(parsers.end(), group.Parsers.begin(), group.Parsers.end());

	for (const std::string &includeGroupName : group.Includes)
		getParsersFromGroup(parsers, processedIncludes, includeGroupName, groups);

	if (group.ExtendedColors.is_initialized())
		Config::ExtendedColors = *group.ExtendedColors;
	if (group.ParseJson.is_initialized())
		Config::ParseJson = *group.ParseJson;
	if (group.WhiteBackground.is_initialized())
		Config::WhiteBackground = *group.WhiteBackground;
	if (group.BoldColorSet.is_initialized())
		Config::BoldColorSet = *group.BoldColorSet;
	if (group.UnderlineColorSet.is_initialized())
		Config::UnderlineColorSet = *group.UnderlineColorSet;
	if (group.BackgroundColorSet.is_initialized())
		Config::BackgroundColorSet = *group.BackgroundColorSet;
	if (group.PrintElapsed.is_initialized())
		Config::PrintElapsed = *group.PrintElapsed;
	if (group.MinElapsedSeconds.is_initialized())
		Config::MinElapsedSeconds = *group.MinElapsedSeconds;
	if (group.MaxElapsedNewLines.is_initialized())
		Config::MaxElapsedNewLines = *group.MaxElapsedNewLines;
	if (group.ElapsedNewLineSeconds.is_initialized())
		Config::ElapsedNewLineSeconds = *group.ElapsedNewLineSeconds;
	if (group.UsePager.is_initialized())
		Config::UsePager = *group.UsePager;
	if (group.PagerBin.size())
		Config::PagerBin = group.PagerBin;
	if (group.TailBin.size())
		Config::TailBin = group.TailBin;
}

std::vector<Parser> parseConfigFiles(const std::string &inputFilename, bool listGroups)
{
	std::map<std::string, Group> groups;
	std::vector<std::shared_ptr<std::istream>> configs;

#ifdef CONFIG_STRING_VAR
	// add compiled in config from std::string
	configs.push_back(std::make_shared<std::istringstream>(CONFIG_STRING_VAR));
#endif

	// create configuration file paths
	std::vector<std::string> replaceStrings = { "~", "$HOME" };
	for (std::string path : Config::ConfigFilepaths) {
		for (const std::string &replaceString : replaceStrings) {
			size_t pos = path.find(replaceString);
			if (pos != std::string::npos)
				path.replace(pos, replaceString.size(), std::getenv("HOME"));
		}
		configs.push_back(std::make_shared<std::ifstream>(path));
	}

	std::string defaultGroup;
	std::string groupName;
	Group group;

	// parse configuration files
	for (std::shared_ptr<std::istream> &config : configs) {
		unsigned lineNumber = 0;

		std::string previousKey;
		std::string line;
		while (std::getline(*config, line)) try {
			++lineNumber;
			std::string key, value;
			std::istringstream input(line);

			input >> key;

			// skip empty lines and comments
			if (key.empty() || key[0] == '#')
				continue;

			std::getline(input, value);
			value = parseConfigValue(value);

			if (Config::Debug)
				std::cout << "CONFIG: " << key << "=" << value << std::endl;

			if (key == "Group") {
				// create new group
				groups[groupName] = group;
				groupName = value;
				group = Group();
				if (listGroups)
					std::cout << groupName << std::endl;
			}
			else if (key == "DefaultGroup") {
				defaultGroup = value;
			}
			else if (key == "ExtendedColors")
				group.ExtendedColors
					= value == "true" ? Config::Feature::yes
					: value == "false" ? Config::Feature::no
					: Config::Feature::autodetect;
			else if (key == "ParseJson")
				group.ParseJson = value.empty() ? true : value == "true";
			else if (key == "PagerCmd")
				group.PagerBin = { "sh", "-c", value };
			else if (key == "FollowCmd")
				group.TailBin = { "sh", "-c", value };
			else if (key == "PrintElapsed")
				group.PrintElapsed = value.empty() ? true : value == "true";
			else if (key == "MinElapsedSeconds")
				group.MinElapsedSeconds = std::stoul(value);
			else if (key == "MaxElapsedNewLines")
				group.MaxElapsedNewLines = std::stoul(value);
			else if (key == "ElapsedNewLineSeconds")
				group.ElapsedNewLineSeconds = std::stoul(value);
			else if (key == "UsePager")
				group.UsePager
					= value == "true" ? Config::Feature::yes
					: value == "false" ? Config::Feature::no
					: Config::Feature::autodetect;
			else if (key == "Include")
				group.Includes.push_front(value);
			else if (key == "FileRegexp" || key == "FileIRegexp")
				group.FileRegexps.push_back(Clog::regex(value,
						key == "FileIRegexp"
							? Clog::regex::ECMAScript | Clog::regex::icase
							: Clog::regex::ECMAScript));
			else if (key == "ColorRegexp" || key == "ColorIRegexp")
				group.Parsers.push_back(Parser(Clog::regex(value,
						key == "ColorIRegexp"
							? Clog::regex::ECMAScript | Clog::regex::icase
							: Clog::regex::ECMAScript)));
			else if (key[0] >= '0' && key[0] <= '9') {
				// sanity check that numbers can be only after ColorRegexp
				if (previousKey == "ColorRegexp" || previousKey == "ColorIRegexp"
					|| (previousKey.size() && previousKey[0] >= '0' && previousKey[0] <= '9'))
					group.Parsers.back().items[std::stoul(key)].push_back(Items::createFromString(value));
				else
					throw std::invalid_argument("Numbers should be after ColorRegexp or ColorIRegexp key!");
			}
			previousKey = key;

		} catch (std::exception &e) {
			std::cerr << "Config error: " << e.what() << std::endl
				<< "Line " << lineNumber << ": " << line << std::endl;
			break;
		}
	}
	groups[groupName] = group;

	// select group by inputFilename (if not already selected)
	for (auto iter = groups.cbegin(); iter != groups.cend() && Config::SelectedGroup.empty() && inputFilename.size(); ++iter) {
		for (auto &regex : iter->second.FileRegexps) {
			if (Clog::regex_search(inputFilename, regex)) {
				Config::SelectedGroup = iter->first;
				break;
			}
		}
	}

	if (Config::SelectedGroup.empty())
		Config::SelectedGroup = defaultGroup;

	if (Config::Debug)
		std::cout << "SELECTED GROUP: " << Config::SelectedGroup << std::endl;

	// create parsers from selected group and its includes
	std::vector<Parser> parsers;
	std::set<std::string> processedIncludes;
	getParsersFromGroup(parsers, processedIncludes, Config::SelectedGroup, groups);

	return parsers;
}

}
