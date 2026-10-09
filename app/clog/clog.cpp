#include "config.h"
#include "filter.h"
#include "color.h"
#include "items.h"
#include "json_formatter.h"
#include "parser.h"

#include <fstream>
#include <iostream>
#include <ostream>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

#define INPUT_END 1
#define OUTPUT_END 0

const char* Help = R"&&&(clog [OPTION...] -p PATTERN -(0-9) COLOR ... [FILE...]

DESCRIPTION
You can set regexp parsers to this utility to colorize its captures.
Parsers can be configured from command line or they are loaded from configuration files based on input file.

OPTIONS
	-h
		This help
	-d
		Turn on debug output that prints indexed submatches
	-j, -J
		Turn on JSON formatting (-J for turning off)
	-s, -S
		Print elapsed time between log lines (-S for turning off)
	-l, -L
		Use pager for output (-L for turning off)
	-t
		Follow files output appending (tail -F <files>)
	-c FILE
		Set configuration file location (default ~/.clog and /etc/clog)
	-g name
		Explicitly set group name from configuration files
	-G
		List all available groups from config files

Filterring input
	-f PATTERN
		Print line only when regular expression pattern is matched
	-v PATTERN
		Filter out lines that match regular expression pattern

Case insensitivity
	-P, -F, -V
		Same as corresponding flag in lower case but they ignore case sensitivity

Colorizing input
	-p PATTERN
		Regular expression parser
	-(0-9) COLOR
		Number specifies index of submatch.
		Available colors are:
		 - "black", red", "green", "yellow", "blue", "magenta", "cyan", "gray", "white"
		 - color code number according to https://misc.flogisoft.com/bash/tip_colors_and_formatting
		You can further modify these colors by specifiers:
		 - "bold", "background", "dim", "light", "underline", "invert", "blink", "hidden"
		 - "line" color will be applied on whole line (note: color priority is reversed for this modifier)
		There are special "colors" available:
		 - "hash" Color is calculated from hash number that is generated from captured text
		 - "time:<format>" time will be captured from log line and when there is big difference this time
		                   will be printed and some automatic spaces will be inserted.
		                   Format is according to https://man7.org/linux/man-pages/man3/strptime.3.html
	-w
		Modify hash colors for white background
	-e
		Enable 256 colors mode for hash colors

EXAMPLES
Syslog:
clog -p "^(\w\w\w  ?\d?\d \d\d:\d\d:\d\d) (\S+) ([^\s:\[]+)" -1 "time:%b %d %T" -2 hash -3 hash -P "\b(warn(?:ing)?)|(error|fail(?:ed)?)\b" -1 line_yellow -2 line_red /var/log/syslog
)&&&";

std::string string_to_hex(const std::string& input)
{
	static const char hex_digits[] = "0123456789ABCDEF";

	std::string output;
	output.reserve(input.length() * 2);
	for (unsigned char c : input) {
		output.push_back(hex_digits[c >> 4]);
		output.push_back(hex_digits[c & 15]);
	}
	return output;
}

void parseAndRemoveEscapeSequences(std::string &line, Color::Map &parsedColors)
{
	static Clog::regex escapeSequenceRegexp = Clog::regex("\x1b\\[[\\d;]+m", Clog::regex::ECMAScript);

	// find all escape sequences and put them to map
	std::map<size_t, std::string> parsedEscapeSequences;
	std::ostringstream ss;
	Clog::smatch matches;
	auto strIt = line.cbegin();
	while (Clog::regex_search(strIt, line.cend(), matches, escapeSequenceRegexp)) {
		if (matches[0].matched) {
			ss << std::string(strIt, matches[0].first);
			strIt = matches[0].second;
			parsedEscapeSequences[std::distance(line.cbegin(), matches[0].first)] = matches[0].str();
			matches = Clog::smatch();  // std::match_results has no clear()
		} else
			break;
	}

	// check if we had some match
	if (strIt != line.cbegin()) {
		if (strIt != line.cend()) {
			// add rest of unparsed line to ss
			ss << std::string(strIt, line.cend());
			strIt = line.cend();
		}
		line = ss.str();
		size_t offset = 0;
		std::string lastEscapeSequence;
		for (auto mapIt = parsedEscapeSequences.cbegin(); mapIt != parsedEscapeSequences.cend(); ++mapIt) {
			std::string escapeSequence = mapIt->second == Color::Reset00 ? Color::Reset : mapIt->second;
			//std::cout << "--- [" << mapIt->first << "] = " << string_to_hex(escapeSequence) << std::endl;
			//std::cout << "--- [last] = " << string_to_hex(lastEscapeSequence) << std::endl;
			// don't put duplicate escape sequnces to parsed map
			if (escapeSequence != lastEscapeSequence)
				parsedColors[line.cbegin() + mapIt->first - offset].push_back(escapeSequence);
			//else
			//    std::cout << "--- SKIP" << std::endl;
			lastEscapeSequence = escapeSequence;
			offset += mapIt->second.size();
		}
	}
}

