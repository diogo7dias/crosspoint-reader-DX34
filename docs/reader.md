# EPUB Reader

The EPUB reader is the core of the DX34 firmware. It supports EPUB 2 and EPUB 3, partial CSS, inline JPEG and PNG images, persistent reading position, and a live in-book workflow.

## Shortcuts

| Action | Result |
|--------|--------|
| Single tap OK | Open the in-book menu |
| Double tap OK | Cycle render mode (Crisp / Dark / Bionic) |
| Long press OK | Enter highlight / quote selection |
| Back | Return home (or return from a footnote) |
| Side / front page buttons | Previous / next page |
| Long-press side button | Skip chapter (if enabled) |

## In-book menu

Pressing **OK** while reading opens the menu. Items adapt to context — footnote and bookmark entries only appear when relevant; wallpaper triage only when a last sleep wallpaper exists.

| Item | What it does |
|------|--------------|
| Select Chapter | Jump to any chapter |
| Highlight Mode | Enter quote selection |
| View Quotes | Open saved quotes for this book |
| Bookmark (Add / Remove) | Toggle bookmark on the current page |
| Bookmarks | Open bookmark list |
| Footnotes | Jump to the footnote index (only for books with footnotes) |
| Rotate Screen | Switch orientation |
| Themes | Open the theme manager |
| Revert Theme | Undo live changes since the last applied theme |
| Sync | Sync reading position with a KOReader server |
| Delete Cache | Clear cached pagination and position for this book |
| Delete Book | Remove the book file |
| Remove from Recents | Remove from the recents list |
| Share QR | Show a QR code to download this book over Wi-Fi |
| **Wallpaper Triage** *(shown when a last wallpaper exists)* | |
| Favorite / Unfavorite | Mark the last wallpaper as a favorite |
| Pause / Unpause Rotation | Pause or resume wallpaper rotation |
| Move to sleep pause | Move the wallpaper out of the active folder |
| Delete Wallpaper | Delete the last wallpaper |
| Random Book on Boot | Toggle opening a random recent book at boot |

## Reading Themes

Save and switch between up to **16 named themes per book**.

1. Adjust settings live inside a book — changes apply immediately without saving.
2. Save the current configuration as a named theme.
3. Apply, rename, overwrite, or delete themes from the Themes menu.

Applying a theme reflows the current chapter and restores your reading position approximately, so you stay near the same spot after font or margin changes.

## Highlights and Quotes

Long press **OK** to enter selection mode. A cursor appears on the first word of the page.

1. Move the cursor with page buttons (word) or side buttons (line).
2. Press **OK** to set the start word.
3. Move the cursor to the end word — it can cross into the next page and cannot move before the start.
4. Press **OK** to confirm. The selection underlines for 3 seconds, then saves.
5. Press **Back** to cancel.

Quotes are stored in a `_QUOTES.txt` sidecar next to the book (e.g. `/recents/MyBook.epub` → `/recents/MyBook_QUOTES.txt`). Each entry includes the chapter title followed by the selected text. Quote files survive removing or deleting the book.

## Bookmarks

Up to **20 bookmarks per book**, managed from the in-book menu. Add a bookmark on the current page, rename, or delete from the bookmark list.

## Footnotes

Tapping a footnote link in an EPUB jumps to the note. The firmware keeps a position stack **up to 3 levels deep**, so nested references return correctly on Back.

## Text settings

Every setting below can be changed live while reading.

| Setting | Options | Default |
|---------|---------|---------|
| Font family | ChareInk, Bookerly, Vollkorn | ChareInk |
| Font size | 12, 13, 14, 15, 16, 17 pt | 16 pt |
| Line spacing | 35% to 150% | 110% |
| Paragraph alignment | Justified, Left, Center, Right, Book Style | Justified |
| First-line indent | Book, Off, Small, Medium, Large | Book |
| Word spacing | -30%, 0%, +80%, +150%, +240% | 0% |
| Extra paragraph spacing | Off, Small, Medium, Large | Off |
| Text render mode | Crisp, Dark, Bionic | Crisp |
| Reader style mode | User, Hybrid | User |
| Embedded CSS | On / Off | On |
| Hyphenation | On / Off | — |
| Bold swap | On / Off | Off |
| Debug borders | On / Off | Off |

## Margins

| Setting | Range |
|---------|-------|
| Horizontal margin | 0–55 px |
| Top margin | 0–55 px |
| Bottom margin | 0–55 px |
| Uniform margins | Toggle to link all three |

## Orientation

Four modes, settable globally or from the in-book menu: Portrait, Landscape CW, Inverted, Landscape CCW.

## Status bar

Each element can be independently shown, hidden, and positioned.

| Element | Options |
|---------|---------|
| Status bar | Show / Hide |
| Battery | Show / Hide, six positions (top/bottom × left/center/right) |
| Page counter | Show / Hide, mode (Current/Total or Left in Chapter), position |
| Book percentage | Show / Hide, position |
| Chapter percentage | Show / Hide, position |
| Book progress bar | Show / Hide, position, style (Thin / Thick / Dotted) |
| Chapter progress bar | Show / Hide, position, style |
| Chapter title | Show / Hide, position, truncation |
| Font size | Small or Medium |
| Bar thickness | Normal or Double |
| Text alignment | Right, Center, Left |
| Hide battery % | Never, In Reader, Always |
