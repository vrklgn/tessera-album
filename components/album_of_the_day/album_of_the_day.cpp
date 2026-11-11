#include "album_of_the_day.h"
#include "albums.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace esphome::album_of_the_day {

using tessera::Font;
namespace ui = tessera::ui;

// What plays it: Music Assistant finds the album by its title and artist on whatever service it has.
static const char *const PLAY = "music_assistant.play_media";
// What makes the other speakers play along with the tile's player.
static const char *const JOIN = "media_player.join";
// The keys of the player. None skips a track: an album is heard from its first track to its last.
static const char *const PAUSE = "media_player.media_play_pause";
static const char *const VOLUME = "media_player.volume_set";
static const char *const LOUDER = "media_player.volume_up";
static const char *const QUIETER = "media_player.volume_down";
static const uint32_t ICON_PLAY = 0xF040A, ICON_PAUSE = 0xF03E4, ICON_LOUDER = 0xF075D, ICON_QUIETER = 0xF075E,
                      ICON_TRACKS = 0xF0CB8;

// A kept text (in PSRAM) as a plain one, for a call that takes std::string.
static std::string plain(const tessera::Text &t) { return std::string(t.data(), t.size()); }

// One address for a cover at every place, so the app fetches it once whichever place shows it.
static std::string cover_url(const Album &a) { return std::string(ART_HOST) + a.art + ART_SIZE; }

// Days since 1970-01-01 for a date (Howard Hinnant's days_from_civil).
static long days_from_civil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<long>(doe) - 719468;
}

// The album of the day at `epoch` on the screen's calendar, its place in ALBUMS; -1 while the clock is not set.
// Counting down, the list runs from its last album to its first, a hundred days, and starts again. Mixed, every album
// still comes once in a hundred days, in an order that jumps through the list (37 shares no factor with 100).
static int album_of(uint32_t epoch, bool mixed) {
  if (!epoch) return -1;
  const tessera::LocalTime now = tessera::local_time(epoch);
  const long day = days_from_civil(now.year, static_cast<unsigned>(now.month), static_cast<unsigned>(now.day));
  const int n = static_cast<int>(((day % ALBUM_COUNT) + ALBUM_COUNT) % ALBUM_COUNT);
  return mixed ? (n * 37 + 11) % ALBUM_COUNT : ALBUM_COUNT - 1 - n;
}

