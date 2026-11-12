#!/usr/bin/env python3
"""Writes components/album_of_the_day/albums.h: the hundred albums the screen picks from.

The list is Apple Music's 100 Best Albums as its site publishes it. The site gives the rank, the title, the artist and
the cover; the tagline and the blurb come from each album's own page there and the year from Apple's public iTunes Lookup. Run it
again when the list or a cover changed, look at the difference, and raise the plugin's version:

    python3 tools/build_albums.py

Only Python's standard library, and nothing of the person running it leaves in a request.
"""
import json
import re
import sys
import time
import urllib.request
from pathlib import Path

SITE = "https://100best.music.apple.com/us"
LOOKUP = "https://itunes.apple.com/lookup?country=us&id="
ART_HOST = "https://is1-ssl.mzstatic.com/image/thumb/"
OUT = Path(__file__).resolve().parent.parent / "components" / "album_of_the_day" / "albums.h"

# What a title carries of its edition: "(2019 Mix)", "[Expanded Edition]". Music Assistant finds the album by its
# title, and another service names its edition another way, so these go. "(Taylor's Version)" is the album's name.
EDITION = re.compile(r"deluxe|remaster|\bmix\b|edition|expanded|anniversary|bonus|platinum", re.I)
GROUP = re.compile(r"\s*(\([^()]*\)|\[[^\[\]]*\])\s*$")

# A title the list writes another way than the services that play it.
TITLES = {"Norman F*****g Rockwell!": "Norman Fucking Rockwell!"}

# The screen's fonts have plain punctuation.
PLAIN = {"’": "'", "‘": "'", "“": '"', "”": '"', "—": "-", "–": "-", "…": "...",
         " ": " "}


def get(url):
    request = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (album list builder)"})
    with urllib.request.urlopen(request, timeout=30) as answer:
        return json.loads(answer.read().decode("utf-8"))


def unflatten(data):
    """SvelteKit's data: one list in which every object and list holds the places of its values."""
    done = {}

    def at(i):
        if i < 0:
            return None
        if i in done:
            return done[i]
        value = data[i]
        if isinstance(value, dict):
            done[i] = out = {}
            for key, place in value.items():
                out[key] = at(place)
            return out
        if isinstance(value, list):
            done[i] = out = []
            out.extend(at(place) for place in value)
            return out
        return value

    return at(0)


def page(url):
    for node in get(url)["nodes"]:
        if node and node.get("type") == "data":
            return unflatten(node["data"])
    raise SystemExit(f"{url}: no data")


def plain(text):
    text = re.sub(r"<[^>]+>", "", text or "")
    for mark, with_ in PLAIN.items():
        text = text.replace(mark, with_)
    return re.sub(r"\s+", " ", text).strip()


def short(title):
    """The title without what names its edition."""
    title = plain(title)
    title = TITLES.get(title, title)
    while True:
        found = GROUP.search(title)
        if not found or not EDITION.search(found.group(1)) or found.start() == 0:
            return title
        title = title[:found.start()]


DASHED = re.compile(r"\s+-\s+[^-]*(remaster|\bmix\b|mono|stereo)[^-]*$", re.I)


def track(name):
    """A track's name without its edition: "Come Together (2019 Mix)", "Money - 2011 Remaster"."""
    return DASHED.sub("", short(name)) or plain(name)


def tracks_of(found):
    """The tracks of an album's page, wherever its data keeps them, in the order of the record."""
    if isinstance(found, dict):
        tracks = found.get("tracks")
        if isinstance(tracks, list) and tracks and isinstance(tracks[0], dict) and "name" in tracks[0]:
            # A line for each: its name, a tab, its length in seconds (0 when the page does not say).
            return [f'{track(t["name"])}\t{round((t.get("duration") or 0) / 1000)}' for t in tracks if t.get("name")]
        found = list(found.values())
    if isinstance(found, list):
        for part in found:
            names = tracks_of(part)
            if names:
                return names
    return []


def quoted(text):
    return json.dumps(text, ensure_ascii=False)


