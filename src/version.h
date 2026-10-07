#pragma once
// Wardrobe's version, written by release.py and compared with the latest
// GitHub release by the application. Date based: YYYY.MM.DD.N.
#define WARDROBE_VERSION "2026.10.07.2"
#define WARDROBE_REPOSITORY "pixl27/dota2mods"
#define WARDROBE_WIDEN_(text) L##text
#define WARDROBE_WIDEN(text) WARDROBE_WIDEN_(text)
#define WARDROBE_VERSION_W WARDROBE_WIDEN(WARDROBE_VERSION)
