#!/usr/bin/env python3
"""Build one self-contained HTML file of the braking story (opens by double-click,
no server). Notes in the bundle are saved in that browser only.

    python3 docs/braking-story/tools/bundle.py [OUT.html]
"""
import json, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "dist", "lean-braking-story.html")
read = lambda p: open(os.path.join(ROOT, p), encoding="utf-8").read()

story = json.loads(read("story.json"))
bundle = {
    "story": story,
    "sections": {s["file"]: read("sections/" + s["file"]) for s in story["sections"] if not s.get("hidden")},
    "data": {f: json.loads(read("data/" + f)) for f in sorted(os.listdir(os.path.join(ROOT, "data"))) if f.endswith(".json")},
}
# keep "</script>" inside section text from closing the bundle's script tag
payload = json.dumps(bundle, ensure_ascii=False).replace("</", "<\\/")
html = read("index.html")
html = html.replace('<link rel="stylesheet" href="story.css">', "<style>\n" + read("story.css") + "\n</style>")
html = html.replace('<script src="story.js"></script>', "<script>window.STORY_BUNDLE = " + payload + ";</script>\n<script>\n" + read("story.js") + "\n</script>")
html = html.replace('<script src="notes.js"></script>', "<script>\n" + read("notes.js") + "\n</script>")
html = html.replace("Saving any file reloads this page.", "Bundled copy: notes stay in this browser.")
assert 'src="story.js"' not in html and 'href="story.css"' not in html
os.makedirs(os.path.dirname(out), exist_ok=True)
open(out, "w", encoding="utf-8").write(html)
print(out, f"{len(html) / 1024:.0f} KB")