def main():
    albums = page(f"{SITE}/__data.json")["albums"]
    albums.sort(key=lambda a: int(a["position"]))
    ranks = [int(a["position"]) for a in albums]
    if ranks != list(range(1, 101)) or len({a["adamid"] for a in albums}) != 100:
        raise SystemExit(f"not the hundred albums: {len(albums)} of them, ranks {ranks[:3]}..{ranks[-3:]}")

    years = {}
    ids = [a["adamid"] for a in albums]
    for start in range(0, 100, 50):
        for found in get(LOOKUP + ",".join(ids[start:start + 50])).get("results", []):
            if found.get("collectionId") and found.get("releaseDate"):
                years[str(found["collectionId"])] = int(found["releaseDate"][:4])

    rows = []
    for album in albums:
        rank = int(album["position"])
        art = album["artwork"]["url"]
        if not art.startswith(ART_HOST) or not art.endswith("/{w}x{h}bb.jpg"):
            raise SystemExit(f"{rank}: a cover from somewhere else, {art}")
        tagline = blurb = ""
        detail = page(f"{SITE}/album/{rank}/__data.json")
        for section in detail["article"]["sections"]:
            if section.get("sectionName") == "tagline":
                tagline = plain(section.get("text"))
            # The page tells of the album in a few paragraphs; the first is the one about the album as a whole, and a
            # hundred of them are what a small screen's flash has room for.
            if section.get("sectionName") == "blurb" and not blurb:
                blurb = plain(section.get("text"))
        time.sleep(0.2)
        row = {"rank": rank, "title": short(album["name"]), "artist": plain(album["artistName"]),
               "year": years.get(album["adamid"], 0), "art": art[len(ART_HOST):-len("/{w}x{h}bb.jpg")],
               "tagline": tagline, "blurb": blurb, "tracks": "\n".join(tracks_of(detail))}
        rows.append(row)
        print(f'{rank:3} {row["title"]} / {row["artist"]} ({row["year"]}), {row["tracks"].count(chr(10)) + 1} tracks',
              file=sys.stderr)

    lines = [
        "#pragma once",
        "// The hundred albums, the first of the list first. Written by tools/build_albums.py: change that, not this.",
        "#include <cstdint>",
        "",
        "namespace esphome::album_of_the_day {",
        "",
        "struct Album {",
        "  uint8_t rank;          // 1 is the first of the list",
        "  uint16_t year;         // 0 when not known",
        "  const char *title;     // without its edition, as Music Assistant finds it",
        "  const char *artist;",
        "  const char *art;       // the cover's address between ART_HOST and ART_SIZE",
        "  const char *tagline;   // one sentence about it, \"\" for none",
        "  const char *blurb;     // a paragraph about it, \"\" for none",
        "  const char *tracks;    // its tracks in the order of the record, a line each (name, tab, seconds), or \"\"",
        "};",
        "",
        f"static const char *const ART_HOST = {quoted(ART_HOST)};",
        'static const char *const ART_SIZE = "/600x600bb.jpg";',
        f"static constexpr int ALBUM_COUNT = {len(rows)};",
        "",
        "static const Album ALBUMS[ALBUM_COUNT] = {",
    ]
    for row in rows:
        lines.append(f'    {{{row["rank"]}, {row["year"]}, {quoted(row["title"])}, {quoted(row["artist"])},')
        lines.append(f'     {quoted(row["art"])},')
        lines.append(f'     {quoted(row["tagline"])},')
        lines.append(f'     {quoted(row["blurb"])},')
        lines.append(f'     {quoted(row["tracks"])}}},')
    lines += ["};", "", "}  // namespace esphome::album_of_the_day", ""]
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(lines), encoding="utf-8")
    missing = [row["rank"] for row in rows if not row["year"] or not row["tagline"] or not row["blurb"] or not row["tracks"]]
    print(f"{OUT}: {len(rows)} albums" + (f", year, tagline, blurb or tracks missing for {missing}" if missing else ""))


if __name__ == "__main__":
    main()
