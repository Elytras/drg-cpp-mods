// Lib_Math.cpp — intentionally empty.
//
// Lib_Math became a header-only template library after the SDK-coupling
// refactor (see Lib_Math.h header comment). All Math::* functions now live
// in the header as concept-constrained templates instantiated per call
// site; there are no out-of-line definitions left to compile.
//
// This file remains so the consumer .vcxproj's ClCompile entry doesn't
// dangle. It produces a near-empty .obj.

#include "Lib_Math.h"
