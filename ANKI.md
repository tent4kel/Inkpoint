# Ankix4 — Anki Flashcard Mode for CrossPoint Reader

Spaced repetition flashcard review on the Xteink X4 e-paper device, built into the CrossPoint Reader firmware.

## Usage

1. Create a CSV file with `Front` and `Back` columns:

```csv
Front,Back
What is the capital of France?,Paris
**Bonjour**,Hello / *Good day*
```

Card text supports Markdown: **bold**, *italic*, `code`, headers, lists, blockquotes.
Card content is centered both vertically and horizontally on screen.

2. Upload to SD card via the web portal (File Transfer)
3. Browse files, select the `.csv` file
4. Deck summary screen shows card count and session number — press **OK** to start

## Controls

### During Card Review

| Button | FRONT state | BACK state |
|--------|-------------|------------|
| 4 front buttons | Flip card | Grade: Again / Hard / Good / Easy (left to right) |
| Upper rocker | Back to deck summary | Back to front side |
| Lower rocker (short) | Cycle font size (S/M/L/XL) | Cycle font size |
| Lower rocker (long) | Toggle portrait / landscape | Toggle portrait / landscape |

### Other Screens

| Button | Deck Summary | Session Complete |
|--------|-------------|-----------------|
| OK / front buttons | Start review | Exit |
| Upper rocker | Exit to file browser | Exit to file browser |

Font size and orientation are saved independently from the reader settings in `/.ankix/anki_settings.bin`. Changing font size in Anki does not affect the e-book reader and vice versa.

## Session-Based SM-2

The device has no reliable real-time clock, so the SM-2 algorithm uses **sessions** instead of days. Each time you open a deck and press Start, the session counter increments. Intervals like "review in 6 sessions" mean "review after opening this deck 6 more times."

If you review daily, sessions approximate days. Review twice a day and progression is faster.

### Grading

| Button | Grade | Effect |
|--------|-------|--------|
| Left   | Again | Reset to 0 reps, re-queue in current session, EF -0.20 |
| Mid-L  | Hard  | Interval x0.7, EF -0.05 |
| Mid-R  | Good  | Normal interval growth, EF unchanged |
| Right  | Easy  | Interval x1.3, EF +0.10 |

Easiness factor minimum: 1.3. New cards start at EF 2.5.

## CSV Format

On first load, the CSV is extended with scheduling columns:

```csv
Front,Back,Repetitions,EasinessFactor,Interval,NextReviewSession
What is the capital of France?,Paris,1,2500,1,2
**Bonjour**,Hello / *Good day*,0,2500,0,0
```

- `EasinessFactor`: EF x1000 (2500 = 2.5)
- `Interval`: sessions until next review
- `NextReviewSession`: absolute session number when card is due
- Fields with commas or quotes are RFC 4180 quoted

The CSV is saved after every grade for crash safety (temp file + rename).

Session counter is stored separately in `/.ankix/<hash>.session` on the SD card.

## Files Added

```
src/anki/
  SM2.h / SM2.cpp            — Session-based SM-2 algorithm
  CsvParser.h / CsvParser.cpp — RFC 4180 CSV read/write
  AnkiDeck.h / AnkiDeck.cpp  — Deck loading, card scheduling, session management

src/activities/anki/
  AnkiActivity.h / AnkiActivity.cpp — Flashcard review UI activity
```

## Files Modified

- `src/main.cpp` — Added `onGoToAnki()`, routes `.csv` files to AnkiActivity
- `src/activities/home/MyLibraryActivity.cpp` — Added `.csv` to file extension filter

## Architecture

AnkiActivity follows the same patterns as TxtReaderActivity / MdReaderActivity:

- FreeRTOS display task with semaphore-protected rendering
- Orientation support (Portrait / Landscape CCW), toggled via lower rocker long press
- Independent font size (S/M/L/XL), cycled via lower rocker short press
- Anti-aliasing pass for grayscale font rendering
- Card text rendered through MarkdownParser with center alignment
- Vertical centering when card content fits on a single page
- Button hints via `GUI.drawButtonHints()` and `GUI.drawSideButtonHints()`

Card content is written to a temp file (`/.ankix/_card.md`) and parsed through the existing MarkdownParser to produce rendered Page objects. This reuses 100% of the typography pipeline (word wrap, bold/italic, lists, etc.) with zero new rendering code.

## Data on SD Card

```
/.ankix/
  anki_settings.bin       — Font size + orientation (independent of reader)
  <hash>.session          — Per-deck session counter
  _card.md                — Temp file for MD rendering (cleaned up on exit)
  md_<hash>/              — MarkdownParser cache directory
```
