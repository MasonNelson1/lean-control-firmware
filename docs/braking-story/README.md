# Braking story

A local site that explains the chair's two braking problems from the physics up,
using the 11/22 court logs. It's a working document: sections get added, edited,
reordered and deleted as the understanding improves.

Run it:

    python3 docs/braking-story/serve.py        # http://localhost:8090

Saving any file in this folder reloads open pages.

- `story.json`: section order and nav labels. Add `"hidden": true` to hide one.
- `sections/*.html`: one file per section. Plain HTML plus an optional inline
  `<script>`; `root` is the section element and `Story` has the chart helper
  (`Story.line`, `Story.legend`), the shared constants (`Story.C`) and
  `Story.data(name)` for JSON in `data/`.
- `story.js`: section loader, chart helper and the shared constants. Change a
  constant there and every chart follows.
- `data/`: log excerpts cut with `tools/extract_fault.py LOG START END OUT.json`.

Labels on numbers: `measured` (logs, VESC config), `estimate` (measured numbers
plus an assumption), `illustration` (a teaching shape, not our data).

Notes: the ✎ Notes pill (bottom right) opens a small notebook. Select text
anywhere on the page and press **+ Note** to save the quote as a link back to
that spot; clicking the quote later scrolls there and highlights it. The server
saves notes to `notes.json` and a readable `notes.md` in this folder (saving a
note doesn't reload the page). Opened without the server, notes stay in that
browser only.
