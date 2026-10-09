#pragma once

#include "color.h"

class JsonFormatter
{
public:
	JsonFormatter(Color::Stack &colorsStack);
	const std::string process(const std::string &txt);
	const std::string process(std::string::const_iterator start, std::string::const_iterator end);

private:
	void indent(std::stringstream &ss);

	const Color::Stack &colorsStack;
	unsigned offset;
	bool escape;
	bool inString;
	char matchingBracket;
};
