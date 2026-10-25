# Main screen redesigns (proposal)

High-fidelity mockups of five redesigns of the launcher's main screen, for choosing one before it is built in `native/ui`.
Each page is 2048x1104 CSS px = DIPs of the native window, rendered at 125 % (2560x1380), with the real builds
(3.6, 14.30, 17.50), season colors from `native/ui/art.cpp` and the battle-pass skins the launcher shows.

- `a-lobby.html`: Fortnite Chapter 1 lobby: build tabs on top, season card left, mode card and Launch bottom right
- `b-library.html`: library: build list on the left, hero art, Play tab with the settings, Launch in a footer on the right
- `c-carousel.html`: shelf: builds as big cards along the bottom, the selected one opens and carries Launch
- `d-modes.html`: modes first: two big mode cards with their own settings, one Launch in the middle, build dropdown
- `e-cinema.html`: poster: skin and version as key art, one big Launch bottom right, settings in a sheet

`node render.mjs [a b …]` writes `out/*.png` (Playwright; `PW_DIR` points at its install). The skins are downloaded to
`art/` from fortnite-api.com the first time.
