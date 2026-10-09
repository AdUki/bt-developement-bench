#include "parser.h"
#include "config.h"

#include <iostream>

Parser::Parser(Clog::regex &&regex)
	: regex(regex)
{
}

void Parser::parse(const std::string &line, Color::Map &colors)
{
	Clog::smatch matches;
	if (Clog::regex_search(line, matches, regex)) {
		if (Config::Debug)
			for (size_t i = 0; i < matches.size(); ++i)
				std::cout << (i == 0 ? "" : "  ") << " - [" << i << "]" << (matches[i].matched ? ": '" + matches[i].str() + "'" : "") << std::endl;

		for (auto mapIt = items.cbegin(); mapIt != items.cend(); ++mapIt)
		for (auto itemIt = mapIt->second.cbegin(); itemIt != mapIt->second.cend(); ++itemIt) {
			if (mapIt->first >= matches.size())
				continue;

			const auto &match = matches[mapIt->first];

			if (!match.matched)
				continue;

			std::string startStr = (*itemIt)->onStart(match.str());
			if (startStr.size()) {
				if ((*itemIt)->line)
					colors[line.begin()].push_front(startStr);
				else
					colors[match.first].push_back(startStr);
			}

			std::string endStr = (*itemIt)->onEnd(match.str());
			if (endStr.size()) {
				if ((*itemIt)->line)
					colors[line.end()].push_back(endStr);
				else
					colors[match.second].push_front(endStr);
			}
		}
	} else if (Config::Debug)
		std::cout << " - [0]" << std::endl;
}
