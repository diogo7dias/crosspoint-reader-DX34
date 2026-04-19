# CAVEMAN GUIDE TO CROSSPOINT-MOD-DX34

Ugg. You here. Good. Me tell you about rock-box-that-show-words.

## WHAT THIS THING

Small flat rock. Screen like cave wall. Hold book inside. No paper. No tree die. Good.

This rock-box call **DX34**. It read book. That all. No game. No noise-box. No shiny-web. Just book.

Firmware name **CrossPoint-Mod-DX34**. Fork of big CrossPoint tribe. We make own tribe. Own rules. Own fire.

## WHAT ROCK DO

- Show book words on gray wall
- Turn page with stick-button
- Remember where you stop reading
- Keep book safe on tiny square (SD card)
- Talk to other rock over air-spirit (Wi-Fi)
- Sleep when you no look
- Show pretty picture when sleep

## BOOK ROCK EAT

| Food | Good? |
|------|-------|
| EPUB | YES. Best food. All magic work here. |
| TXT / MD | OK. Just plain word. |
| XTC / XTCH | OK. Pre-chew pages. |

No PDF. No sound-book. No moving-picture. Rock say no.

## INSIDE BOOK — PRESS OK STICK

Push OK stick. Menu come. You do things:

- Jump to chapter
- Save good word (quote)
- Change how word look
- Make theme (save look for later)
- Delete book
- Sleep picture stuff

**Hold OK stick long** (one sun-count) = pick words to save. Point start. Point end. Rock save in file next to book. `_QUOTES.txt`. Good for remember smart thing.

**Tap OK two time fast** = change look-mode:
- **Crisp** = clean
- **Dark** = dark
- **Bionic** = first letter thick (brain read fast)

## WORD-SHAPES (FONTS)

Four tribe of letters:

- **ChareInk** — our tribe. Made for gray-wall.
- **Bookerly** — big river tribe (Amazon) letter.
- **Vollkorn** — free tribe letter.
- **IM Fell DW Pica** — old-old-old letter. Look like grandfather grandfather book.

Sizes: 12, 14, 16, 17. Default 16. IM Fell only 15. Because IM Fell special.

## SLEEP WALL

When you no touch rock, rock sleep. Show picture. Many mode:

- **Dark** — black
- **Light** — white
- **Custom** — picture from `/sleep` pile
- **Cover** — show last book cover
- **None** — stay like was
- **Cover + Custom** — cover if have, else picture

Put BMP picture in `/sleep` folder. Rock pick random. Mark good one favorite (add `_F` to name). Favorite safe. No get eat by cleanup.

Cap at 200 picture. More than 200? Extra go to `/sleep pause` pile. Rock brain small (ESP32-C3). Can not hold too much.

## HOME CAVE (HOME SCREEN)

Show you:
- Books you read lately (up to 100)
- Door to library
- Door to file-send (Wi-Fi)
- Door to settings
- Maybe OPDS door (if you set up far-away-book-server)

Two style: **Classic** (list) or **Single Cover** (big picture).

## BUTTONS DO WHAT

- **Side button** = turn page
- **Long press side** = skip chapter (if you turn on)
- **OK** = menu
- **OK long** = pick word
- **OK tap tap** = change look
- **Back** = go home
- **Power short** = you choose (sleep, page, refresh, nothing)

You can REMAP front buttons. Rock ask you press button for each job. Good for broken-button tribe.

## AIR-SPIRIT (WI-FI)

- **File-transfer mode** — open little web-place on phone or big-rock browser. Upload book. Move book. Rename book.
- **Calibre** — talk to Calibre spirit on big-rock.
- **OPDS** — grab book from sky-library.
- **KOReader sync** — tell other rock where you stop reading.
- **OTA** — rock heal itself. Get new firmware from GitHub sky.

## SECRET FOLDER

Rock make `/.crosspoint/` on SD card. Inside:

- `settings.json` — your choices
- `state.json` — what rock remember
- `books/` — per-book memory (position, pages, themes)

Rock fingerprint book by content. You move file, rename file, rock still know. Rock smart.

## BUILD ROCK FROM STONES

Need:
- PlatformIO magic (`pio`)
- Python 3.8+ snake
- USB-C vine
- DX34 rock

Commands:

```sh
git clone --recursive https://github.com/diogo7dias/crosspoint-reader-DX34
cd crosspoint-reader-DX34
pio run           # shape stone
pio run -t upload # put in rock
pio device monitor # listen to rock talk
```

Test on your big-rock (no need small-rock):

```sh
pio test -e test_host
```

## LIMIT OF ROCK

- EPUB is chief food. Others OK but simpler.
- English first. Other tongue later.
- Small brain (~182 KB free). Can not hold forest of pictures.

## TRIBE RULES

Rock only for read. No game. No noise. No browse web. Reader tribe focus.

Want add thing? Ask in Discussions FIRST. Then code. Else rock chief say no.

## THANK TRIBE

- **@daveallie** and CrossPoint tribe — they start fire
- **atomic14** — also start fire
- **Xteink** tribe — make rock body (we not same tribe, we just use)

Now go. Read book. Be quiet. Enjoy gray wall.

Ugg.