template<typename T>
void parseInputLineByLine(T &input, std::vector<Parser> &parsers, LineFilter &filter)
{
	std::string line;
	while (std::getline(input, line)) {
		Color::Map parsedColors;
		Color::Stack colorsStack;
		JsonFormatter json(colorsStack);

		parseAndRemoveEscapeSequences(line, parsedColors);

		if (!filter.process(line))
			continue;

		if (parsers.empty())
			std::cout << json.process(line) << std::endl;
		else for (auto &parser : parsers)
			parser.parse(line, parsedColors);

		Items::ElapsedPrinter::printSeconds();

		auto position = line.cbegin();
		for (auto it = parsedColors.cbegin(); it != parsedColors.cend(); ++it) {
			std::cout << json.process(position, it->first);

			for (const auto &color : it->second) {
				if (color != Color::Reset) {
					colorsStack.push(color);
				} else if (colorsStack.size()) {
					colorsStack.pop();
					std::cout << Color::Reset;
				}
			}

			if (colorsStack.size())
				std::cout << colorsStack.top();

			position = it->first;
		}
		std::cout << json.process(position, line.cend());

		// if there's some leftover color in stack reset it before progressing to newline
		if (colorsStack.size())
			std::cout << Color::Reset;

		std::cout << std::endl;
	}
}

enum class Flag {
	unset,
	on,
	off,
};

