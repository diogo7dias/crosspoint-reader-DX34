#pragma once

#include <string>
#include <vector>

namespace LibraryListingFilter {

// Removes EPUB-render cache files from a flat directory listing.
//
// Rules:
//  - Always drop *_q.pxc.
//  - Drop <base>.pxc when a sibling file with the same stem and a source
//    image extension (.jpg/.jpeg/.png/.gif/.webp) is present in the listing.
//  - .bmp is intentionally NOT in the source-extension list: a .bmp and a
//    .pxc with the same basename in /sleep are two independent user
//    wallpapers and must both survive.
//
// The function operates on bare filenames (no directory prefix). Directory
// entries (names ending with '/') are ignored and left untouched.
void filterEpubCachePxc(std::vector<std::string>& files);

}  // namespace LibraryListingFilter
