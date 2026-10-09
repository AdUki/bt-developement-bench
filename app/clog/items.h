#pragma once

#include <deque>
#include <iomanip>
#include <memory>
#include <vector>

namespace Items {

class Item
{
public:
	bool line = false;

	Item() = default;

	virtual std::string onStart(const std::string & /* txt */) const;
	virtual std::string onEnd(const std::string & /* txt */) const;
};

class ElapsedPrinter : public Item
{
public:
	static void printSeconds();

	ElapsedPrinter(const std::string &format);

	std::string onStart(const std::string &txt) const override;

private:
	static std::time_t emptyTime;
	static std::time_t printedTime;
	static std::time_t parsedTime;
	std::string format;
};

class Colorizer : public Item
{
public:
	Colorizer(const std::string &code);

	std::string onStart(const std::string & /* txt */) const override;
	std::string onEnd(const std::string & /* txt */) const override;

private:
	std::string code;
};

class HashColorizer : public Item
{
public:
	HashColorizer();

	std::string onStart(const std::string &txt) const override;
	std::string onEnd(const std::string & /* txt */) const override;

private:
	static std::vector<std::string> initializeHashColors();
	static unsigned isColorLight(unsigned color);

	static std::vector<std::string> colors;
	static std::hash<std::string> hasher;
};

std::shared_ptr<Item> createFromString(const std::string &str);
}
