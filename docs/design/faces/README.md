# Face design reference

Reference renders for Taco's face aesthetic: glowing cyan eyes on a black
screen, with a soft outer glow, a crescent "moon" shadow for depth, a white
highlight dot, and small cheek/whisker accent marks. These are full device
mockups (bezel included) for art-direction purposes, not display-ready
sprites — the firmware renders faces as vector primitives on the CoreS3's
M5Canvas, not raster images, so each expression is redrawn in code rather
than blitted from these files.

| File | Expression | Used for mood |
| --- | --- | --- |
| `happy.jpg` / `happy_alt.jpg` | Open grin | `Mood::Happy` |
| `curious.jpg` | Raised brow, sparkle, squiggle mouth | `Mood::Curious` |
| `sleepy.jpg` | Closed eyes, "Zz" | `Mood::Sleepy` |
| `surprised.jpg` | Wide eyes, round open mouth | `Mood::Surprised` |
| `sad.jpg` | Downturned brows/mouth | `Mood::Grumpy` |
| `startled.jpg` | Wide shaky eyes, small o-mouth | reference only, not yet mapped |
| `playful_wink.jpg` | Wink with tongue out | reference only, not yet mapped |
| `content.jpg` | Closed-mouth smile | reference only, not yet mapped |

The five existing moods in `src/main.cpp` were restyled to match this look
(`drawEye`, `drawCheekMarks`, `drawMoodAccent`). `startled`, `playful_wink`,
and `content` aren't wired to a mood yet — they're candidates for new moods
in a future expressiveness milestone (e.g. the "Play with my kids" games and
reactions work), since adding a mood today means touching the `Mood` enum,
MQTT `parseMood`/`moodName`, and the Home Assistant `select` discovery
options in `publishDiscovery()`.

# Shared Taco face set

Firmware `1.0.0-alpha.9` embeds the professional faces from the approved ChatGPT shared collections as LCD-sized, baseline 4:2:0 JPEG assets. The firmware uses them as follows:

- happy: idle
- startled: conversation wake-up
- curious: listening
- sleepy: resting or sleeping
- thinking: processing a question
- talk closed, small, medium and wide: aligned, audio-reactive speaking animation
- excited: loud/emphatic speech
- surprised: remote surprised mood
- sad: error mood
- wink: natural idle blink

The runtime assets are in `assets/faces/`. Talking mouth selection follows the average amplitude of each incoming PCM audio chunk. The two generated exploration strips in `shared/` are design references only and are not loaded by the firmware.
