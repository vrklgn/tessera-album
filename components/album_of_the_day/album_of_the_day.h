#pragma once
#include "esphome/core/component.h"
#include "esphome/components/smart_display/plugin_api.h"

namespace esphome::album_of_the_day {

// The plugin: the tile "album" and the card "album" a tap on the tile opens. The tile hands the card what it shows
// through here, as the card is made later and knows only the entity and the tile it came from.
class AlbumOfTheDay : public Component, public tessera::Plugin {
 public:
  void setup() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  int chosen = -1;             // the album the card shows, its place in ALBUMS
  tessera::Text also[2];       // the other speakers of the tile, "" for none
  tessera::Text player_name;   // the player's name in Home Assistant, for the card's key
};

}  // namespace esphome::album_of_the_day
