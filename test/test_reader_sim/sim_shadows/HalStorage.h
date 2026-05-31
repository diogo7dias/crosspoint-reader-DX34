// Shadow HalStorage.h for the host reader-sim.
//
// The layout-only slice never touches the filesystem. blocks/TextBlock.h
// includes <HalStorage.h> and references FsFile in its serialize/deserialize
// declarations (which we do NOT compile in this slice), so an incomplete type
// is all that's needed for those declarations to parse.
#pragma once

class FsFile;
