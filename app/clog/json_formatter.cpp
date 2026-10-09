#include "json_formatter.h"
#include "config.h"

#include <sstream>

JsonFormatter::JsonFormatter(Color::Stack &colorsStack)
	: colorsStack(colorsStack)
	, offset(0)
	, escape(false)
	, inString(false)
	, matchingBracket(0)
{
}

const std::string JsonFormatter::process(const std::string &txt)
{
	if (Config::ParseJson)
		return process(txt.cbegin(), txt.cend());
	return txt;
}

const std::string JsonFormatter::process(std::string::const_iterator start, std::string::const_iterator end)
{
	if (!Config::ParseJson)
		return std::string(start, end);

	std::stringstream ss;

	for (; start < end; ++start) {
		char c = *start;

		// we will check here for empty brackets
		// if there is some content, do indent
		if (matchingBracket && c != matchingBracket) {
			++offset;
			indent(ss);
		}
		matchingBracket = 0;

		if (escape) {
			escape = false;
			ss << c;
		} else switch (c) {
			case '\\':
				ss << c;
				escape = true;
				break;
			case '"':
				inString = !inString;
				ss << c;
				break;
			case '[':
				ss << c;
				if (offset && !inString)
					matchingBracket = ']';
				break;
			case '{':
				ss << c;
				if (!inString)
					matchingBracket = '}';
				break;
			case ',':
				ss << c;
				if (!inString && offset)
					indent(ss);
				break;
			case '}':
			case ']':
				if (!inString && offset) {
					--offset;
					indent(ss);
				}
				ss << c;
				break;
			default:
				ss << c;
				break;
		}
	}
	return ss.str();
}

void JsonFormatter::indent(std::stringstream &ss)
{
	ss << std::endl;
	if (colorsStack.size())
		ss << colorsStack.top();
	for (size_t i = 0; i < offset; ++i)
		ss << '\t';
}
