#include "items.h"
#include "color.h"
#include "config.h"

#include <iostream>
#include <unordered_map>
#include <time.h>

std::time_t Items::ElapsedPrinter::emptyTime = {};
std::time_t Items::ElapsedPrinter::printedTime = {};
std::time_t Items::ElapsedPrinter::parsedTime = {};
std::vector<std::string> Items::HashColorizer::colors;

std::string Items::Item::onStart(const std::string & /* txt */) const
{
	return std::string();
}

std::string Items::Item::onEnd(const std::string & /* txt */) const
{
	return std::string();
}

void Items::ElapsedPrinter::printSeconds()
{
	if (parsedTime == emptyTime)
		return;

	// calculate time diff and set internal time static variables
	double diffSeconds = std::difftime(parsedTime, printedTime);
	printedTime = parsedTime;
	parsedTime = {};

	if (diffSeconds < Config::MinElapsedSeconds)
		return;

	// print number of newlines based on elapsed time and configuration options
	for (unsigned i = 0; i < Config::MaxElapsedNewLines && i * Config::ElapsedNewLineSeconds < diffSeconds; ++i)
		std::cout << std::endl;

	int diffMinutes = diffSeconds / 60;
	diffSeconds -= diffMinutes * 60;
	int diffHours = diffMinutes / 60;
	diffMinutes -= diffHours * 60;
	int diffDays = diffHours / 24;
	diffHours -= diffDays * 24;

	std::cout << "\x1b[90m... Elapsed";
	if (diffDays > 0)
		std::cout << " " << diffDays << " day" << (diffDays >= 2 ? "s" : "");
	if (diffHours > 0)
		std::cout << " " << diffHours << " hour" << (diffHours >= 2 ? "s" : "");
	if (diffMinutes > 0)
		std::cout << " " << diffMinutes << " minute" << (diffMinutes >= 2 ? "s" : "");
	if (diffSeconds > 0)
		std::cout << " " << diffSeconds << " second" << (diffSeconds >= 2 ? "s" : "");
	std::cout << "\x1b[0m" << std::endl;
}

Items::ElapsedPrinter::ElapsedPrinter(const std::string &format)
	: Items::Item()
	, format(format)
{
}

std::string Items::ElapsedPrinter::onStart(const std::string &txt) const
{
	if (!Config::PrintElapsed)
		return std::string();

	std::tm t = {};
	if (!strptime(txt.c_str(), format.c_str(), &t))
		return std::string();

	std::time_t time = std::mktime(&t);

	if (printedTime == emptyTime)
		printedTime = time;

	parsedTime += time;
	return std::string();
}

Items::Colorizer::Colorizer(const std::string &code)
	: Items::Item()
	, code(code)
{
}

std::string Items::Colorizer::onStart(const std::string & /* txt */) const
{
	return code;
}

std::string Items::Colorizer::onEnd(const std::string & /* txt */) const
{
	return Color::Reset;
}

Items::HashColorizer::HashColorizer()
	: Items::Item()
{
}

template<typename T> bool calcHashHelper(int32_t &hash, const char *&data, unsigned &size)
{
	if (size < sizeof(T))
		return false;

	hash <<= sizeof(T);
	hash ^= *(T*)data;
	data += sizeof(T);
	size -= sizeof(T);
	return true;
}

std::string Items::HashColorizer::onStart(const std::string &txt) const
{
	if (colors.empty())
		colors = initializeHashColors();

	// calculate hash
	int32_t hash = 0;
	const char *data = txt.data();
	unsigned size = txt.size();
	while (true) {
		if (calcHashHelper<int32_t>(hash, data, size)
			|| calcHashHelper<int16_t>(hash, data, size)
			|| calcHashHelper<int8_t>(hash, data, size))
			continue;
		else
			break;
	}

	return colors[hash % colors.size()];
}

std::string Items::HashColorizer::onEnd(const std::string & /* txt */) const
{
	return Color::Reset;
}

