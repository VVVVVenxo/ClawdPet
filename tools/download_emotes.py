#!/usr/bin/env python3
"""Download all 24 clawd emotes from GitHub gallery to src/anim/emotes/."""

import os
import urllib.request

REPO_BASE = "https://raw.githubusercontent.com/xixicc186/clawd-emotes-skill/main/gallery"
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "anim", "emotes")

EMOTES = [
    "clawd-birthday.gif",
    "clawd-christmas.gif",
    "clawd-coding.gif",
    "clawd-coffee.gif",
    "clawd-dragon-boat.gif",
    "clawd-eating.gif",
    "clawd-exercise.gif",
    "clawd-gaming.gif",
    "clawd-guitar.gif",
    "clawd-halloween.gif",
    "clawd-lantern.gif",
    "clawd-listening.gif",
    "clawd-mid-autumn.gif",
    "clawd-new-year.gif",
    "clawd-painting.gif",
    "clawd-photo.gif",
    "clawd-qixi.gif",
    "clawd-reading.gif",
    "clawd-shower.gif",
    "clawd-singing.gif",
    "clawd-sleeping.gif",
    "clawd-spring.gif",
    "clawd-valentine.gif",
    "clawd-watering.gif",
]


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    total = 0
    for name in EMOTES:
        url = f"{REPO_BASE}/{name}"
        dest = os.path.join(OUT_DIR, name)
        try:
            urllib.request.urlretrieve(url, dest)
            size = os.path.getsize(dest)
            total += size
            print(f"  OK  {name}  ({size:,} bytes)")
        except Exception as e:
            print(f"  FAIL {name}  ({e})")
    print(f"\nTotal: {len(EMOTES)} files, {total:,} bytes ({total/1024:.1f} KB)")


if __name__ == "__main__":
    main()
