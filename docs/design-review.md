# Reviewing the design

The design system lives in `src/design/tokens.json` and the [design system guide](design-system.md)
explains how the code consumes it. This page is about *looking* at the result: one place to review
the spaces, the typography, the sizes, the icons, the colors and every component against the
guidelines, in the app, from the command line, or from an agent through MCP. All three read the
same data (`DesignAudit`, `src/utils/designaudit.h`) and render the same pages
(`DesignReviewWindow`, `src/widgets/designreviewwindow.h`).

## In the app: Help → Design Review…

A window with six pages and a Dark / Light switch that changes the application theme while it is
open:

| Page | Shows |
| --- | --- |
| Colors | Every color token with its swatch, both theme values and its usage, then the contrast matrix: every ink on every surface with its WCAG ratio; a cell is outlined when it fails the level its usage asks for (4.5:1 for text, 3:1 for `ink-tertiary`). |
| Type | Every type style rendered with the bundled fonts at its design size, with family, size, line height, weight and tracking; then the fonts the live main window sets on widgets, matched against the type styles. A font without a style is a size or weight the design does not define. |
| Spacing & Sizes | The spacing scale drawn at its pixel values, the radii, every fixed size with a real control at that size, the opacities. |
| Icons | The icon theme (one cell per icon, filterable) and the icons the main window asks for that the theme does not draw, each drawn by the fallback theme. |
| Components | Every control the Kdenlive style paints in four columns: normal, hover (simulated), pressed or checked, disabled. Buttons in all variants (primary, segmented, page bar, panel toggle, tool buttons), selection, fields including the timecode and precision spin boxes, sliders, progress, scroll bars, tab bars, group box, menu bar, tree and list views, text, badges and inline messages. |
| Live Inventory | Findings over the running main window (fonts, margins and spacings off the scale, control heights and icon sizes off the sizes, icons outside the theme, style sheets, custom palettes and text slips such as an ASCII ellipsis) and the full widget tree with class, object name, geometry, font and layout margins, filterable. |

**Export…** writes the pages, the main window and the open panels as PNG files for both themes,
plus `review.json`, into a folder of your choice.

## From the command line: the review report

```sh
python3 src/design/review-design.py            # uses build/bin/kdenlive, writes build/design-review/
python3 src/design/review-design.py --binary ~/.local/opt/kdenlive-live/bin/kdenlive --project ~/Videos/paris.kdenlive
```

The script runs the editor headless (`kdenlive --design-review <folder>`, offscreen, isolated
config) on a generated fixture project or the one you pass, lets it settle, and captures every
page, the window and *every* panel in both themes. It then adds the source audits and writes
`index.html`:

- the counts at the top: contrast failures, fonts without a type style, margins off the scale,
  icons outside the theme, style sheets, literal colors, `.ui` values;
- the window, the panels and the pages, with a dark / light / both switch;
- the color, type and length tokens as tables;
- the live findings from `review.json`;
- the source findings: `check-design.py`, icon names the sources ask for outside the theme,
  `.ui` files with margins, spacings, point sizes, style sheets or colors that bypass the tokens,
  and literal colors in C++ (content code such as scopes and the titler is skipped; mark a line
  `design:allow` to accept it).

Open `index.html` in a browser, or hand the folder to an agent: `report.json` holds everything the
page shows. `--strict` exits 1 when a source audit or a contrast check fails, for CI.
`--skip-run` rebuilds the audits and the page from an existing `review.json`.

The private Qt HTTP modules of the MCP build must be on `LD_LIBRARY_PATH` as for the acceptance
test. `KDENLIVE_DESIGN_REVIEW_WINDOW` (default `1600x1000` from the script) sizes the window and
`KDENLIVE_DESIGN_REVIEW_SETTLE_MS` is the wait before capturing.

## From an agent: the MCP tools

With the [native MCP API](native-mcp.md) enabled, two read-only tools let an agent review the
interface the user has open:

- `desktop_design_audit {include?, panel?}` returns the tokens, the colors with both theme values,
  the contrast matrix and its failures per theme, the type styles, the theme icons and the icons in
  use, the live findings and the list of panels (name, title, open, current). `include: ["inventory"]`
  adds the widget tree; `panel` restricts findings and inventory to one dock.
- `desktop_ui_capture {target, page?, panel?, theme?, scale?, width?}` returns a PNG of the main
  window, one open panel (raised if it is behind another tab) or one review page rendered at least
  `width` logical pixels wide without showing the window. `theme` switches the application theme for the
  capture and restores it afterwards; `scale` up to 3 gives a sharper picture.

A review conversation then goes: audit, look at the window and the pages, name the token or
component to change, edit `tokens.json` or the style, rebuild, capture again.

## Working on the guidelines

1. Change a value in `tokens.json` (and in the design system page, they must stay identical), or
   a drawing in `kdenlivestyle.cpp`.
2. Rerun the generators when colors or icons changed: `generate-color-schemes.py`,
   `generate-icon-theme.py`.
3. Build, then `review-design.py` and compare `index.html` before and after: the pages show the
   token, the components show how every control took it, the window and panels show it in context.
4. Keep `check-design.py --strict` and the contrast failures at zero; the live findings are the
   list of places still to bring onto the tokens.
