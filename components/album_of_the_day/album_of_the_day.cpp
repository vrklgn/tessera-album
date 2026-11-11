#include "album_of_the_day.h"
#include "albums.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

namespace esphome::album_of_the_day {

using tessera::Font;
namespace ui = tessera::ui;

// What plays it: Music Assistant finds the album by its title and artist on whatever service it has.
static const char *const PLAY = "music_assistant.play_media";
// What makes the other speakers play along with the tile's player.
static const char *const JOIN = "media_player.join";
static const uint32_t ICON_PLAY = 0xF040A;

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

// Whether the player plays this album now: its album starts with the title (a service may add "(Remastered)" to it)
// and, when it names one, its artist starts with the album's.
static bool playing(const Album &a, const std::string &state, const std::string &album, const std::string &artist) {
  const size_t t = strlen(a.title), n = strlen(a.artist);
  if (state != "playing" || album.size() < t || !same(album.c_str(), a.title, t)) return false;
  return artist.empty() || (artist.size() >= n && same(artist.c_str(), a.artist, n));
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

// The cover's place: a picture's, or on a board without pictures (the CYD) a block with the album's place in the list
// as a large number.
struct Art {
  lv_obj_t *place = nullptr, *block = nullptr, *number = nullptr;
  int side = 0;

  void make(lv_obj_t *parent, int x, int y, int s) {
    side = s;
    if (ui::pictures()) {
      place = ui::picture(parent);
      lv_obj_set_size(place, s, s);
      lv_obj_set_pos(place, x, y);
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
// entity is the Music Assistant player that plays it.
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
    const int th = ui::line_height(Font::TITLE), bh = ui::line_height(Font::BODY), gap = ui::px(8);
    if (c.columns == 1 && c.rows == 1) {
      // One cell: the cover over the whole card.
      const int s = std::min(width_, height_);
      art_.make(c.parent, (width_ - s) / 2, (height_ - s) / 2, s);
    } else if (width_ * 10 >= height_ * 13) {
      // Lying down: the cover at the left, as high as the card, the words beside it in the middle of its height.
      const int s = std::min(height_, width_ / 2);
      art_.make(c.parent, 0, (height_ - s) / 2, s);
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
      art_.make(c.parent, (width_ - s) / 2, 0, s);
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
    if (data["state"].isNull()) return;
    plugin_->player_name = tessera::Text(data["name"] | "");
  }

  // The album follows the clock: another one at midnight.
  void on_tick(uint32_t epoch) override {
    const int now = album_of(epoch, mixed_);
    if (now == at_ && shown_) return;
    at_ = now;
    shown_ = true;
    show();
  }

  void on_theme() override {
    art_.paint();
    if (place_) ui::set_color(place_, theme::MUTED);
    if (title_) ui::set_color(title_, theme::INK);
    if (artist_) ui::set_color(artist_, theme::MUTED);
    if (tagline_) ui::set_color(tagline_, theme::MUTED);
  }

  // The card of the album, with what the tile knows: the card is made after this and reads it from the plugin.
  void on_tap() override {
    if (at_ < 0) return;
    plugin_->chosen = at_;
    plugin_->also[0] = also_[0];
    plugin_->also[1] = also_[1];
    tessera::open_card(plugin_->plugin_id(), "album", plain(player_), tile_);
  }

 private:
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

  void show() {
    const Album *a = at_ >= 0 ? &ALBUMS[at_] : nullptr;
    art_.show(a);
    if (!title_) return;
    ui::set_text(title_, a ? a->title : "");
    if (stacked_) {
      ui::set_text(artist_, a ? a->artist : "");
      return;
    }
    const int th = ui::line_height(Font::TITLE), bh = ui::line_height(Font::BODY), space = ui::px(2), gap = ui::px(8);
    ui::set_text(place_, a ? place_line(plugin_, *a, words_w_, Font::BODY, true) : "");
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
  tessera::Text player_, also_[2];
  lv_obj_t *place_{}, *title_{}, *artist_{}, *tagline_{};
};

// The card of the album: its cover large, then its place in the list and its year over its title, the artist under
// it, a sentence about it, and a key that plays it on the tile's Music Assistant player and the other speakers chosen.
// The key says how that goes: starting, playing, or try again.
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
    const int bh = ui::line_height(Font::BODY), bl = ui::line_height(Font::BODY_LARGE);
    key_h_ = std::max(ui::mm(9), bl + ui::px(18));
    const int foot = player_.empty() ? 2 * bh : key_h_;
    // The words, from the top: the place and year, the title, the artist, the sentence, and the key a gap under it.
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
      side = std::max(c.height * 35 / 100,
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
    int y, cover_y;
    if (beside) {
      const int top = std::max(pad_, (c.height - std::max(side, block)) / 2);
      cover_y = top + std::max(0, (block - side) / 2);
      y = top + std::max(0, (side - block) / 2);
      art_.make(c.parent, pad_, cover_y, side);
    } else {
      cover_y = pad_;
      y = pad_ + side + ui::px(14);
      art_.make(c.parent, (c.width - side) / 2, cover_y, side);
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
    key_y_ = y + ui::px(12);
    // The key, as wide as its words, a finger high at least.
    key_ = plain_block(c.parent, theme::ACCENT);
    lv_obj_set_style_radius(key_, key_h_ / 2, 0);
    key_icon_ = ui::label(key_, Font::ICON_SMALL, theme::ON_ACCENT);
    ui::set_text(key_icon_, ui::icon(ICON_PLAY));
    key_text_ = ui::label(key_, Font::BODY_LARGE, theme::ON_ACCENT);
    note_ = ui::text(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_size(note_, width_, 2 * bh);
    lv_obj_set_pos(note_, x0_, key_y_);
    paint();
  }

  void on_state(JsonObjectConst data) override {
    if (data["state"].isNull()) return;
    state_ = tessera::Text(data["state"] | "");
    album_ = tessera::Text(data["attributes"]["media_album_name"] | "");
    playing_artist_ = tessera::Text(data["attributes"]["media_artist"] | "");
    plugin_->player_name = tessera::Text(data["name"] | "");
    paint();
  }

  void on_theme() override {
    art_.paint();
    ui::set_color(place_, theme::MUTED);
    ui::set_color(title_, theme::INK);
    ui::set_color(artist_, theme::INK_SOFT);
    if (tagline_) ui::set_color(tagline_, theme::MUTED);
    ui::set_color(note_, theme::MUTED);
    ui::set_color(key_icon_, theme::ON_ACCENT);
    ui::set_color(key_text_, theme::ON_ACCENT);
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
    const tessera::Point p = tessera::tap_point();
    if (player_.empty() || p.y < key_y_ - ui::px(8) || p.y > key_y_ + key_h_ + ui::px(8)) return;
    if (p.x < key_x_ - ui::px(8) || p.x > key_x_ + key_w_ + ui::px(8)) return;
    const Album &a = ALBUMS[at_];
    const std::string player = plain(player_);
    // The other speakers join the tile's player, so all of them play the one album together.
    JsonDocument group;
    JsonArray members = group["group_members"].to<JsonArray>();
    const std::string first = plain(also_[0]), second = plain(also_[1]);
    if (!first.empty() && first != player) members.add(first);
    if (!second.empty() && second != player && second != first) members.add(second);
    if (members.size() > 0) tessera::action(JOIN, player, group);
    JsonDocument d;
    d["media_id"] = a.title;
    d["media_type"] = "album";
    d["artist"] = a.artist;
    d["enqueue"] = "replace";
    sent_ = tessera::action(PLAY, player, d) ? Sent::YES : Sent::FAILED;
    sent_at_ = esphome::millis();
    paint();
  }

 private:
  enum class Sent : uint8_t { NO, YES, FAILED };

  // The key and what it says, again with every change of the player.
  void paint() {
    const Album &a = ALBUMS[at_];
    const bool now = playing(a, plain(state_), plain(album_), plain(playing_artist_));
    if (now) sent_ = Sent::NO;
    // "Play on Living room" when that fits, else "Play".
    const int icon_w = ui::line_height(Font::ICON_SMALL), space = ui::px(8), inner = ui::px(22);
    std::string words = plugin_->text(now ? "playing" : sent_ == Sent::YES ? "sent" : sent_ == Sent::FAILED ? "retry" : "play");
    const std::string name = plain(plugin_->player_name);
    if (!name.empty() && !now && sent_ == Sent::NO) {
      const std::string on = tessera::fill(plugin_->text("play_on"), "player", name);
      if (inner + icon_w + space + ui::text_width(on, Font::BODY_LARGE) + inner <= width_) words = on;
    }
    const int tw = std::min(ui::text_width(words, Font::BODY_LARGE) + 1, width_ - 2 * inner - icon_w - space);
    key_w_ = inner + icon_w + space + tw + inner;
    key_x_ = x0_;
    lv_obj_set_size(key_, key_w_, key_h_);
    lv_obj_set_pos(key_, key_x_, key_y_);
    lv_obj_set_pos(key_icon_, inner, (key_h_ - ui::line_height(Font::ICON_SMALL)) / 2);
    lv_obj_set_width(key_text_, tw);
    lv_obj_set_pos(key_text_, inner + icon_w + space, (key_h_ - ui::line_height(Font::BODY_LARGE)) / 2);
    ui::set_text(key_text_, words);
    if (player_.empty()) lv_obj_add_flag(key_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(key_, LV_OBJ_FLAG_HIDDEN);
    // The key's colour says it too: green while it plays, amber when Home Assistant was not reached.
    ui::set_fill(key_, now ? tessera::Tone::GOOD : sent_ == Sent::FAILED ? tessera::Tone::BUSY : tessera::Tone::NORMAL,
                 theme::ACCENT);
    ui::set_text(note_, player_.empty() ? plugin_->text("no_player") : "");
  }

  AlbumOfTheDay *plugin_;
  Art art_;
  int at_ = 0, width_ = 0, pad_ = 0, x0_ = 0;
  int key_x_ = 0, key_y_ = 0, key_w_ = 0, key_h_ = 0;
  Sent sent_ = Sent::NO;
  uint32_t sent_at_ = 0;
  tessera::Text player_, also_[2], state_, album_, playing_artist_;
  lv_obj_t *place_{}, *title_{}, *artist_{}, *tagline_{}, *key_{}, *key_icon_{}, *key_text_{}, *note_{};
};

void AlbumOfTheDay::setup() {
  add_tile("album", [this]() { return new AlbumTile(this); });
  add_card("album", [this]() { return new AlbumCard(this); });
}

}  // namespace esphome::album_of_the_day
