// Hex, numbers, env files and argument splitting: what every value from the API and the scripts
// passes through.
#include "check.h"
#include "util/strings.h"

using namespace btb;

namespace {

void test_hex() {
  std::vector<uint8_t> b;
  CHECK(from_hex("0a1B", &b));
  CHECK_EQ(to_hex(b), std::string("0a1b"));
  CHECK(from_hex("0A 1b:2c-3D", &b));
  CHECK_EQ(to_hex(b), std::string("0a1b2c3d"));
  CHECK(from_hex("0x0102", &b));
  CHECK_EQ(b.size(), size_t(2));
  CHECK(from_hex("", &b));
  CHECK(b.empty());
  CHECK(!from_hex("abc", &b));   // odd
  CHECK(!from_hex("zz", &b));
}

void test_printable() {
  CHECK_EQ(printable({'H', 'i', 0}), std::string("Hi"));  // a C string's terminator is not text
  CHECK_EQ(printable({0x01, 0x02}), std::string());
  CHECK_EQ(printable({0x5a}), std::string());  // a battery level of 90 %, not the letter Z
  CHECK_EQ(printable({}), std::string());
  CHECK_EQ(printable({0xc3, 0xa9}), std::string("\xc3\xa9"));  // UTF-8 is text
}

void test_numbers() {
  uint64_t v = 0;
  CHECK(parse_uint("42", 100, &v) && v == 42);
  CHECK(parse_uint("0x2A", 100, &v) && v == 42);
  CHECK(!parse_uint("101", 100, &v));
  CHECK(!parse_uint("-1", 100, &v));
  CHECK(!parse_uint("4x", 100, &v));
  CHECK(!parse_uint("", 100, &v));
  CHECK(!parse_uint("0x", 100, &v));
}

void test_env() {
  const auto e = parse_env("# comment\nBOARD=rpi0w\nexport KERNEL=\"next\"\nARGS='-d -E'\n"
                           "Q=\"a \\\"b\\\"\"\nBAD LINE\nX=1 # trailing\n");
  CHECK_EQ(e.at("BOARD"), std::string("rpi0w"));
  CHECK_EQ(e.at("KERNEL"), std::string("next"));
  CHECK_EQ(e.at("ARGS"), std::string("-d -E"));
  CHECK_EQ(e.at("Q"), std::string("a \"b\""));
  CHECK_EQ(e.at("X"), std::string("1"));
  CHECK(!e.count("BAD LINE"));
  // What env_quote writes, parse_env reads back.
  const std::string tricky = "-d \"x\" $HOME `y` \\";
  CHECK_EQ(parse_env("A=" + env_quote(tricky)).at("A"), tricky);
}

void test_split_args() {
  const auto a = split_args("-c 3  'AA BB' \"x y\" z");
  CHECK_EQ(a.size(), size_t(5));
  CHECK_EQ(a[0], std::string("-c"));
  CHECK_EQ(a[2], std::string("AA BB"));
  CHECK_EQ(a[3], std::string("x y"));
  CHECK(split_args("   ").empty());
  CHECK_EQ(split_args("''").size(), size_t(1));  // an empty argument is still one
}

}  // namespace

int main() {
  test_hex();
  test_printable();
  test_numbers();
  test_env();
  test_split_args();
  return report("test_strings");
}
