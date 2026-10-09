#pragma once

#include <map>
#include <memory>

#include "color.h"
#include "items.h"
#include "regex.h"

class Parser
{
public:
	Parser() = default;
	Parser(Clog::regex &&regex);

	void parse(const std::string &line, Color::Map &colors);

	Clog::regex regex;
	std::map<unsigned, std::vector<std::shared_ptr<Items::Item>>> items;
};
