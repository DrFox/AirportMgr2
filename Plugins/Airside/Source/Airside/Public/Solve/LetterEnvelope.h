#pragma once

#include "CoreMinimal.h"
#include "Solve/IcaoCode.h"

/**
 * The FLEET half of what an ICAO code letter used to keep in one row: how far a letter's
 * longest admitted airframe reaches aft of the nose-gear stop mark (MaxTailAft) and forward of
 * it (MaxNoseFwd).
 *
 * SPLIT OUT OF IcaoCode::Rows BY #292. The row mixed Annex-14 nominals (MaxWingspan,
 * RunwayWidth, StandTurnRadius, WingtipClearance, StandDepth, AftEdgeAllowance - see
 * IcaoCode.h's own header) with these two figures, which move every time a longer-tailed or
 * longer-nosed UAircraftType is added - and until this split, "move" meant a human retyping
 * MAX_TAIL_AFT_<letter> in a build_plane<N>_type.py AND the matching row in IcaoCode.cpp, by
 * hand, seven times over (issue #292's own evidence). UAirsideSettings::ResolveLetterEnvelope
 * computes this by scanning the loaded fleet instead, so adding a type edits no C++.
 *
 * ONLY THESE TWO FIELDS, NOT WingFwd/WingAft TOO, though the issue that created this file asked
 * for all four. WingFwd/WingAft are the wing keep-out's band edges, and FEntityFootprint - the
 * one struct any UAircraftType states its geometry through - carries WingX, A SINGLE SPANWISE
 * LINE, not a leading/trailing edge (see RoadEntity.h's own comment: "the model carries no wing
 * planform... there is nothing to derive a leading edge from"). There is no per-type figure to
 * MAX over, so nothing raises them - they stay exactly what they were, IcaoCode::WingFwdForLetter
 * / WingAftForLetter's authored return, unaffected by this split. Recorded here rather than
 * silently narrowed, per CLAUDE.md's rule for an issue item that turns out to be a false premise.
 *
 * PLAIN STRUCT, DEPENDENCY-FREE LIKE THE REST OF Solve/: CoreMinimal.h and Solve/ only, no
 * USTRUCT - nothing here needs UHT, the same reason IcaoCode.h's own EIcaoCode is a plain enum.
 */
struct FLetterEnvelope
{
	/**
	 * How far aft of the nose-gear stop mark the longest airframe this letter admits
	 * reaches, uu. Positive - see IcaoCode::MaxTailAftForLetter's retired comment (moved to
	 * IcaoCode::FloorEnvelopeForLetter, this figure's floor) for the measured history.
	 */
	double MaxTailAft = 0.0;

	/** The greatest nose overhang this letter admits, uu forward of the stop mark. Positive. */
	double MaxNoseFwd = 0.0;

	/** Exact, not epsilon - both fields are either read straight off IcaoCode::
	 *  FloorEnvelopeForLetter or MAX'd against a footprint figure, never accumulated in a way
	 *  that could drift by rounding. Exists for FBuildSessionTunables::operator==, which
	 *  FBuildSession's frame-context cache (#303) needs field-by-field. */
	bool operator==(const FLetterEnvelope& Other) const
	{
		return MaxTailAft == Other.MaxTailAft && MaxNoseFwd == Other.MaxNoseFwd;
	}
};

/**
 * All six letters' envelopes, indexed by EIcaoCode - what
 * UAirsideSettings::ResolveLetterEnvelopeTable resolves ONCE and a caller that touches more
 * than one letter in a single pass threads down, rather than re-resolving per letter.
 *
 * WHY A TABLE EXISTS AT ALL BESIDE UAirsideSettings::ResolveLetterEnvelope(EIcaoCode): Build/
 * and Tool/ may not include Content/ (Check-Architecture's include-direction rule) but often
 * need MORE THAN ONE letter's envelope in one call - FStandMarkingBuilder::Build paints every
 * stand on the level, whatever letters they are, in one pass. Present/ resolves the table once
 * per rebuild and hands it down as plain data, so the include-direction rule costs nothing.
 */
struct AIRSIDE_API FLetterEnvelopeTable
{
	FLetterEnvelope Envelopes[6];

	const FLetterEnvelope& operator[](EIcaoCode Code) const
	{
		return Envelopes[static_cast<uint8>(Code)];
	}

	/** Field by field, FBuildSessionTunables::operator==' own reason. */
	bool operator==(const FLetterEnvelopeTable& Other) const
	{
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Envelopes); ++Index)
		{
			if (!(Envelopes[Index] == Other.Envelopes[Index]))
			{
				return false;
			}
		}
		return true;
	}

	/**
	 * Every letter at its AUTHORED FLOOR, nothing raised - what a caller that cannot reach
	 * Content/ at all (Model/'s legacy-stand migration, Tool/'s interactive preview) builds
	 * for itself, and what UAirsideSettings::ResolveLetterEnvelopeTable starts from before
	 * raising entries against the loaded fleet. Free of any asset load - safe to call every
	 * frame or every legacy stand on load, unlike the Content-backed resolve.
	 */
	static FLetterEnvelopeTable Floor();
};
