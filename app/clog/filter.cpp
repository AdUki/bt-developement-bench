#include "filter.h"
#include <iostream>

bool LineFilter::process(const std::string &line) const
{
	if (matchOnlyRegexps.size()) {
		auto iter = matchOnlyRegexps.cbegin();
		while (true) {
			if (Clog::regex_search(line, *iter))
				break;
			if (++iter == matchOnlyRegexps.cend())
				return false;
		}
	}
	for (const Clog::regex &regex : filterOutRegexps) {
		if (Clog::regex_search(line, regex))
			return false;
	}

	return true;
}
