// Compile miniz (C) as part of this C++ test binary. miniz.h wraps its decls in
// extern "C", so linkage matches ZipFile's C++ call sites. Done via #include so
// the project's C++ std flags apply (avoids -std=gnu++2a being passed to a .c).
#include "../../lib/miniz/miniz.c"
