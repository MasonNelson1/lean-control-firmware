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
