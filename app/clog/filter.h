#pragma once

#include "regex.h"

#include <vector>

class LineFilter
{
public:
	std::vector<Clog::regex> filterOutRegexps;
	std::vector<Clog::regex> matchOnlyRegexps;

	bool process(const std::string &line) const;
};
