# Fonts

The DX34 firmware ships three reader font families, each at six sizes with regular, bold, and italic styles.

## Families and sizes

| Family | Style | Sizes (pt) | Notes |
|--------|-------|-----------|-------|
| **ChareInk** | Serif, e-ink optimized | 12, 13, 14, 15, 16, 17 | DX34 default |
| **Bookerly** | Serif | 12, 13, 14, 15, 16, 17 | Amazon's reading font |
| **Vollkorn** | Serif | 12, 13, 14, 15, 16, 17 | Open-source book font |

Defaults: **ChareInk 16 pt**, 110% line spacing.

## Size switching

All three families share the same size set, so switching family preserves size. Line spacing ranges from 35% to 150% regardless of family.

Settings saved under older firmware are migrated automatically — legacy sizes collapse into the nearest current size (e.g. legacy 10 → 12, legacy 18/19 → 17).

## Unicode fallback

Unifont is bundled as a Unicode fallback for glyphs outside the main families. It is not selectable as a reader font.

## UI fonts

Menus, popups, and the status bar use separate built-in bitmap fonts. They are not affected by reader font settings.