// Whether two texts are the same but for case.
static bool same(const char *a, const char *b, size_t n) {
  for (size_t i = 0; i < n; ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  return true;
}

// Whether the album the player has is this one, playing or paused: its album starts with the title (a service may
// add "(Remastered)" to it) and, when it names one, its artist starts with the album's.
static bool on_player(const Album &a, const std::string &album, const std::string &artist) {
  const size_t t = strlen(a.title), n = strlen(a.artist);
  if (album.size() < t || !same(album.c_str(), a.title, t)) return false;
  return artist.empty() || (artist.size() >= n && same(artist.c_str(), a.artist, n));
}

// One track of an album: its name (not ended, `length` bytes) and how long it is.
struct Track {
  const char *name = "";
  size_t length = 0;
  int seconds = 0;
};

// The track at `index` of the album (0 the first), from its line "name<tab>seconds"; false past the last.
static bool track_at(const Album &a, int index, Track &out) {
  const char *line = a.tracks;
  for (int i = 0; *line; ++i) {
    const char *end = strchr(line, '\n');
    const size_t n = end ? static_cast<size_t>(end - line) : strlen(line);
    if (i == index) {
      const char *tab = static_cast<const char *>(memchr(line, '\t', n));
      out.name = line;
      out.length = tab ? static_cast<size_t>(tab - line) : n;
      out.seconds = tab ? atoi(tab + 1) : 0;
      return true;
    }
    if (!end) break;
    line = end + 1;
  }
  return false;
}

static int track_count(const Album &a) {
  int count = a.tracks[0] ? 1 : 0;
  for (const char *t = a.tracks; *t; ++t) count += *t == '\n';
  return count;
}

// How long the first `tracks` tracks of the album are together, in seconds.
static int seconds_of(const Album &a, int tracks) {
  int sum = 0;
  Track t;
  for (int i = 0; i < tracks && track_at(a, i, t); ++i) sum += t.seconds;
  return sum;
}

// A length as a player writes it: "3:07".
static std::string length_text(int seconds) {
  char text[12];
  snprintf(text, sizeof(text), "%d:%02d", seconds / 60, seconds % 60);
  return text;
}

// The place of a track on the album by its name, 1 the first, 0 when the album does not have it. The player's name
// may be cut short or carry an edition after it, so one of the two starting with the other is the same track.
static int track_number(const Album &a, const char *title) {
  const size_t t = strlen(title);
  Track track;
  for (int i = 0; t && track_at(a, i, track); ++i) {
    const size_t n = std::min(track.length, t);
    if (n && same(track.name, title, n) && (track.length == t || n >= 4)) return i + 1;
  }
  return 0;
}

// The album on a player: the other speakers join it first, so all of them play the one album together, then Music
// Assistant plays the album there instead of what its queue held. False when Home Assistant was not reached.
static bool start_album(const Album &a, const std::string &player, const tessera::Text *also) {
  JsonDocument group;
  JsonArray members = group["group_members"].to<JsonArray>();
  const std::string first = plain(also[0]), second = plain(also[1]);
  if (!first.empty() && first != player) members.add(first);
  if (!second.empty() && second != player && second != first) members.add(second);
  if (members.size() > 0) tessera::action(JOIN, player, group);
  JsonDocument d;
  d["media_id"] = a.title;
  d["media_type"] = "album";
  d["artist"] = a.artist;
  d["enqueue"] = "replace";
  return tessera::action(PLAY, player, d);
}

// The volume of a player a step of five in a hundred down or up, and what it is then; where the player does not say
// its volume (`level` under 0), a step of its own.
static float turn_volume(const std::string &player, float level, int by) {
  if (level < 0) {
    tessera::action(by > 0 ? LOUDER : QUIETER, player);
    return level;
  }
  const int hundredths = std::max(0, std::min(100, static_cast<int>(level * 100.0f + 0.5f) + by * 5));
  JsonDocument d;
  d["volume_level"] = static_cast<float>(hundredths) / 100.0f;
  tessera::action(VOLUME, player, d);
  return static_cast<float>(hundredths) / 100.0f;
}

// A player's volume from its `volume_level`, -1 when it does not say one.
static float volume_of(JsonVariantConst level) {
  if (level.is<float>()) return level.as<float>();
  if (level.is<const char *>()) return static_cast<float>(atof(level.as<const char *>()));
  return -1.0f;
}

// "No. 37", and the year after it where it is known and fits `width`.
static std::string place_line(const tessera::Plugin *plugin, const Album &a, int width, Font font, bool with_year) {
  std::string line = tessera::format(plugin->text("place"), a.rank);
  if (!with_year || !a.year) return line;
  char year[8];
  snprintf(year, sizeof(year), "%u", static_cast<unsigned>(a.year));
  const std::string both = line + (ui::covers(" · ", font) ? " · " : ", ") + year;
  return ui::text_width(both, font) <= width ? both : line;
}

static lv_obj_t *plain_block(lv_obj_t *parent, theme::Role fill) {
  lv_obj_t *o = ui::block(parent, fill);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(o, 0, 0);
  return o;
}

// A round key: an icon of Tessera's set on a circle.
static lv_obj_t *round_block(lv_obj_t *parent, uint32_t icon, int x, int y, int size, theme::Role fill, theme::Role ink) {
  lv_obj_t *key = plain_block(parent, fill);
  lv_obj_set_size(key, size, size);
  lv_obj_set_style_radius(key, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_pos(key, x, y);
  lv_obj_t *glyph = ui::label(key, Font::ICON_SMALL, ink);
  lv_obj_set_width(glyph, size);
  lv_obj_set_style_text_align(glyph, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(glyph, 0, (size - ui::line_height(Font::ICON_SMALL)) / 2);
  ui::set_text(glyph, ui::icon(icon));
  return key;
}

// Whether a point is on a round key, a little room round it counted in.
static bool on_key(lv_obj_t *key, const tessera::Point &p, int size) {
  if (!key || lv_obj_has_flag(key, LV_OBJ_FLAG_HIDDEN)) return false;
  const int x = lv_obj_get_x(key), y = lv_obj_get_y(key), slack = ui::px(5);
  return p.x >= x - slack && p.x < x + size + slack && p.y >= y - ui::px(8) && p.y <= y + size + ui::px(8);
}

// The cover's place: a picture's, or on a board without pictures (the CYD) a block with the album's place in the list
// as a large number.
struct Art {
  lv_obj_t *place = nullptr, *block = nullptr, *number = nullptr;
  int side = 0;

  // `square`: without the rounded corners, behind which the app paints the card's usual colour.
  void make(lv_obj_t *parent, int x, int y, int s, bool square = false) {
    side = s;
    if (ui::pictures()) {
      place = ui::picture(parent);
      lv_obj_set_size(place, s, s);
      lv_obj_set_pos(place, x, y);
      if (square) lv_obj_set_style_radius(place, 0, 0);
      return;
    }
    block = plain_block(parent, theme::TRACK);
    lv_obj_set_size(block, s, s);
    lv_obj_set_pos(block, x, y);
    number = ui::label(block, Font::VALUE, theme::INK);
    lv_obj_set_width(number, s);
    lv_obj_set_style_text_align(number, LV_TEXT_ALIGN_CENTER, 0);
  }
  void show(const Album *a) const {
    if (place) {
      ui::set_picture(place, a ? cover_url(*a) : "", tessera::Fit::COVER);
      return;
    }
    char rank[8] = "";
    if (a) snprintf(rank, sizeof(rank), "%u", static_cast<unsigned>(a->rank));
    Font face = Font::VALUE;
    for (Font f : {Font::VALUE, Font::HEADLINE, Font::TITLE}) {
      face = f;
      if (ui::text_width(rank, f) <= side - ui::px(8)) break;
    }
    ui::set_font(number, face);
    ui::set_text(number, rank);
    lv_obj_set_pos(number, 0, (side - ui::line_height(face)) / 2);
  }
  void paint() const {
    if (!block) return;
    ui::set_fill(block, tessera::Tone::NORMAL, theme::TRACK);
    ui::set_color(number, theme::INK);
  }
};

// The tile: the cover of the album of the day. One cell shows the cover alone; a tile lying down the cover beside the
// album's place in the list, its title and its artist, and a sentence about it where there is room; a tile standing up
// the cover over them. It changes at midnight on the screen's own clock. A tap opens the album's card. The tile's
// entity is the Music Assistant player that plays it. Its `background` option takes the card behind it away, or tints
// it in one of the four colours of the Tessera mark.
//
// A large tile is a deck, with everything on the page itself: the cover and the words in a band at the top, with keys
// that play and pause the album and turn the volume; a bar that says how far into the album the player is; and under
// it every track with its length, the one that plays standing out. A tap on a key works it, a tap anywhere else opens
// the card. No key skips a track.
class AlbumTile : public tessera::Tile {
 public:
  explicit AlbumTile(AlbumOfTheDay *plugin) : plugin_(plugin) {}

  void create(const tessera::TileContext &c) override {
    width_ = c.width;
    height_ = c.height;
    tile_ = c.tile;
    player_ = tessera::Text(c.entity ? c.entity : "");
    also_[0] = tessera::Text(c.options["also_1"] | "");
    also_[1] = tessera::Text(c.options["also_2"] | "");
    mixed_ = !strcmp(c.options["order"] | "countdown", "mixed");
    parent_ = c.parent;
    const char *ground = c.options["background"] | "default";
    ground_ = !strcmp(ground, "none") ? Ground::NONE : !strcmp(ground, "blue") ? Ground::BLUE
            : !strcmp(ground, "yellow") ? Ground::YELLOW : !strcmp(ground, "purple") ? Ground::PURPLE
            : !strcmp(ground, "green") ? Ground::GREEN : Ground::DEFAULT;
    const bool square = ground_ != Ground::DEFAULT;
    paint_ground(true);
    const int th = ui::line_height(Font::TITLE), bh = ui::line_height(Font::BODY), gap = ui::px(8);
    if (plan_deck()) {
      make_deck(c.parent, square);
    } else if (c.columns == 1 && c.rows == 1) {
      // One cell: the cover over the whole card.
      const int s = std::min(width_, height_);
      art_.make(c.parent, (width_ - s) / 2, (height_ - s) / 2, s, square);
    } else if (width_ * 10 >= height_ * 13) {
      // Lying down: the cover at the left, as high as the card, the words beside it in the middle of its height.
      const int s = std::min(height_, width_ / 2);
      art_.make(c.parent, 0, (height_ - s) / 2, s, square);
      words_x_ = s + ui::px(12);
      words_w_ = width_ - words_x_;
      const int base = bh + ui::px(2) + th + ui::px(2) + bh;
      title_lines_ = base + th <= height_ ? 2 : 1;
      const int left = height_ - base - (title_lines_ - 1) * th - gap;
      tag_lines_ = std::max(0, std::min(3, left / bh));
      make_words(c.parent, LV_TEXT_ALIGN_LEFT);
    } else {
      // Standing up: the cover as wide as the card allows, its title and artist under it.
      const int text = th + bh;
      const int s = std::max(1, std::min(width_, height_ - text - gap));
      art_.make(c.parent, (width_ - s) / 2, 0, s, square);
      words_w_ = width_;
      stacked_ = true;
      title_ = ui::label(c.parent, Font::TITLE, theme::INK);
      lv_obj_set_width(title_, width_);
      lv_obj_set_style_text_align(title_, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_pos(title_, 0, height_ - text);
      artist_ = ui::label(c.parent, Font::BODY, theme::MUTED);
      lv_obj_set_width(artist_, width_);
      lv_obj_set_style_text_align(artist_, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_pos(artist_, 0, height_ - bh);
    }
  }

  void on_state(JsonObjectConst data) override {
    paint_ground(false);
    if (data["state"].isNull()) return;
    plugin_->player_name = tessera::Text(data["name"] | "");
    state_ = tessera::Text(data["state"] | "");
    album_ = tessera::Text(data["attributes"]["media_album_name"] | "");
    playing_artist_ = tessera::Text(data["attributes"]["media_artist"] | "");
    track_ = tessera::Text(data["attributes"]["media_title"] | "");
    level_ = volume_of(data["attributes"]["volume_level"]);
    if (shown_) show();
  }

  // The album follows the clock: another one at midnight.
  void on_tick(uint32_t epoch) override {
    paint_ground(false);
    // A key that did not reach Home Assistant says so for a few seconds.
    if (failed_ && esphome::millis() - failed_at_ > 5000) {
      failed_ = false;
      if (shown_) show();
    }
    const int now = album_of(epoch, mixed_);
    if (now == at_ && shown_) return;
    at_ = now;
    shown_ = true;
    show();
  }

  void on_theme() override {
    paint_ground(true);
    art_.paint();
    if (place_) ui::set_color(place_, theme::MUTED);
    if (title_) ui::set_color(title_, theme::INK);
    if (artist_) ui::set_color(artist_, theme::MUTED);
    if (tagline_) ui::set_color(tagline_, theme::MUTED);
    if (deck_) {
      ui::set_color(head_, theme::MUTED);
      ui::set_fill(bar_, tessera::Tone::NORMAL, theme::TRACK);
      ui::set_fill(bar_fill_, tessera::Tone::NORMAL, theme::ACCENT);
      if (volume_) ui::set_color(volume_, theme::MUTED);
      if (play_) ui::set_color(lv_obj_get_child(play_, 0), theme::ON_ACCENT);
      for (lv_obj_t *key : {quieter_, louder_}) {
        if (!key) continue;
        ui::set_fill(key, tessera::Tone::NORMAL, theme::TRACK);
        ui::set_color(lv_obj_get_child(key, 0), theme::INK);
      }
      for (int i = 0; i < slots_; ++i) {
        ui::set_color(names_[i], theme::INK);
        ui::set_color(times_[i], theme::MUTED);
      }
    }
    if (shown_) show();
  }

  // A deck's keys work the player; a tap anywhere else opens the card of the album, with what the tile knows: the
  // card is made after this and reads it from the plugin.
  void on_tap() override {
    if (at_ < 0) return;
    if (deck_ && !player_.empty()) {
      const tessera::Point p = tessera::tap_point();
      const std::string player = plain(player_);
      if (on_key(play_, p, round_)) {
        if (on_ && (state_ == "playing" || state_ == "paused")) {
          tessera::action(PAUSE, player);
        } else if (!start_album(ALBUMS[at_], player, also_)) {
          failed_ = true;
          failed_at_ = esphome::millis();
        }
        show();
        return;
      }
      if (on_key(quieter_, p, round_) || on_key(louder_, p, round_)) {
        level_ = turn_volume(player, level_, on_key(louder_, p, round_) ? 1 : -1);
        show();
        return;
      }
      // A finger between two keys meant a key, not the card.
      if (p.x >= words_x_ && p.y >= keys_y_ - ui::px(8) && p.y <= keys_y_ + round_ + ui::px(8)) return;
    }
    plugin_->chosen = at_;
    plugin_->also[0] = also_[0];
    plugin_->also[1] = also_[1];
    tessera::open_card(plugin_->plugin_id(), "album", plain(player_), tile_);
  }

 private:
  enum class Ground : uint8_t { DEFAULT, NONE, BLUE, YELLOW, PURPLE, GREEN };

  // The card behind the tile, as the tile's `background` option wants it: gone, or tinted. The card is the screen's
  // own, the parent of the tile's drawing area, and the screen paints it again in its usual look now and then (a new
  // look, a new state), so this runs with every call of the tile and changes it only when it differs. Back at
  // "default", a card an earlier choice changed gets its usual look again.
  void paint_ground(bool colour) {
    lv_obj_t *card = parent_ ? lv_obj_get_parent(parent_) : nullptr;
    if (!card) return;
    const lv_opa_t fill = ground_ == Ground::DEFAULT ? LV_OPA_COVER : ground_ == Ground::NONE ? LV_OPA_TRANSP : LV_OPA_30;
    const lv_opa_t edge = ground_ == Ground::DEFAULT ? LV_OPA_COVER : LV_OPA_TRANSP;
    const bool differs = lv_obj_get_style_bg_opa(card, LV_PART_MAIN) != fill ||
                         lv_obj_get_style_border_opa(card, LV_PART_MAIN) != edge;
    if (!differs && (!colour || ground_ == Ground::DEFAULT || ground_ == Ground::NONE)) return;
    lv_obj_set_style_bg_opa(card, fill, 0);
    lv_obj_set_style_border_opa(card, edge, 0);
    const theme::Role role = ground_ == Ground::BLUE ? theme::MARK_BLUE : ground_ == Ground::YELLOW ? theme::MARK_AMBER
                           : ground_ == Ground::PURPLE ? theme::MARK_PURPLE : ground_ == Ground::GREEN ? theme::MARK_GREEN
                           : theme::CARD;
    if (ground_ != Ground::NONE) lv_obj_set_style_bg_color(card, ui::color(role), 0);
  }

  // The words beside the cover: the place in the list, the title (two lines when they fit), the artist, the sentence.
  void make_words(lv_obj_t *parent, lv_text_align_t align) {
    const int th = ui::line_height(Font::TITLE), bh = ui::line_height(Font::BODY);
    place_ = ui::label(parent, Font::BODY, theme::MUTED);
    title_ = title_lines_ == 2 ? ui::text(parent, Font::TITLE, theme::INK) : ui::label(parent, Font::TITLE, theme::INK);
    artist_ = ui::label(parent, Font::BODY, theme::MUTED);
    for (lv_obj_t *o : {place_, title_, artist_}) {
      lv_obj_set_width(o, words_w_);
      lv_obj_set_style_text_align(o, align, 0);
    }
    if (title_lines_ == 2) lv_obj_set_height(title_, 2 * th);
    if (tag_lines_ > 0) {
      tagline_ = ui::text(parent, Font::BODY, theme::MUTED);
      lv_obj_set_size(tagline_, words_w_, tag_lines_ * bh);
      lv_obj_set_style_text_align(tagline_, align, 0);
    }
  }

  // Whether the tile has the room of a deck, and its measures then: the band of the cover and the words, the keys in
  // it, and how many tracks stand under it, in one column or two.
  bool plan_deck() {
    const int th = ui::line_height(Font::TITLE), bh = ui::line_height(Font::BODY), gap = ui::px(8);
    round_ = ui::mm(8);
    const int words = bh + ui::px(2) + th + ui::px(2) + bh;
    const int band = words + (player_.empty() ? 0 : gap + round_);
    side_ = std::max(band, std::min(height_ * 32 / 100, width_ * 38 / 100));
    row_h_ = bh + ui::px(5);
    bar_y_ = side_ + gap;
    list_y_ = bar_y_ + ui::px(4) + ui::px(6) + bh + ui::px(6);
    rows_ = std::min(MAX_ROWS, (height_ - list_y_) / row_h_);
    columns_ = (width_ - ui::px(16)) / 2 >= ui::px(180) ? 2 : 1;
    while (rows_ * columns_ > MAX_SLOTS) --rows_;
    words_x_ = side_ + ui::px(12);
    words_w_ = width_ - words_x_;
    return width_ >= ui::px(250) && words_w_ >= ui::px(110) && rows_ >= 4;
  }

  void make_deck(lv_obj_t *parent, bool square) {
    const int th = ui::line_height(Font::TITLE), bh = ui::line_height(Font::BODY), gap = ui::px(8);
    deck_ = true;
    art_.make(parent, 0, 0, side_, square);
    // The words beside the cover, at the top of the band; the keys at its foot.
    place_ = ui::label(parent, Font::BODY, theme::MUTED);
    title_ = ui::label(parent, Font::TITLE, theme::INK);
    artist_ = ui::label(parent, Font::BODY, theme::MUTED);
    int y = 0;
    for (lv_obj_t *o : {place_, title_, artist_}) {
      lv_obj_set_width(o, words_w_);
      lv_obj_set_pos(o, words_x_, y);
      y += (o == title_ ? th : bh) + ui::px(2);
    }
    if (!player_.empty()) {
      // Three round keys, smaller where the row would not fit, and the volume in words after them.
      if (3 * round_ + 2 * gap > words_w_) round_ = std::max(1, (words_w_ - 2 * gap) / 3);
      keys_y_ = side_ - round_;
      int x = words_x_;
      play_ = round_block(parent, ICON_PLAY, x, keys_y_, round_, theme::ACCENT, theme::ON_ACCENT);
      x += round_ + gap;
      quieter_ = round_block(parent, ICON_QUIETER, x, keys_y_, round_, theme::TRACK, theme::INK);
      x += round_ + gap;
      louder_ = round_block(parent, ICON_LOUDER, x, keys_y_, round_, theme::TRACK, theme::INK);
      x += round_ + gap;
      if (width_ - x >= ui::text_width("100%", Font::BODY)) {
        volume_ = ui::label(parent, Font::BODY, theme::MUTED);
        lv_obj_set_width(volume_, width_ - x);
        lv_obj_set_pos(volume_, x, keys_y_ + (round_ - bh) / 2);
      }
    }
    // How far into the album the player is: a thin bar over the whole width, filled up to the track that plays.
    bar_ = plain_block(parent, theme::TRACK);
    lv_obj_set_size(bar_, width_, ui::px(4));
    lv_obj_set_pos(bar_, 0, bar_y_);
    lv_obj_set_style_radius(bar_, ui::px(2), 0);
    bar_fill_ = plain_block(parent, theme::ACCENT);
    lv_obj_set_size(bar_fill_, 1, ui::px(4));
    lv_obj_set_pos(bar_fill_, 0, bar_y_);
    lv_obj_set_style_radius(bar_fill_, ui::px(2), 0);
    lv_obj_add_flag(bar_fill_, LV_OBJ_FLAG_HIDDEN);
    // How many tracks and how long together, then the tracks: a place for each line the room has.
    head_ = ui::label(parent, Font::BODY, theme::MUTED);
    lv_obj_set_width(head_, width_);
    lv_obj_set_pos(head_, 0, bar_y_ + ui::px(4) + ui::px(6));
    slots_ = rows_ * columns_;
    for (int i = 0; i < slots_; ++i) {
      names_[i] = ui::label(parent, Font::BODY, theme::INK);
      times_[i] = ui::label(parent, Font::BODY, theme::MUTED);
      lv_obj_set_style_text_align(times_[i], LV_TEXT_ALIGN_RIGHT, 0);
    }
  }

  void show_deck(const Album *a) {
    art_.show(a);
    const int count = a ? track_count(*a) : 0;
    on_ = a && on_player(*a, plain(album_), plain(playing_artist_));
    const bool sounding = state_ == "playing", held = state_ == "paused";
    const int number = on_ && (sounding || held) && !track_.empty() ? track_number(*a, track_.c_str()) : 0;
    // A track the album's list does not know by that name stands where the album's place in the list did.
    const bool unknown = on_ && sounding && !track_.empty() && !number;
    ui::set_text(place_, unknown ? plain(track_) : a ? place_line(plugin_, *a, words_w_, Font::BODY, true) : "");
    ui::set_tone(place_, unknown ? tessera::Tone::GOOD : tessera::Tone::NORMAL, theme::MUTED);
    ui::set_text(title_, a ? a->title : "");
    ui::set_text(artist_, a ? a->artist : "");
    // The key that plays: pause while the album sounds, green then; amber when Home Assistant was not reached.
    if (play_) {
      ui::set_text(lv_obj_get_child(play_, 0), ui::icon(on_ && sounding ? ICON_PAUSE : ICON_PLAY));
      ui::set_fill(play_, failed_ ? tessera::Tone::BUSY : on_ && sounding ? tessera::Tone::GOOD : tessera::Tone::NORMAL,
                   theme::ACCENT);
    }
    if (volume_) {
      char percent[8] = "";
      if (level_ >= 0) snprintf(percent, sizeof(percent), "%d%%", static_cast<int>(level_ * 100.0f + 0.5f));
      ui::set_text(volume_, percent);
    }
    // "16 tracks · 77 min", and the bar up to where the track that plays starts.
    const int total = a ? seconds_of(*a, count) : 0;
    std::string head;
    if (count) {
      head = tessera::format(plugin_->text("tracks"), count);
      if (total) head += (ui::covers(" · ", Font::BODY) ? " · " : ", ") + tessera::format(plugin_->text("minutes"), (total + 30) / 60);
    }
    ui::set_text(head_, head);
    const int done = number > 1 && total ? width_ * seconds_of(*a, number - 1) / total : number ? ui::px(4) : 0;
    if (done > 0) {
      lv_obj_set_width(bar_fill_, std::min(width_, std::max(ui::px(4), done)));
      lv_obj_remove_flag(bar_fill_, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(bar_fill_, LV_OBJ_FLAG_HIDDEN);
    }
    // The tracks: one column while it holds them all, else two where the tile has them. An album longer than its
    // places shows the part round the track that plays.
    const int columns = count > rows_ ? columns_ : 1;
    const int places = rows_ * columns, between = ui::px(16);
    const int column_w = (width_ - (columns - 1) * between) / columns;
    const int time_w = ui::text_width("00:00", Font::BODY) + ui::px(2);
    int first = 0;
    if (count > places && number) first = std::max(0, std::min(count - places, number - 1 - places / 2));
    Track track;
    for (int i = 0; i < slots_; ++i) {
      if (i >= places || !a || !track_at(*a, first + i, track)) {
        lv_obj_add_flag(names_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(times_[i], LV_OBJ_FLAG_HIDDEN);
        continue;
      }
      const int x = (i / rows_) * (column_w + between), y = list_y_ + (i % rows_) * row_h_;
      lv_obj_set_width(names_[i], column_w - time_w - ui::px(6));
      lv_obj_set_pos(names_[i], x, y);
      lv_obj_set_width(times_[i], time_w);
      lv_obj_set_pos(times_[i], x + column_w - time_w, y);
      char place[8];
      snprintf(place, sizeof(place), "%d  ", first + i + 1);
      ui::set_text(names_[i], std::string(place) + std::string(track.name, track.length));
      ui::set_text(times_[i], track.seconds ? length_text(track.seconds) : "");
      const tessera::Tone tone = first + i + 1 == number ? tessera::Tone::ACCENT : tessera::Tone::NORMAL;
      ui::set_tone(names_[i], tone, theme::INK);
      ui::set_tone(times_[i], tone, theme::MUTED);
      lv_obj_remove_flag(names_[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(times_[i], LV_OBJ_FLAG_HIDDEN);
    }
  }

  void show() {
    const Album *a = at_ >= 0 ? &ALBUMS[at_] : nullptr;
    if (deck_) return show_deck(a);
    art_.show(a);
    if (!title_) return;
    ui::set_text(title_, a ? a->title : "");
    if (stacked_) {
      ui::set_text(artist_, a ? a->artist : "");
      return;
    }
    const int th = ui::line_height(Font::TITLE), bh = ui::line_height(Font::BODY), space = ui::px(2), gap = ui::px(8);
    // While the album plays on the tile's player, the track that plays stands where its place in the list did.
    const bool now = a && state_ == "playing" && !track_.empty() && on_player(*a, plain(album_), plain(playing_artist_));
    ui::set_text(place_, now ? plain(track_) : a ? place_line(plugin_, *a, words_w_, Font::BODY, true) : "");
    ui::set_tone(place_, now ? tessera::Tone::GOOD : tessera::Tone::NORMAL, theme::MUTED);
    ui::set_text(artist_, a ? a->artist : "");
    // A short title takes one line, and the sentence only the lines it needs: the words stand together in the middle.
    const int lines = title_lines_ == 2 && a && ui::text_width(a->title, Font::TITLE) > words_w_ ? 2 : 1;
    int tag = 0;
    if (tagline_ && a && a->tagline[0])
      tag = std::min(tag_lines_, ui::text_width(a->tagline, Font::BODY) / std::max(1, words_w_ * 9 / 10) + 1);
    const int block = bh + space + lines * th + space + bh + (tag ? gap + tag * bh : 0);
    int y = std::max(0, (height_ - block) / 2);
    lv_obj_set_pos(place_, words_x_, y);
    y += bh + space;
    if (title_lines_ == 2) lv_obj_set_height(title_, lines * th);
    lv_obj_set_pos(title_, words_x_, y);
    y += lines * th + space;
    lv_obj_set_pos(artist_, words_x_, y);
    y += bh + gap;
    if (tagline_) {
      lv_obj_set_pos(tagline_, words_x_, y);
      lv_obj_set_height(tagline_, std::max(1, tag) * bh);
      ui::set_text(tagline_, tag ? a->tagline : "");
    }
  }

  AlbumOfTheDay *plugin_;
  Art art_;
  int width_ = 0, height_ = 0, tile_ = -1, at_ = -1;
  int words_x_ = 0, words_w_ = 0, title_lines_ = 1, tag_lines_ = 0;
  bool mixed_ = false, stacked_ = false, shown_ = false;
  Ground ground_ = Ground::DEFAULT;
  tessera::Text player_, also_[2], state_, album_, playing_artist_, track_;
  lv_obj_t *parent_{}, *place_{}, *title_{}, *artist_{}, *tagline_{};
  // The deck: the longest album of the list has forty tracks, so forty places hold any of them.
  static constexpr int MAX_SLOTS = 40, MAX_ROWS = 24;
  bool deck_ = false, on_ = false, failed_ = false;
  int side_ = 0, round_ = 0, keys_y_ = 0, bar_y_ = 0, list_y_ = 0, row_h_ = 0, rows_ = 0, columns_ = 1, slots_ = 0;
  float level_ = -1.0f;
  uint32_t failed_at_ = 0;
  lv_obj_t *play_{}, *quieter_{}, *louder_{}, *volume_{}, *bar_{}, *bar_fill_{}, *head_{};
  lv_obj_t *names_[MAX_SLOTS] = {}, *times_[MAX_SLOTS] = {};
};

// The card of the album: its cover large, then its place in the list and its year over its title, the artist under
// it and a sentence about it. Under them what the tile's Music Assistant player plays now, a key that plays the album
// there and on the other speakers chosen (it pauses and goes on again while the album is on), and a row of round keys:
// the album's tracks, laid over the card until a tap or Back, and the volume down and up. There is no key to skip a
// track: an album is heard from its first track to its last.
class AlbumCard : public tessera::Card {
 public:
  explicit AlbumCard(AlbumOfTheDay *plugin) : plugin_(plugin) {}

  void open(const tessera::CardContext &c) override {
    at_ = std::max(0, std::min(ALBUM_COUNT - 1, plugin_->chosen));
    const Album &a = ALBUMS[at_];
    player_ = tessera::Text(c.entity ? c.entity : "");
    also_[0] = plugin_->also[0];
    also_[1] = plugin_->also[1];
    pad_ = ui::px(16);
    const bool player = !player_.empty();
    const int bh = ui::line_height(Font::BODY), bl = ui::line_height(Font::BODY_LARGE), gap = ui::px(10);
    key_h_ = std::max(ui::mm(9), bl + ui::px(18));
    count_ = track_count(a);
    // The round keys: the tracks where the list knows them, the volume where there is a player.
    const int rounds = (count_ ? 1 : 0) + (player ? 2 : 0);
    round_ = ui::mm(8);
    // The foot: what plays now over the key, or the note that asks for a player; the round keys under it.
    const int foot = (player ? bh + ui::px(6) + key_h_ : 2 * bh) + (rounds ? gap + round_ : 0);
    // The words, from the top: the place and year, the title, the artist, the sentence, and the foot a gap under it.
    auto need = [&](int lines, Font face, int tag) {
      return bh + ui::px(6) + lines * ui::line_height(face) + ui::px(4) + bl + (tag ? ui::px(10) + tag * bh : 0) +
             ui::px(12) + foot;
    };
    // The cover, as large as the card allows beside the words: at the left of a card lying down, on top of a card
    // standing up.
    const bool beside = c.width > c.height;
    int room, side;
    if (beside) {
      side = std::min(c.height - 2 * pad_, c.width * (ui::pictures() ? 45 : 36) / 100);
      x0_ = pad_ + side + ui::px(20);
      width_ = c.width - x0_ - pad_;
      room = c.height - 2 * pad_;
    } else {
      x0_ = pad_;
      width_ = c.width - 2 * pad_;
      side = std::max(c.height * 30 / 100,
                      std::min(c.width - 2 * pad_, c.height - 2 * pad_ - ui::px(14) - need(2, Font::HEADLINE, 2)));
      room = c.height - 2 * pad_ - side - ui::px(14);
    }
    // The title in the largest face it fits, two lines at most; then as much of the sentence as the room leaves.
    Font face = Font::HEADLINE;
    int lines = ui::text_width(a.title, face) <= width_ ? 1 : 2;
    if (ui::text_width(a.title, face) > lines * width_ * 9 / 10 || need(lines, face, 0) > room) {
      face = Font::TITLE;
      lines = ui::text_width(a.title, face) <= width_ ? 1 : 2;
    }
    if (need(lines, face, 0) > room) lines = 1;
    int tag = a.tagline[0] ? std::min(4, ui::text_width(a.tagline, Font::BODY) / std::max(1, width_ * 9 / 10) + 1) : 0;
    while (tag > 0 && need(lines, face, tag) > room) --tag;
    const int block = need(lines, face, tag);
    int y;
    if (beside) {
      const int top = std::max(pad_, (c.height - std::max(side, block)) / 2);
      y = top + std::max(0, (side - block) / 2);
      art_.make(c.parent, pad_, top + std::max(0, (block - side) / 2), side);
    } else {
      y = pad_ + side + ui::px(14);
      art_.make(c.parent, (c.width - side) / 2, pad_, side);
    }
    art_.show(&a);
    const int hh = ui::line_height(face);
    place_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_width(place_, width_);
    lv_obj_set_pos(place_, x0_, y);
    ui::set_text(place_, place_line(plugin_, a, width_, Font::BODY, true));
    y += bh + ui::px(6);
    title_ = lines == 2 ? ui::text(c.parent, face, theme::INK) : ui::label(c.parent, face, theme::INK);
    lv_obj_set_size(title_, width_, lines * hh);
    lv_obj_set_pos(title_, x0_, y);
    ui::set_text(title_, a.title);
    y += lines * hh + ui::px(4);
    artist_ = ui::label(c.parent, Font::BODY_LARGE, theme::INK_SOFT);
    lv_obj_set_width(artist_, width_);
    lv_obj_set_pos(artist_, x0_, y);
    ui::set_text(artist_, a.artist);
    y += bl;
    if (tag) {
      y += ui::px(10);
      tagline_ = ui::text(c.parent, Font::BODY, theme::MUTED);
      lv_obj_set_size(tagline_, width_, tag * bh);
      lv_obj_set_pos(tagline_, x0_, y);
      ui::set_text(tagline_, a.tagline);
      y += tag * bh;
    }
    y += ui::px(12);
    // What plays now, one line; without a player the note that asks for one, in its place and the key's.
    now_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_width(now_, width_);
    lv_obj_set_pos(now_, x0_, y);
    note_ = ui::text(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_size(note_, width_, 2 * bh);
    lv_obj_set_pos(note_, x0_, y);
    key_y_ = player ? y + bh + ui::px(6) : y;
    // The key, as wide as its words, a finger high at least.
    key_ = plain_block(c.parent, theme::ACCENT);
    lv_obj_set_style_radius(key_, key_h_ / 2, 0);
    key_icon_ = ui::label(key_, Font::ICON_SMALL, theme::ON_ACCENT);
    key_text_ = ui::label(key_, Font::BODY_LARGE, theme::ON_ACCENT);
    if (!player) lv_obj_add_flag(key_, LV_OBJ_FLAG_HIDDEN);
    // The round keys in a row under it, smaller where the row would not fit, and the volume in words after them.
    round_y_ = (player ? key_y_ + key_h_ : y + 2 * bh) + gap;
    if (rounds && rounds * round_ + (rounds - 1) * gap > width_) round_ = std::max(1, (width_ - (rounds - 1) * gap) / rounds);
    int x = x0_;
    if (count_) {
      tracks_ = round_key(c.parent, ICON_TRACKS, x);
      x += round_ + gap;
      make_list(c, a);
    }
    if (player) {
      quieter_ = round_key(c.parent, ICON_QUIETER, x);
      x += round_ + gap;
      louder_ = round_key(c.parent, ICON_LOUDER, x);
      x += round_ + gap;
      if (x0_ + width_ - x >= ui::text_width("100%", Font::BODY)) {
        volume_ = ui::label(c.parent, Font::BODY, theme::MUTED);
        lv_obj_set_width(volume_, x0_ + width_ - x);
        lv_obj_set_pos(volume_, x, round_y_ + (round_ - bh) / 2);
      }
    }
    // The tracks lie over everything made before them.
    if (list_) lv_obj_move_foreground(list_);
    paint();
  }

  // Back takes the tracks away first, then closes the card.
  bool on_back() override {
    if (!listed()) return false;
    lv_obj_add_flag(list_, LV_OBJ_FLAG_HIDDEN);
    return true;
  }

  void on_state(JsonObjectConst data) override {
    if (data["state"].isNull()) return;
    state_ = tessera::Text(data["state"] | "");
    JsonObjectConst attributes = data["attributes"];
    album_ = tessera::Text(attributes["media_album_name"] | "");
    playing_artist_ = tessera::Text(attributes["media_artist"] | "");
    track_ = tessera::Text(attributes["media_title"] | "");
    level_ = volume_of(attributes["volume_level"]);
    plugin_->player_name = tessera::Text(data["name"] | "");
    paint();
  }

  void on_theme() override {
    art_.paint();
    ui::set_color(place_, theme::MUTED);
    ui::set_color(title_, theme::INK);
    ui::set_color(artist_, theme::INK_SOFT);
    if (tagline_) ui::set_color(tagline_, theme::MUTED);
    ui::set_color(now_, theme::MUTED);
    ui::set_color(note_, theme::MUTED);
    ui::set_color(key_icon_, theme::ON_ACCENT);
    ui::set_color(key_text_, theme::ON_ACCENT);
    if (volume_) ui::set_color(volume_, theme::MUTED);
    for (lv_obj_t *key : {tracks_, quieter_, louder_}) {
      if (!key) continue;
      ui::set_fill(key, tessera::Tone::NORMAL, theme::TRACK);
      ui::set_color(lv_obj_get_child(key, 0), theme::INK);
    }
    if (list_) ui::set_fill(list_, tessera::Tone::NORMAL, theme::PAGE);
    if (more_) ui::set_color(more_, theme::MUTED);
    paint();
  }

  // A player that does not start within a while: the key says Play again, so the person can try again.
  void on_tick(uint32_t) override {
    if (sent_ == Sent::YES && esphome::millis() - sent_at_ > 20000) {
      sent_ = Sent::NO;
      paint();
    }
  }

  void on_tap() override {
    // A tap anywhere on the tracks takes them away again.
    if (listed()) {
      lv_obj_add_flag(list_, LV_OBJ_FLAG_HIDDEN);
      return;
    }
    const tessera::Point p = tessera::tap_point();
    if (p.x < 0 || p.y < 0) return;
    if (on_key(tracks_, p, round_)) {
      lv_obj_remove_flag(list_, LV_OBJ_FLAG_HIDDEN);
      return;
    }
    if (player_.empty()) return;
    const std::string player = plain(player_);
    if (on_key(quieter_, p, round_) || on_key(louder_, p, round_)) {
      level_ = turn_volume(player, level_, on_key(louder_, p, round_) ? 1 : -1);
      paint();
      return;
    }
    const int slack = ui::px(8);
    if (p.y < key_y_ - slack || p.y > key_y_ + key_h_ + slack / 2) return;
    if (p.x < key_x_ - slack || p.x > key_x_ + key_w_ + slack) return;
    // While the album is on, the key pauses it and lets it go on.
    if (on_ && (state_ == "playing" || state_ == "paused")) {
      tessera::action(PAUSE, player);
      return;
    }
    sent_ = start_album(ALBUMS[at_], player, also_) ? Sent::YES : Sent::FAILED;
    sent_at_ = esphome::millis();
    paint();
  }

 private:
  enum class Sent : uint8_t { NO, YES, FAILED };

  bool listed() const { return list_ && !lv_obj_has_flag(list_, LV_OBJ_FLAG_HIDDEN); }

  // A round key of the row under the large one: an icon on a soft circle.
  lv_obj_t *round_key(lv_obj_t *parent, uint32_t icon, int x) {
    return round_block(parent, icon, x, round_y_, round_, theme::TRACK, theme::INK);
  }

  // The tracks, over the whole card in the page's own colour: a numbered line each, in two columns when one does not
  // hold them and the card is wide enough, and "+ 4 more" in the last place when even those do not.
  void make_list(const tessera::CardContext &c, const Album &a) {
    list_ = plain_block(c.parent, theme::PAGE);
    lv_obj_set_style_radius(list_, 0, 0);
    lv_obj_set_size(list_, c.width, c.height);
    lv_obj_set_pos(list_, 0, 0);
    const int row = ui::line_height(Font::BODY) + ui::px(6), between = ui::px(16);
    const int rows = std::max(1, (c.height - 2 * pad_) / row);
    const int wide = c.width - 2 * pad_;
    const int columns = count_ > rows && (wide - between) / 2 >= ui::px(140) ? 2 : 1;
    const int column_w = (wide - (columns - 1) * between) / columns;
    const int places = rows * columns;
    listed_ = count_ <= places ? count_ : places - 1;
    // Each line its name, with its length at the right of it.
    const int time_w = ui::text_width("00:00", Font::BODY) + ui::px(2);
    Track track;
    for (int i = 0; i < listed_ && track_at(a, i, track); ++i) {
      const int x = pad_ + (i / rows) * (column_w + between), y = pad_ + (i % rows) * row;
      lv_obj_t *line = ui::label(list_, Font::BODY, theme::INK);
      lv_obj_set_width(line, column_w - time_w - ui::px(6));
      lv_obj_set_pos(line, x, y);
      char number[8];
      snprintf(number, sizeof(number), "%d  ", i + 1);
      ui::set_text(line, std::string(number) + std::string(track.name, track.length));
      lv_obj_t *time = ui::label(list_, Font::BODY, theme::MUTED);
      lv_obj_set_width(time, time_w);
      lv_obj_set_style_text_align(time, LV_TEXT_ALIGN_RIGHT, 0);
      lv_obj_set_pos(time, x + column_w - time_w, y);
      ui::set_text(time, track.seconds ? length_text(track.seconds) : "");
    }
    if (listed_ < count_) {
      more_ = ui::label(list_, Font::BODY, theme::MUTED);
      lv_obj_set_width(more_, column_w);
      lv_obj_set_pos(more_, pad_ + (listed_ / rows) * (column_w + between), pad_ + (listed_ % rows) * row);
      ui::set_text(more_, tessera::format(plugin_->text("more"), count_ - listed_));
    }
    lv_obj_add_flag(list_, LV_OBJ_FLAG_HIDDEN);
  }

  // What the card says of the player, again with every change of it.
  void paint() {
    const Album &a = ALBUMS[at_];
    const bool player = !player_.empty();
    on_ = on_player(a, plain(album_), plain(playing_artist_));
    const bool sounding = state_ == "playing", held = state_ == "paused";
    const bool now = on_ && sounding, waits = on_ && held;
    if (on_ && (sounding || held)) sent_ = Sent::NO;
    // What plays now: the track, with its place on the album where the album is this one.
    const std::string track = sounding || held ? plain(track_) : "";
    const int number = on_ && !track.empty() ? track_number(a, track.c_str()) : 0;
    std::string line = track;
    if (number) {
      char place[16];
      snprintf(place, sizeof(place), "%d/%d", number, count_);
      line = std::string(place) + (ui::covers(" · ", Font::BODY) ? " · " : "  ") + track;
    }
    ui::set_text(now_, player ? line : "");
    ui::set_tone(now_, now ? tessera::Tone::GOOD : tessera::Tone::NORMAL, theme::MUTED);
    // The track that plays stands out among the tracks.
    for (int i = 0; list_ && i < listed_; ++i) {
      const tessera::Tone tone = i + 1 == number ? tessera::Tone::ACCENT : tessera::Tone::NORMAL;
      ui::set_tone(lv_obj_get_child(list_, 2 * i), tone, theme::INK);
      ui::set_tone(lv_obj_get_child(list_, 2 * i + 1), tone, theme::MUTED);
    }
    // The key: "Play on Living room" when that fits, else "Play"; "Pause" and "Go on" while the album is on.
    const int icon_w = ui::line_height(Font::ICON_SMALL), space = ui::px(8), inner = ui::px(22);
    std::string words = plugin_->text(now ? "pause" : waits ? "resume" : sent_ == Sent::YES ? "sent"
                                      : sent_ == Sent::FAILED ? "retry" : "play");
    const std::string name = plain(plugin_->player_name);
    if (!name.empty() && !now && !waits && sent_ == Sent::NO) {
      const std::string on = tessera::fill(plugin_->text("play_on"), "player", name);
      if (inner + icon_w + space + ui::text_width(on, Font::BODY_LARGE) + inner <= width_) words = on;
    }
    const int tw = std::max(1, std::min(ui::text_width(words, Font::BODY_LARGE) + 1, width_ - 2 * inner - icon_w - space));
    key_w_ = inner + icon_w + space + tw + inner;
    key_x_ = x0_;
    lv_obj_set_size(key_, key_w_, key_h_);
    lv_obj_set_pos(key_, key_x_, key_y_);
    lv_obj_set_pos(key_icon_, inner, (key_h_ - ui::line_height(Font::ICON_SMALL)) / 2);
    ui::set_text(key_icon_, ui::icon(now ? ICON_PAUSE : ICON_PLAY));
    lv_obj_set_width(key_text_, tw);
    lv_obj_set_pos(key_text_, inner + icon_w + space, (key_h_ - ui::line_height(Font::BODY_LARGE)) / 2);
    ui::set_text(key_text_, words);
    // The key's colour says it too: green while it plays, amber when Home Assistant was not reached.
    ui::set_fill(key_, now ? tessera::Tone::GOOD : sent_ == Sent::FAILED ? tessera::Tone::BUSY : tessera::Tone::NORMAL,
                 theme::ACCENT);
    ui::set_text(note_, player ? "" : plugin_->text("no_player"));
    if (volume_) {
      char percent[8] = "";
      if (level_ >= 0) snprintf(percent, sizeof(percent), "%d%%", static_cast<int>(level_ * 100.0f + 0.5f));
      ui::set_text(volume_, percent);
    }
  }

  AlbumOfTheDay *plugin_;
  Art art_;
  int at_ = 0, width_ = 0, pad_ = 0, x0_ = 0;
  int key_x_ = 0, key_y_ = 0, key_w_ = 0, key_h_ = 0;
  int count_ = 0, listed_ = 0, round_ = 0, round_y_ = 0;
  float level_ = -1.0f;
  bool on_ = false;
  Sent sent_ = Sent::NO;
  uint32_t sent_at_ = 0;
  tessera::Text player_, also_[2], state_, album_, playing_artist_, track_;
  lv_obj_t *place_{}, *title_{}, *artist_{}, *tagline_{}, *now_{}, *note_{}, *key_{}, *key_icon_{}, *key_text_{};
  lv_obj_t *tracks_{}, *quieter_{}, *louder_{}, *volume_{}, *list_{}, *more_{};
};

void AlbumOfTheDay::setup() {
  add_tile("album", [this]() { return new AlbumTile(this); });
  add_card("album", [this]() { return new AlbumCard(this); });
}

}  // namespace esphome::album_of_the_day
