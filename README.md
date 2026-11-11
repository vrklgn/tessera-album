# Album of the day

One album a day on a [Tessera](https://github.com/MaxGramser/homeassistant_espscreen) screen, from Apple Music's list of
the [100 Best Albums](https://100best.music.apple.com/us): its cover, its place in the list, its title and its artist.
Tap the tile to open the album with its cover large and a sentence about it; a key there plays it on the speakers you
chose, through Music Assistant in Home Assistant. While it plays, the card says which track is on, the key pauses it
and lets it go on, and round keys show its tracks and turn the volume down and up. There is no key to skip a track, on
purpose: an album is heard from its first track to its last.

The album changes at midnight. Counting down, the tile starts somewhere in the list and goes from number 100 to number
1 in a hundred days, then starts again; mixed, it jumps through the list and still shows every album once in a hundred
days.

This plugin is not made by or with Apple. The list and the sentences about the albums are Apple Music's.

## Set up

1. In the Tessera app, open **Plugins**, choose **Add with a link** and give it
   `https://github.com/vrklgn/tessera-album`. Add the plugin to a screen; the screen builds once.
2. In Layout, place the **Album of the day** tile from the library's Plugins group. One cell shows the cover alone; a
   wide tile shows the cover beside the words; a tall tile shows the cover over the title and artist. A large tile
   (about three by three cells and up, to five by five) is a deck: the cover and the words at the top with keys that
   play, pause and turn the volume, a bar that says how far into the album you are, and under it every track with its
   length and the album's whole length, the track that plays standing out. A tap on a key works it; a tap anywhere
   else opens the album's card.
3. To play from the screen, choose the tile's player in the inspector: it offers the players of
   [Music Assistant](https://www.music-assistant.io) in Home Assistant. A Music Assistant group plays on all its
   speakers.
4. Optional: choose a **Second speaker** and a **Third speaker**. They are grouped with the tile's player when you
   press Play.
5. **Order** chooses between counting down and a mixed order.
6. **Background** chooses the card behind the tile: **Default**, **None** (the cover and the words stand on the page
   itself), or a soft blue, yellow, purple or green.

## Good to know

- Music Assistant finds the album by its title and artist on its own services (Spotify, Apple Music, Tidal, your own
  files), so it plays the best match it finds there, which may be another edition than the one in Apple's list.
- Play replaces what the player's queue held.
- The second and third speaker are joined to the tile's player with Home Assistant's `media_player.join`, and stay
  grouped afterwards. This needs speakers that can be grouped with that player; a Music Assistant group with fixed
  members takes no others, so choose either the group or single speakers.
- The screen must be allowed to perform Home Assistant actions (the ESPHome integration's option, which Tessera turns
  on when it adds the screen).
- The deck needs room for at least four tracks under its cover; a tile with less shows the smaller form. An album with
  more tracks than the deck has lines (the longest has forty) shows the part round the track that plays.
- The bar fills up to the start of the track that plays: it counts whole tracks, not the seconds within one.
- The volume keys step five in a hundred on the tile's player (a Music Assistant group turns all its speakers); the
  speakers joined as second and third keep the volume they had.
- The track that plays is found among the album's tracks by its name, so its place ("3/16") shows only where the
  service names the track as Apple does.
- The round key with the list on the album's card lays the tracks over it, in two columns where they fit; a long album ends in
  "+ 4 more", and the track that plays stands out. A tap anywhere, or Back, takes them away again.
- With a background other than Default the cover has square corners. The soft colours are the four colours of the
  Tessera mark, thinned; they are darker in the dark look.
- The background is the plugin's own doing: it changes the look of the screen's card behind the tile, which the plugin
  API has no call for. If a later Tessera paints its cards another way, this option may stop working; the rest of the
  tile does not depend on it.
- The album of the day follows the date alone, so every screen with the same order shows the same album.
- A board without the memory for pictures (the CYD, the Waveshare 3.5-inch, the Hosyond 4-inch) shows the album's
  place in the list as a large number instead of its cover.
- The list is built into the screen. `python3 tools/build_albums.py` reads it again from Apple's site and writes
  `components/album_of_the_day/albums.h`.

## Privacy

Tessera (the app in Home Assistant, not the screen) fetches the cover of the day's album from Apple's picture host,
`is1-ssl.mzstatic.com`. Apple sees the address of your Home Assistant and which cover was asked for; nothing else of
your home. Playing goes from the screen to Home Assistant and Music Assistant only. The screen never reaches the
internet.
