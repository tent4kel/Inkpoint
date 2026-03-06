# CrossPoint Reader — Inkpoint Fork

Firmware for the **Xteink X4** e-paper display reader (unaffiliated with Xteink).
Built using **PlatformIO** and targeting the **ESP32-C3** microcontroller.

CrossPoint Reader is a purpose-built firmware designed to be a drop-in, fully open-source replacement for the official
Xteink firmware. It aims to match or improve upon the standard EPUB reading experience.

> **This is [tent4kel/Inkpoint](https://github.com/tent4kel/Inkpoint)**, a personal fork of
> [crosspoint-reader/crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader).
> It adds Anki flashcard study, Instapaper integration, and experimental font customisation on top of the upstream
> feature set. See [Releases](https://github.com/tent4kel/Inkpoint/releases) for pre-built firmware.

![](./docs/images/cover.jpg)

---

## Fork Features

This is my personal fork to bring some features I wanted to the device. The code was created with heavy usage of AI / Claude. 

### Anki Flashcards

A full flashcard study system using a session-based SM-2 spaced repetition algorithm.
Cards are stored as CSV files on the SD card and can be edited on-device or managed from a computer.

- SM-2 scheduling: cards are rated after each review and rescheduled accordingly
- Session goals: Device does not have a clock so we assume doing your goal equals a day.
- Overall progress view per deck (total cards, due, review counts)
- Cards rendered with Markdown formatting (bold, italic, lists)
- Configurable font size and front/back swap per deck
- Study-ahead mode to review cards before they are technically due
- Deck browser sorted by due count, last opened, or name
- **Web card editor**: live Markdown preview, keyboard shortcuts for formatting and quick card creation
- CSV import and export — decks are plain `.csv` files; the app reads and writes them directly, making it easy to
  manage cards in any spreadsheet tool or text editor
- Support for comma and tab delimited csv/tsv. Pasting from excel works.

### Instapaper

Save and read Instapaper articles on the device. Articles are downloaded as HTML and rendered through the built-in HTML
reader with full hyphenation and justification.

- OAuth authentication (user tokens stored obfuscated on SD card)
- Syncing and caching last 30 bookmarks, option to move older downloaded articles to archive or delete automatically.
- Queuing for download, 
- Archive and delete articles from within the reader
- Language auto-detection for correct hyphenation rules
- Reading progress cached to SD card per article, caches emptied when articles are deleted.

### Web File Manager

Upload files to the device over Wi-Fi from any browser — no cables required beyond the initial flash.

- Addition: show hidden.


## Branches & Releases

| Branch | Description |
|--------|-------------|
| `personal/integration` | Stable bundle: all features above on top of upstream |
| `personal/integration-experimental` | Experimental: integration + bookerly, newsreader and chareink as fonts, XS font sizes. |

Pre-built firmware binaries are attached to each [release](https://github.com/tent4kel/Inkpoint/releases).
Stable releases follow `vX.Y.Z`; experimental releases follow `vX.Y.Z-experimental.N`.

To flash, connect via USB-C and run:

```sh
pio run --target upload
```

Or flash a downloaded `firmware.bin` via the web flasher at https://xteink.dve.al/ using the OTA fast flash controls.

---

## Motivation

E-paper devices are fantastic for reading, but most commercially available readers are closed systems with limited 
customisation. The **Xteink X4** is an affordable e-paper device; however, the official firmware remains closed.
CrossPoint exists partly as a fun side-project and partly to open up the ecosystem and truly unlock the device's
potential.

CrossPoint Reader aims to:
* Provide a **fully open-source alternative** to the official firmware.
* Offer a **document reader** capable of handling EPUB content on constrained hardware.
* Support **customisable font, layout, and display** options.
* Run purely on the **Xteink X4 hardware**.

This project is **not affiliated with Xteink**; it's built as a community project.

## Features & Usage

- [x] EPUB parsing and rendering (EPUB 2 and EPUB 3)
- [x] Image support within EPUB
- [x] Saved reading position
- [x] File explorer with file picker
  - [x] Basic EPUB picker from root directory
  - [x] Support nested folders
  - [ ] EPUB picker with cover art
- [x] Custom sleep screen
  - [x] Cover sleep screen
- [x] Wifi book upload
- [x] Wifi OTA updates
- [x] Configurable font, layout, and display options
  - [ ] User provided fonts
  - [ ] Full UTF support
- [x] Screen rotation

Multi-language support: Read EPUBs in various languages, including English, Spanish, French, German, Italian, Portuguese, Russian, Ukrainian, Polish, Swedish, Norwegian, [and more](./USER_GUIDE.md#supported-languages).

See [the user guide](./USER_GUIDE.md) for instructions on operating CrossPoint. 

For more details about the scope of the project, see the [SCOPE.md](SCOPE.md) document.

## Installing

### Web (latest firmware)

1. Connect your Xteink X4 to your computer via USB-C and wake/unlock the device
2. Go to https://xteink.dve.al/ and click "Flash CrossPoint firmware"

To revert back to the official firmware, you can flash the latest official firmware from https://xteink.dve.al/, or swap
back to the other partition using the "Swap boot partition" button here https://xteink.dve.al/debug.

### Web (specific firmware version)

1. Connect your Xteink X4 to your computer via USB-C
2. Download the `firmware.bin` file from the release of your choice via the [releases page](https://github.com/crosspoint-reader/crosspoint-reader/releases)
3. Go to https://xteink.dve.al/ and flash the firmware file using the "OTA fast flash controls" section

To revert back to the official firmware, you can flash the latest official firmware from https://xteink.dve.al/, or swap
back to the other partition using the "Swap boot partition" button here https://xteink.dve.al/debug.

### Manual

See [Development](#development) below.

## Development

### Prerequisites

* **PlatformIO Core** (`pio`) or **VS Code + PlatformIO IDE**
* Python 3.8+
* USB-C cable for flashing the ESP32-C3
* Xteink X4

### Checking out the code

CrossPoint uses PlatformIO for building and flashing the firmware. To get started, clone the repository:

```
git clone --recursive https://github.com/crosspoint-reader/crosspoint-reader

# Or, if you've already cloned without --recursive:
git submodule update --init --recursive
```

### Flashing your device

Connect your Xteink X4 to your computer via USB-C and run the following command.

```sh
pio run --target upload
```
### Debugging

After flashing the new features, it’s recommended to capture detailed logs from the serial port.

First, make sure all required Python packages are installed:

```python
python3 -m pip install pyserial colorama matplotlib
```
after that run the script:
```sh
# For Linux
# This was tested on Debian and should work on most Linux systems.
python3 scripts/debugging_monitor.py

# For macOS
python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101
```
Minor adjustments may be required for Windows.

## Internals

CrossPoint Reader is pretty aggressive about caching data down to the SD card to minimise RAM usage. The ESP32-C3 only
has ~380KB of usable RAM, so we have to be careful. A lot of the decisions made in the design of the firmware were based
on this constraint.

### Data caching

The first time chapters of a book are loaded, they are cached to the SD card. Subsequent loads are served from the 
cache. This cache directory exists at `.crosspoint` on the SD card. The structure is as follows:


```
.crosspoint/
├── epub_12471232/       # Each EPUB is cached to a subdirectory named `epub_<hash>`
│   ├── progress.bin     # Stores reading progress (chapter, page, etc.)
│   ├── cover.bmp        # Book cover image (once generated)
│   ├── book.bin         # Book metadata (title, author, spine, table of contents, etc.)
│   └── sections/        # All chapter data is stored in the sections subdirectory
│       ├── 0.bin        # Chapter data (screen count, all text layout info, etc.)
│       ├── 1.bin        #     files are named by their index in the spine
│       └── ...
│
└── epub_189013891/
```

Deleting the `.crosspoint` directory will clear the entire cache. 

Due the way it's currently implemented, the cache is not automatically cleared when a book is deleted and moving a book
file will use a new cache directory, resetting the reading progress.

For more details on the internal file structures, see the [file formats document](./docs/file-formats.md).

## Contributing

Contributions are very welcome!

If you are new to the codebase, start with the [contributing docs](./docs/contributing/README.md).

If you're looking for a way to help out, take a look at the [ideas discussion board](https://github.com/crosspoint-reader/crosspoint-reader/discussions/categories/ideas).
If there's something there you'd like to work on, leave a comment so that we can avoid duplicated effort.

Everyone here is a volunteer, so please be respectful and patient. For more details on our goverance and community 
principles, please see [GOVERNANCE.md](GOVERNANCE.md).

### To submit a contribution:

1. Fork the repo
2. Create a branch (`feature/dithering-improvement`)
3. Make changes
4. Submit a PR

---

CrossPoint Reader is **not affiliated with Xteink or any manufacturer of the X4 hardware**.

Huge shoutout to [**diy-esp32-epub-reader** by atomic14](https://github.com/atomic14/diy-esp32-epub-reader), which was a project I took a lot of inspiration from as I
was making CrossPoint.