std::vector<std::string> Items::HashColorizer::initializeHashColors()
{
	std::vector<std::string> styles = { "" };

	if (Config::BoldColorSet)
		styles.push_back("1;");

	if (Config::UnderlineColorSet)
		styles.push_back("4;");

	if (Config::ExtendedColors == Config::Feature::yes) {
		// 256 colors
		for (unsigned color = Config::WhiteBackground ? 16 : 20; color < 230; ++color) {
			if (Config::WhiteBackground && isColorLight(color))
				continue;
			for (const auto &style : styles) {
				std::stringstream ss;
				ss << "\x1b[" << style << "38;5;" << color << "m";
				colors.push_back(ss.str());
			}
		}
	} else {
		// ANSI colors
		if (Config::BoldColorSet && Config::UnderlineColorSet)
			styles.push_back("1;4;");
		for (unsigned color = 31; color <= 36; ++color) {
			for (const auto &style : styles) {
				std::stringstream ss;
				ss << "\x1b[" << style << color << "m";
				colors.push_back(ss.str());
			}
		}
		if (Config::BackgroundColorSet) {
			for (unsigned color = 41; color <= 46; ++color) {
				for (const auto &style : styles) {
					std::stringstream ss;
					ss << "\x1b[" << style << color << "m";
					colors.push_back(ss.str());
				}
			}
		}
		if (Config::BoldColorSet && Config::UnderlineColorSet) {
			colors.push_back("\x1b[1;4;30m");
		}
	}
	return colors;
}

unsigned Items::HashColorizer::isColorLight(unsigned color)
{
	// magic that filter out colors that are tooo bright
	if ((color - 16) % 36 >= 28 - (color / 36) * 3) {
		return 1;
	} else {
		return 0;
	}
}

std::shared_ptr<Items::Item> Items::createFromString(const std::string &str)
{
	std::shared_ptr<Items::Item> item;
	bool background = false;

	if (str.find("hash") != std::string::npos)
		item = std::make_shared<Items::HashColorizer>();

	else if (str.find("time:") != std::string::npos)
		item = std::make_shared<Items::ElapsedPrinter>(str.substr(5));

	else {
		std::stringstream ss;
		if (str.find("bold") != std::string::npos)
			ss << "1;";
		if (str.find("dim") != std::string::npos)
			ss << "2;";
		if (str.find("underline") != std::string::npos)
			ss << "4;";
		if (str.find("blink") != std::string::npos)
			ss << "5;";
		if (str.find("invert") != std::string::npos)
			ss << "7;";
		if (str.find("hidden") != std::string::npos)
			ss << "8;";
		if (str.find("background") != std::string::npos)
			background = true;

		bool light = str.find("light") != std::string::npos;
		if (str.find("black") != std::string::npos)
			ss << (light ? 90 : 30) << ';';
		if (str.find("red") != std::string::npos)
			ss << (light ? 91 : 31) << ';';
		if (str.find("green") != std::string::npos)
			ss << (light ? 93 : 32) << ';';
		if (str.find("yellow") != std::string::npos)
			ss << (light ? 93 : 33) << ';';
		if (str.find("blue") != std::string::npos)
			ss << (light ? 94 : 34) << ';';
		if (str.find("magenta") != std::string::npos)
			ss << (light ? 95 : 35) << ';';
		if (str.find("cyan") != std::string::npos)
			ss << (light ? 96 : 36) << ';';
		if (str.find("gray") != std::string::npos)
			ss << (light ? 37 : 90) << ';';
		if (str.find("white") != std::string::npos)
			ss << 97 << ';';

		ss << str << ';';

		// clear out invalid characters and additional semicolons
		std::stringstream itemCode;
		unsigned number = 0;
		char c;
		while (ss.get(c)) {
			if (c >= '0' && c <= '9')
				number = (10 * number) + (c - '0');
			else if (number) {
				if (itemCode.str().size())
					itemCode << ';';
				if (background && number >= 30) {
					background = false;
					number += 10;
				}
				itemCode << number;
				number = 0;
			}
		}

		if (itemCode.str().empty())
			item = std::make_shared<Items::Item>();
		else
			item = std::make_shared<Items::Colorizer>("\x1b[" + itemCode.str() + "m");
	}

	item->line = str.find("line") != std::string::npos;

	return item;
}
