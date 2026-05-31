// Stub the image-decoder factory so ChapterHtmlSlimParser's image branch
// safe-fails (decoder == nullptr) without pulling JPEGDEC/PNGdec. Uses the REAL
// ImageDecoderFactory.h (via -I lib/Epub/Epub/converters) so signatures match;
// the static jpeg/png members are never odr-used, so they need no definition.
#include <ImageDecoderFactory.h>
ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string&) { return nullptr; }
bool ImageDecoderFactory::isFormatSupported(const std::string&) { return false; }
