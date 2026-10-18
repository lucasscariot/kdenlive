# Kdenlive PRO design system

Kdenlive PRO's interface follows one design language, inspired by Apple's pro apps. The full
blueprint (principles, type scale, spacing, colors, components and their guidelines) is the
design system page; this file explains how the code consumes it so the interface cannot drift.

## One source of truth

`src/design/tokens.json` holds every design value: colors per theme, the doubling spacing scale,
radii, sizes, opacities and type styles. Everything else is derived from it:

| Consumer | How it reads the tokens |
| --- | --- |
| C++ widgets | `DesignTokens::color("ink")`, `space(3)`, `radius("radius-md")`, `size("control-md")`, `font("text-body")` (`src/utils/designtokens.h`) |
| QML | the `K.Design` singleton: `K.Design.colors["accent"]`, `K.Design.alpha("ink", 0.2)`, `K.Design.space3`, `K.Design.radiusMd`, `K.Design.fonts["text-caption"]` |
| Widget style | `KdenliveStyle` (`src/utils/kdenlivestyle.cpp`) paints every Qt control from the tokens |
| Shared shapes | `DesignPaint` (`src/utils/designpaint.h`): panels, pills, chevrons, header strips, washes, badge style sheets |
| KDE color schemes | generated: `python3 src/design/generate-color-schemes.py` writes `data/color-schemes/KdenliveDark.colors` and `KdenliveLight.colors` |
| Icons | generated: `python3 src/design/generate-icon-theme.py <lucide-static/icons>` writes the Kdenlive PRO icon themes |
| Fonts | Inter and JetBrains Mono are bundled in `src/design/fonts` and registered by `DesignTokens::loadFonts()` |

The design system page and `tokens.json` must stay identical: change a value in one, copy it to
the other, then rerun the two generators.

## Rules

- **No literal colors in interface code.** Use a color token. Content colors (scopes, the
  titler, thumbnails, colors chosen by the user) are the exception; mark such a line with a
  `design:allow` comment.
- **No pixel values between spacing steps.** Margins and gaps are `space(1)` 2px to `space(7)`
  128px, doubling at each step. Control heights are `control-sm`, `control-md`, `control-lg`.
- **No system font lookups.** Use `UiUtils::smallFont()`, `UiUtils::fixedFont()` or a named type
  style with `DesignTokens::font()`.
- **Draw shapes with DesignPaint**, and opt into variants with the style properties documented in
  `kdenlivestyle.h` (`_kdenlive_primary`, `_kdenlive_segmented`, `_kdenlive_pagebar`,
  `_kdenlive_panel_toggle`) instead of painting a widget by hand.
- **One icon per meaning.** Add an icon name to the map in `generate-icon-theme.py` instead of
  picking a different drawing in one place.

## Checking

`python3 src/design/check-design.py` lists colors, palette lookups, fonts and style sheets that
bypass the tokens. Run it with `--strict` before sending changes; it exits non zero when it finds
anything.

## Reviewing

**Help → Design Review…** opens the review window: every token as rendered, the contrast of every
ink on every surface, the type styles next to the fonts the window really uses, the icon theme and
the icons outside it, every component in its states, and a live inventory of the main window.
`python3 src/design/review-design.py` produces the same as pictures and tables in one HTML report,
and the MCP tools `desktop_design_audit` and `desktop_ui_capture` give it to an agent. See
[Reviewing the design](design-review.md).

## Scope

The Kdenlive style, fonts and icon themes apply when Qt falls back to its Fusion style, which is
the case outside Plasma. Desktops with a native style such as Breeze keep their own controls,
while QML views, the timeline colors and the custom widgets follow the tokens everywhere.