int main(int argc, char **argv)
{
	std::vector<std::ifstream> inputFiles;
	std::vector<std::string> tailFiles;
	bool tailFile = false;
	bool listGroups = false;
	std::vector<Parser> parsers;
	LineFilter filter;

	Flag parseJsonFlag = Flag::unset;
	Flag usePagerFlag = Flag::unset;
	Flag printElapsedFlag = Flag::unset;
	Flag extendedColorsFlag = Flag::unset;
	Flag whiteBackgroundFlag = Flag::unset;

	int pidStatus;
	int pagerPipe[2], tailPipe[2];
	int pagerPid = 0, tailPid = 0;

	std::ios_base::sync_with_stdio(false);

	int c;
	opterr = 0;
	while ((c = getopt(argc, argv, "hdp:P:0:1:2:3:4:5:6:7:8:9:f:F:v:V:eEwWjJtlLsSc:g:G")) != -1) {
		switch (c) {
		case 'h':
			std::cout << Help;
			return 0;
		case 'd':
			Config::Debug = true;
			break;
		case 'p': // parser
			if (Config::Debug)
				std::cout << "PARSER: " << optarg << std::endl;
			parsers.push_back(Parser(Clog::regex(std::string(optarg), Clog::regex::ECMAScript)));
			break;
		case 'P': // parser, ignore case
			if (Config::Debug)
				std::cout << "PARSER: " << optarg << std::endl;
			parsers.push_back(Parser(Clog::regex(std::string(optarg), Clog::regex::ECMAScript | Clog::regex::icase)));
			break;
		case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
			if (parsers.empty()) {
				std::cerr << "First create parser with -p <regexp>" << std::endl;
				return 1;
			}
			if (Config::Debug)
				std::cout << " - ITEM: [" << (c - '0') << "] = " << optarg << std::endl;
			parsers.back().items[c - '0'].push_back(Items::createFromString(optarg));
			break;
		case 'f': // match only filter
			filter.matchOnlyRegexps.push_back(Clog::regex(std::string(optarg), Clog::regex::ECMAScript));
			break;
		case 'F': // match only filter, ignore case
			filter.matchOnlyRegexps.push_back(Clog::regex(std::string(optarg), Clog::regex::ECMAScript | Clog::regex::icase));
			break;
		case 'v': // filter out match
			filter.filterOutRegexps.push_back(Clog::regex(std::string(optarg), Clog::regex::ECMAScript));
			break;
		case 'V': // filter out match, ignore case
			filter.filterOutRegexps.push_back(Clog::regex(std::string(optarg), Clog::regex::ECMAScript | Clog::regex::icase));
			break;
		case 'e':
			extendedColorsFlag = Flag::on;
			break;
		case 'E':
			extendedColorsFlag = Flag::off;
			break;
		case 'w':
			whiteBackgroundFlag = Flag::on;
			break;
		case 'W':
			whiteBackgroundFlag = Flag::off;
			break;
		case 'j':
			parseJsonFlag = Flag::on;
			break;
		case 'J':
			parseJsonFlag = Flag::off;
			break;
		case 't':
			tailFile = true;
			break;
		case 'l':
			usePagerFlag = Flag::on;
			break;
		case 'L':
			usePagerFlag = Flag::off;
			break;
		case 's':
			printElapsedFlag = Flag::on;
			break;
		case 'S':
			printElapsedFlag = Flag::off;
			break;
		case 'c':
			Config::ConfigFilepaths = { optarg };
			break;
		case 'g':
			Config::SelectedGroup = optarg;
			break;
		case 'G':
			listGroups = true;
			break;
		case '?':
			std::cerr << "Unknown option '-" << char(optopt) << "' or argument missing" << std::endl;
			return 1;
		}
	}

	{
		std::vector<Parser> configParsers = Config::parseConfigFiles(optind < argc ? argv[optind] : std::string(), listGroups);
		if (listGroups)
			return 0;

		parsers.insert(parsers.end(), configParsers.begin(), configParsers.end());
	}

	for (int i = optind; i < argc; ++i) {
		if (Config::Debug)
			std::cout << "FILE: " << argv[i] << std::endl;
		if (tailFile)
			tailFiles.push_back(argv[i]);
		else
			inputFiles.push_back(std::ifstream(argv[i]));
	}

	if (parseJsonFlag != Flag::unset)
		Config::ParseJson = parseJsonFlag == Flag::on;
	if (usePagerFlag != Flag::unset)
		Config::UsePager = usePagerFlag == Flag::on ? Config::Feature::yes : Config::Feature::no;
	if (extendedColorsFlag != Flag::unset)
		Config::ExtendedColors = extendedColorsFlag == Flag::on ? Config::Feature::yes : Config::Feature::no;
	if (whiteBackgroundFlag != Flag::unset)
		Config::WhiteBackground = whiteBackgroundFlag == Flag::on;
	if (printElapsedFlag != Flag::unset)
		Config::PrintElapsed = printElapsedFlag == Flag::on;

	if (Config::ExtendedColors == Config::Feature::autodetect) {
		// check if terminal supports 256 colors
		const char* term_env = std::getenv("TERM");  // unset without a terminal (ssh host cmd, a service)
		std::string term(term_env ? term_env : "");
		if (term.find("256") != std::string::npos)
			Config::ExtendedColors = Config::Feature::yes;
	}

	if (Config::UsePager == Config::Feature::autodetect) {
		if (inputFiles.size() && isatty(fileno(stdout)))
			Config::UsePager = Config::Feature::yes;
	}

	if (Config::UsePager == Config::Feature::yes) {
		if (pipe(pagerPipe)) {
			std::cerr << "Failed to create pager pipe" << std::endl;
			return 1;
		}
		pagerPid = fork();
		if (pagerPid == 0) {
			dup2(pagerPipe[OUTPUT_END], STDIN_FILENO);
			close(pagerPipe[INPUT_END]);
			close(pagerPipe[OUTPUT_END]);

			std::vector<const char *> args;
			for (const std::string &arg : Config::PagerBin)
				args.push_back(arg.data());
			args.push_back(nullptr);
			execvp(Config::PagerBin.front().data(), (char* const *)args.data());
			std::cerr << "Failed to create pager process" << std::endl;
			exit(1);
		}

		dup2(pagerPipe[INPUT_END], STDOUT_FILENO);
		close(pagerPipe[INPUT_END]);
		close(pagerPipe[OUTPUT_END]);

		// send Ctrl-C/End-of-text character character to pager by writting it to piped stdin
		signal(SIGINT, [](int signal) { std::cout << char(3); });
	}

	if (tailFiles.size()) {
		if (pipe(tailPipe)) {
			std::cerr << "Failed to create tail pipe" << std::endl;
			return 1;
		}
		tailPid = fork();
		if (tailPid == 0) {
			dup2(tailPipe[INPUT_END], STDOUT_FILENO);
			close(tailPipe[INPUT_END]);
			close(tailPipe[OUTPUT_END]);

			std::vector<const char *> args;
			for (const std::string &arg : Config::TailBin)
				args.push_back(arg.data());
			for (const std::string &arg : tailFiles)
				args.push_back(arg.data());
			args.push_back(nullptr);
			execvp(Config::TailBin.front().data(), (char* const *)args.data());
			std::cerr << "Failed to create tail process" << std::endl;
			exit(1);
		}

		dup2(tailPipe[OUTPUT_END], STDIN_FILENO);
		close(tailPipe[INPUT_END]);
		close(tailPipe[OUTPUT_END]);
	}

	if (inputFiles.empty())
		parseInputLineByLine(std::cin, parsers, filter);
	else for (auto &file : inputFiles)
		parseInputLineByLine(file, parsers, filter);

	if (tailPid) {
		close(STDIN_FILENO);
		waitpid(tailPid, &pidStatus, 0);
	}
	if (pagerPid) {
		close(STDOUT_FILENO);
		waitpid(pagerPid, &pidStatus, 0);
	}

	return 0;
}
