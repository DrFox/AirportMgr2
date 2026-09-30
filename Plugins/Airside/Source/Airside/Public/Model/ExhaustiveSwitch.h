#pragma once

// A SWITCH ON AN ENUM WITH NO default: IS ONLY A CHECK IF THE COMPILER SAYS SO, and on this toolchain it does not:
// UE leaves MSVC's C4061/C4062 ("enumerator not handled in switch") off - WindowsPlatformCompilerSetup.h lists both
// among its skipped warnings, and UBT's SwitchUnhandledEnumeratorWarningLevel (CppCompileWarnings.cs) defaults to
// Off. So "no default, so a new enumerator is a compiler warning here" was a comment, not a contract, at four sites
// (#436 review, 2026-09-30).
//
// Wrap the FUNCTION holding such a switch in AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN / _END and a missing case is a BUILD
// ERROR there - checked by adding a stray enumerator to each enum and watching the build fail at each site. The pair
// is scoped (push/pop) so it touches nothing else in the translation unit; the level is set as well as the error
// state, so an off-by-default warning is switched ON at this site and not merely escalated if something else enabled it.
// Other compilers: nothing - this project builds Win64 with MSVC.
//
// WHY A MACRO PAIR and not a module-wide flag: most of this codebase's switches have a deliberate default (a clamp,
// a fallback row), and C4061 would flag every one of them; only a switch whose point is to be total opts in.
#if defined(_MSC_VER) && !defined(__clang__)
	#define AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN __pragma(warning(push)) __pragma(warning(1: 4062)) __pragma(warning(error: 4062))
	#define AIRSIDE_EXHAUSTIVE_SWITCH_END __pragma(warning(pop))
#else
	#define AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
	#define AIRSIDE_EXHAUSTIVE_SWITCH_END
#endif
