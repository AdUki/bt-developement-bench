// The board's player: the library a remote browses (its folders, tracks and what they play), the
// metadata a track carries, and search — what bench_player.cpp answers BlueZ with, without a bus.
#include "bench_player.h"
#include "check.h"

using namespace btb;
using json = nlohmann::json;

namespace {

const json kRadio = json{{"stations", json::array({json{{"name", "Groove Salad"}, {"url", "https://ice1.somafm.com/gs"}},
                                                  json{{"name", "no url"}},
                                                  json{{"name", "Drone Zone"}, {"url", "https://ice1.somafm.com/dz"}}})}};

int child_named(const BenchLibrary& lib, int folder, const std::string& name) {
  for (int id : lib.children(folder))
    if (lib.items()[id].name == name) return id;
  return -1;
}

void test_tree() {
  BenchLibrary lib;
  lib.build(kRadio);
  // The root: the test signals and the radio, both folders that play.
  CHECK_EQ(lib.root().size(), 2u);
  const int sig = child_named(lib, -1, "Test signals");
  const int radio = child_named(lib, -1, "Radio");
  CHECK(sig >= 0 && radio >= 0);
  CHECK(lib.find(sig)->folder && lib.find(sig)->playable);
  CHECK_EQ(lib.find(radio)->folder_type, std::string("playlists"));
  // A station without a URL is left out.
  CHECK_EQ(lib.children(radio).size(), 2u);
  const LibItem* gs = lib.find(lib.children(radio)[0]);
  CHECK_EQ(gs->source.value("type", ""), std::string("url"));
  CHECK_EQ(gs->source.value("title", ""), std::string("Groove Salad"));
  // A folder in a folder, and the tracks of a folder are its entries that are not folders.
  const int levels = child_named(lib, sig, "Levels");
  CHECK(levels >= 0);
  CHECK_EQ(lib.find(levels)->parent, sig);
  CHECK_EQ(lib.tracks(levels).size(), 6u);
  CHECK_EQ(lib.tracks(sig).size(), lib.children(sig).size() - 1);
  CHECK_EQ(lib.folder_name(levels), std::string("/Filesystem/Test signals/Levels"));
  CHECK_EQ(lib.folder_name(-1), std::string("/Filesystem"));
  // Ids are indexes, and a track is nobody's folder.
  for (const LibItem& it : lib.items()) CHECK_EQ(lib.find(it.id), &it);
  CHECK(lib.children(lib.tracks(sig)[0]).empty());
  CHECK(lib.find(-1) == nullptr);
  CHECK(lib.find(static_cast<int>(lib.items().size())) == nullptr);
  // Rebuilt (the radio list changed): the same tree again, not twice.
  lib.build(json::object());
  CHECK_EQ(lib.root().size(), 2u);
  CHECK(lib.children(child_named(lib, -1, "Radio")).empty());
}

void test_sources() {
  BenchLibrary lib;
  lib.build(kRadio);
  const int sig = child_named(lib, -1, "Test signals");
  const LibItem* lr = lib.find(child_named(lib, sig, "Left 1 kHz, right 2 kHz"));
  CHECK_EQ(lr->source.value("type", ""), std::string("tone"));
  CHECK_EQ(lr->source.value("freq_right", 0.0), 2000.0);
  // The sweep ends (a track that ends is what a remote sees go to the next one) and says how long.
  const LibItem* sweep = lib.find(child_named(lib, sig, "Sweep 20 Hz to 20 kHz"));
  CHECK_EQ(sweep->source.value("repeat", true), false);
  CHECK_EQ(sweep->duration_ms, 10000u);
  const LibItem* zero = lib.find(child_named(lib, child_named(lib, sig, "Levels"), "Sine 1 kHz at 0 dBFS"));
  CHECK(zero != nullptr);
  CHECK_EQ(zero->source.value("level_db", -1.0), 0.0);
}

void test_metadata() {
  BenchLibrary lib;
  lib.build(kRadio);
  const int sig = child_named(lib, -1, "Test signals");
  const std::vector<int> t = lib.tracks(sig);
  const json md = lib_item_metadata(lib, *lib.find(t[1]));
  CHECK_EQ(md.value("Title", ""), lib.find(t[1])->name);
  CHECK_EQ(md.value("Album", ""), std::string("Test signals"));
  // Its number among the folder's tracks (the Levels folder is not counted).
  CHECK_EQ(md.value("Number", 0u), 2u);
  CHECK_EQ(md.value("NumberOfTracks", 0u), static_cast<unsigned>(t.size()));
  CHECK(!md.contains("Duration"));  // a tone has no end
  const json sw = lib_item_metadata(lib, *lib.find(child_named(lib, sig, "Sweep 20 Hz to 20 kHz")));
  CHECK_EQ(sw.value("Duration", 0u), 10000u);
}

void test_search() {
  BenchLibrary lib;
  lib.build(kRadio);
  // Every word, any case, anywhere in the name; tracks only.
  const std::vector<int> r = lib.search("sine 1 KHZ");
  CHECK(!r.empty());
  for (int id : r) {
    const std::string n = lib.find(id)->name;
    CHECK(n.find("Sine") != std::string::npos && n.find("1") != std::string::npos &&
          n.find("kHz") != std::string::npos);
  }
  CHECK_EQ(lib.search("10 khz").size(), 1u);
  CHECK_EQ(lib.search("drone").size(), 1u);
  CHECK(lib.search("levels").empty());  // a folder is not a result
  CHECK(lib.search("").empty());
  CHECK(lib.search("nothing like it").empty());
}

// The track of a stream the player did not start: what the remote's display shows.
void test_stream_track() {
  using nlohmann::json;
  const json radio{{"source", {{"type", "url"}, {"title", "Radio FM"}, {"playlist", json::array({{{"url", "u"}, {"title", ""}}})}}},
                   {"meta", {{"name", "FM"}, {"title", "Artist - Song"}}}, {"track", 0}};
  json md = stream_track(radio);
  CHECK_EQ(md.value("Title", ""), std::string("Song"));
  CHECK_EQ(md.value("Artist", ""), std::string("Artist"));
  CHECK_EQ(md.value("Album", ""), std::string("Radio FM"));
  CHECK(!md.contains("Number"));
  CHECK(!md.contains("Duration"));
  // A title without the " - " is all title.
  json plain = radio;
  plain["meta"]["title"] = "Jingle-Time";
  CHECK_EQ(stream_track(plain).value("Title", ""), std::string("Jingle-Time"));
  CHECK(!stream_track(plain).contains("Artist"));
  // A media server's folder: each track as the server described it; the stream's title is the
  // first track's ("One and 1 more" from the console), never the album.
  const json folder{{"source", {{"type", "url"}, {"title", "One"},
                                {"playlist", json::array({{{"title", "One"}},
                                                          {{"title", "Two"}, {"artist", "Band"}, {"album", "LP"}, {"duration_ms", 183000}}})}}},
                    {"label", "One and 1 more"}, {"track", 1}};
  md = stream_track(folder);
  CHECK_EQ(md.value("Title", ""), std::string("Two"));
  CHECK_EQ(md.value("Artist", ""), std::string("Band"));
  CHECK_EQ(md.value("Album", ""), std::string("LP"));
  CHECK_EQ(md.value("Duration", 0u), 183000u);
  CHECK_EQ(md.value("Number", 0u), 2u);
  CHECK_EQ(md.value("NumberOfTracks", 0u), 2u);
  // A sweep's label keeps its own arrow; only where it goes is cut.
  md = stream_track(json{{"source", {{"type", "sweep"}}}, {"label", "sweep 20 Hz → 20 kHz → bluez_output.x"}});
  CHECK_EQ(md.value("Title", ""), std::string("Sweep 20 Hz → 20 kHz"));
  // btbench is the player's name, never a track's artist.
  CHECK(!stream_track(json{{"sink", nullptr}}).contains("Artist"));
}

void test_stream_to_bluetooth() {
  using nlohmann::json;
  const json bt{{"sink", {{"type", "pipewire"}, {"target", "bluez_output.50_1E.1"}}}};
  const json def{{"sink", {{"type", "pipewire"}, {"target", ""}}}};
  CHECK(stream_to_bluetooth(bt, ""));
  CHECK(!stream_to_bluetooth(def, ""));
  CHECK(stream_to_bluetooth(def, "bluez_output.50_1E.1"));
  CHECK(!stream_to_bluetooth(json{{"sink", {{"type", "pipewire"}, {"target", "alsa_output.x"}}}}, "y"));
  CHECK(!stream_to_bluetooth(json{{"sink", nullptr}}, "y"));
  CHECK(!stream_to_bluetooth(json{{"sink", {{"type", "alsa"}, {"target", ""}}}}, "y"));
  // Any of several sinks: the loopback card and a speaker.
  CHECK(stream_to_bluetooth(json::parse(R"({"sinks": [{"type": "pipewire", "target": "alsa_output.x"},
      {"type": "pipewire", "target": "bluez_output.50_1E.1"}], "sink": {"type": "pipewire", "target": "alsa_output.x"}})"), ""));
  CHECK(!stream_to_bluetooth(json::parse(R"({"sinks": [], "sink": null})"), "y"));
}

}  // namespace

int main() {
  test_stream_track();
  test_stream_to_bluetooth();
  test_tree();
  test_sources();
  test_metadata();
  test_search();
  return report("test_bench_player");
}
