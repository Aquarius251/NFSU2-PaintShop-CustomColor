# NFSU2 - Paint Shop: Custom color everything

Any colour for every part of your car in **Need for Speed Underground 2** (PC, v1.2).
It is an ASI plugin. The game's own files are never changed.

**Download:** see [Releases](../../releases). Get `NFSU2_CustomColor_v1.2.zip`.

## New in v1.2

- **Lightness slider** (0-300 %) next to Saturation. Each part, vinyl and decal has its own, and My colours remembers it.
  Below 100 % is darker. Above 100 % the dark shading fills in first, then it gets brighter: clean, bright white paint and rims.
- **Rims: Shine down to 0 % = matte.** It fades out the chrome reflection and highlights and evens out the dark faces,
  so white or coloured rims look like painted matte rims. The tyres are never touched.
- **Shine 0 % on paint** switches off the paint type's own pearl/metallic tint, so the colour shows exactly as picked.
- The Shine slider changes smoothly near 0 %, with no jump.

## What it does

- The paint, vinyl and decal shops get a **Custom color** button. Click it and a colour picker replaces the preset colours.
  **Preset colors** takes you back to the game's own colours.
- Works for: body paint, rims, spinners, brake calipers, spoiler, mirrors, roof scoop, engine, exhaust tips,
  trunk audio, all vinyl colours and decals (each decal spot has its own).
- Picture vinyls (Art Factory, Contest, Unique) can be tinted too. **Default** shows the picture as it was drawn.
- The car on the turntable changes live while you pick.
- **Saturation slider** (0-200 %). Each part, vinyl and decal has its own, and dark colours stay rich.
- **Lightness slider** (0-300 %). Darker or brighter, up to clean white. Each part, vinyl and decal has its own.
- **Base + Shine** colours for paints and rims give a two-tone pearl. A slider sets how strong the shine is.
  On rims, Shine at 0 % gives a matte painted look (no chrome).
- Gloss / Metallic / Pearlescent still work. Your colour stays, and only the finish changes.
- **My colours**: 10 saved swatches, with their saturation and lightness, shared by all cars.
- Every vinyl layer and decal slot keeps its colour while you browse, install, leave the shop or restart.
- K / L turn the car in the shops (hold; Shift = faster).
- Colours are saved per car and stay in races, in free roam and after a restart.
- Works with HD vinyl / decal texture packs (up to 4096 x 4096).

## Install

1. Copy `dinput8.dll` and the `scripts` folder from the zip next to `SPEED2.EXE`.
   If you already have a `dinput8.dll` (from WidescreenFix or most mod packs), keep yours.
2. Start the game. The settings are in `scripts\NFSU2_CustomColor.ini`.

Needs NFSU2 PC **v1.2**. WidescreenFix (ThirteenAG) is compatible and recommended.

## Picker controls

| Input | Action |
|---|---|
| Mouse | drag in the square (shade) and on the rainbow bar (colour) |
| Left / Right | colour |
| Up / Down | brightness |
| Home / End | saturation |
| PgUp / PgDn | other part |
| Del | back to the game's colour |
| F4 | open the picker anywhere |

## Where your colours are saved

- `scripts\NFSU2_CustomColor.txt`: your colours.
- `scripts\NFSU2_CustomColor_presets.txt`: My colours.

If the game is under `C:\Program Files (x86)`, Windows keeps these files in
`%LOCALAPPDATA%\VirtualStore\Program Files (x86)\EA GAMES\Need for Speed Underground 2\scripts\`.

## Building from source

You need the Visual Studio 2019 Build Tools (x86) and Windows SDK. Run `build.bat`, which writes `build\NFSU2_CustomColor.asi`.
The release build is compiled with `/DNFSU2CC_SHIP`, which leaves out some test-only features.

## Credits

- Made with the help of Claude (AI assistant).
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) by ThirteenAG (MIT), included in the release zip.
- Need for Speed Underground 2 is a trademark of Electronic Arts. This is an unofficial fan mod.

## Licence

MIT, see [LICENSE](LICENSE).
