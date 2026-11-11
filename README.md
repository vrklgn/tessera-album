# Album of the day

One album a day on a [Tessera](https://github.com/MaxGramser/homeassistant_espscreen) screen, from Apple Music's list of
the [100 Best Albums](https://100best.music.apple.com/us): its cover, its place in the list, its title and its artist.
Tap the tile to open the album with its cover large and a sentence about it; a key there plays it on the speakers you
chose, through Music Assistant in Home Assistant.

The album changes at midnight. Counting down, the tile starts somewhere in the list and goes from number 100 to number
1 in a hundred days, then starts again; mixed, it jumps through the list and still shows every album once in a hundred
days.

This plugin is not made by or with Apple. The list and the sentences about the albums are Apple Music's.

## Set up

1. In the Tessera app, open **Plugins**, choose **Add with a link** and give it
   `https://github.com/vrklgn/tessera-album`. Add the plugin to a screen; the screen builds once.
2. In Layout, place the **Album of the day** tile from the library's Plugins group. One cell shows the cover alone; a
   wide tile shows the cover beside the words; a tall tile shows the cover over the title and artist.
3. To play from the screen, choose the tile's player in the inspector: it offers the players of
   [Music Assistant](https://www.music-assistant.io) in Home Assistant. A Music Assistant group plays on all its
   speakers.
4. Optional: choose a **Second speaker** and a **Third speaker**. They are grouped with the tile's player when you
   press Play.
5. **Order** chooses between counting down and a mixed order.

## Good to know

- Music Assistant finds the album by its title and artist on its own services (Spotify, Apple Music, Tidal, your own
  files), so it plays the best match it finds there, which may be another edition than the one in Apple's list.
- Play replaces what the player's queue held.
- The second and third speaker are joined to the tile's player with Home Assistant's `media_player.join`, and stay
  grouped afterwards. This needs speakers that can be grouped with that player; a Music Assistant group with fixed
  members takes no others, so choose either the group or single speakers.
- The screen must be allowed to perform Home Assistant actions (the ESPHome integration's option, which Tessera turns
  on when it adds the screen).
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
