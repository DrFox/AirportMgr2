#include "Solve/IcaoCode.h"

#include "Solve/LetterEnvelope.h"

namespace IcaoCode
{
	namespace
	{
		/** One row of ICAO Annex 14 Table 1-1: everything the letter sets, uu. */
		struct FRow
		{
			const TCHAR* Letter;
			double MaxWingspan;
			double RunwayWidth;

			/**
			 * ICAO Annex 14's MINIMUM taxiway width for the letter, uu - 7.5/10.5/15/18/23/25 m.
			 * The ICAO figure, not the game's even-metre rounding (build_road_profiles.py
			 * TAXIWAY_WIDTHS rounds UP from these), for the reason RunwayWidth keeps ICAO's:
			 * the rounded widths are read back through TaxiwayLetterForWidth, and a minimum is
			 * what a wider pavement still meets.
			 */
			double TaxiwayWidth;
			double StandTurnRadius;

			/**
			 * Wingtip clearance on a stand, uu - the gap ICAO wants between a parked
			 * aeroplane's wingtip and anything beside it.
			 *
			 * THE STAND'S WIDTH IS NOT A COLUMN, because it is this plus the span band twice
			 * over and a stored width would be a third figure that has to agree with two
			 * others. See StandWidthForLetter, and this file's header for the three call
			 * sites that once typed the same table separately.
			 */
			double WingtipClearance;

			/**
			 * How deep a stand of this letter is, uu: tail setback + aircraft + service
			 * ground ahead of the nose, entrance edge to the back of the GSE road.
			 *
			 * AUTHORED, not derived, and it is the only figure here that is. Width follows
			 * from span and clearance; depth follows from aircraft LENGTH and the room an
			 * equipment area and a service road need, and no clean rule produces it. Standard
			 * aerodrome design values, as the header says of the rest - the first thing to
			 * check if a real layout looks wrong.
			 *
			 * RAISED PAST THOSE VALUES ON 2026-09-26, A-E (user ruling; F unchanged): A 2000 ->
			 * 3200 (then 3400 with the A/B widening, same day), B 3000 -> 3950, C 5500 -> 6500 (the published 55 m is now 65 m), D 7000 ->
			 * 8400, E 9000 -> 9500. Service vehicles now enter only by the far edge, so the
			 * service lane lies in the slack AHEAD of the nose, and on the published depths that
			 * slack was too short for a bay's serve leg to reach its lane forward of the service
			 * point. The depths at which UEntityDefinition::BuildStandTemplate's contact band
			 * first opens were measured - A 2900, B 3650, C 6181, D 8093, E 9195 - and each floor
			 * is that plus about 300 of margin. Widths are unchanged.
			 * ENFORCED BY: Airside.Entities.StandLayoutEveryLetterBuilds
			 */
			double StandDepth;

			/**
			 * The fore-aft extent of every wing this letter admits, uu about the stop mark -
			 * forward-most leading edge and aft-most trailing edge. Both negative.
			 *
			 * Code C is measured against the two types this project ships: an A320 root
			 * leading edge at about -990 and a 737-800's at -1030, with the wing-body fairing
			 * running back to about -2080, so the band covers both with a little margin. The
			 * rest are authored design values, as the rest of this table is. The test pins
			 * every builder's WingX inside its own letter's band.
			 */
			double WingFwd;
			double WingAft;

			/**
			 * Extra width, uu, for the road contacts along a stand's aft edge.
			 *
			 * A STAND IS ENTERED FROM BEHIND AND EVERY BAY HAS ITS OWN WAY IN, so a Code C
			 * stand puts six contacts on its back edge - four bay entries and one exit per
			 * side. Each splits the GSE road where it joins it, and the fillet either side
			 * wants its own run along that road, so neighbours cannot be closer than twice
			 * that. The aeroplane and its two lanes do not pay for any of it.
			 *
			 * 600 EVERYWHERE, and that is right rather than lazy: the figure is set by the
			 * service VEHICLE's turning radius and by how many services a stand has, neither
			 * of which is a property of the code letter. It cannot be derived here because
			 * the vehicle lives in Content/ and Solve/ may see only CoreMinimal - so it is
			 * authored, and Airside.Entities.StandLayoutFitsItsLettersFloor measures what the
			 * layout actually reaches against the width this produces.
			 */
			double AftEdgeAllowance;

			/**
			 * The width a stand of this letter needs for its DESIGN VEHICLE's lane, uu, where
			 * that is more than the span derivation gives - zero, and so no floor, where it is
			 * not. See WidthOf, which takes the larger.
			 *
			 * A AND B ONLY, 5000 each (user ruling 2026-09-26). Their design vehicle is the
			 * utility tow (UAirsideSettings::ResolveStandDesignVehicle), and a tow needs a
			 * straight into each service point long enough for its trailer to settle before it
			 * reverses, which pushes its lane outboard of where the span puts it:
			 * UEntityDefinition::BuildStandTemplate measured both at 4757 wide, and 5000 is that
			 * plus about 250 of margin. A's width is set by the tow lane now, not the wingspan
			 * (derived 3500), and B's likewise (derived 4400) - so A and B share a width floor and
			 * a stand's DEPTH is what tells them apart.
			 * ENFORCED BY: Airside.Entities.StandLayoutEveryLetterBuilds
			 */
			double TowLaneWidth = 0.0;
		};

		// D and E deliberately share RunwayWidth (45 m serves both) - see MaxWingspanForWidth.
		//
		// DESIGNATED INITIALISERS SINCE 2026-09-25 (#292) - eleven positional doubles read
		// back as which column only by counting commas against the struct above, and that is
		// exactly how a column got typed into the wrong slot unnoticed. A column is now named
		// at its own value, so a misplaced figure is a compile error (wrong member) rather
		// than a row that silently sizes a stand from the wrong figure.
		//
		// NINE COLUMNS, NOT ELEVEN, SINCE #292's SECOND STEP: MaxTailAft and MaxNoseFwd left this
		// row for FLetterEnvelope - see Solve/LetterEnvelope.h and EnvelopeFloors below, which
		// carries their measured history now that it is theirs rather than this table's.
		static const FRow Rows[] = {
			// NO STANDS OF ITS OWN since 2026-09-27 - an A aircraft parks on B's (StandLetterFor).
			// Its size columns are omitted, not zeroed by hand, so HasStands reads the table
			// rather than a flag.
			{ .Letter = TEXT("A"), .MaxWingspan = 1500.0, .RunwayWidth = 1800.0, .TaxiwayWidth = 750.0, .StandTurnRadius = 1500.0,
			  .WingtipClearance = 300.0,
			  .WingFwd = -50.0, .WingAft = -700.0 },
			{ .Letter = TEXT("B"), .MaxWingspan = 2400.0, .RunwayWidth = 2300.0, .TaxiwayWidth = 1050.0, .StandTurnRadius = 2000.0,
			  .WingtipClearance = 300.0, .StandDepth = 3950.0,
			  .WingFwd = -300.0, .WingAft = -1400.0, .AftEdgeAllowance = 600.0, .TowLaneWidth = 5000.0 },
			{ .Letter = TEXT("C"), .MaxWingspan = 3600.0, .RunwayWidth = 3000.0, .TaxiwayWidth = 1500.0, .StandTurnRadius = 2500.0,
			  .WingtipClearance = 450.0, .StandDepth = 6500.0,
			  .WingFwd = -950.0, .WingAft = -2150.0, .AftEdgeAllowance = 600.0 },
			{ .Letter = TEXT("D"), .MaxWingspan = 5200.0, .RunwayWidth = 4500.0, .TaxiwayWidth = 1800.0, .StandTurnRadius = 4000.0,
			  .WingtipClearance = 750.0, .StandDepth = 8400.0,
			  .WingFwd = -1300.0, .WingAft = -3000.0, .AftEdgeAllowance = 600.0 },
			{ .Letter = TEXT("E"), .MaxWingspan = 6500.0, .RunwayWidth = 4500.0, .TaxiwayWidth = 2300.0, .StandTurnRadius = 5000.0,
			  .WingtipClearance = 750.0, .StandDepth = 9500.0,
			  .WingFwd = -1600.0, .WingAft = -3700.0, .AftEdgeAllowance = 600.0 },
			{ .Letter = TEXT("F"), .MaxWingspan = 8000.0, .RunwayWidth = 6000.0, .TaxiwayWidth = 2500.0, .StandTurnRadius = 6000.0,
			  .WingtipClearance = 750.0, .StandDepth = 10000.0,
			  .WingFwd = -1900.0, .WingAft = -4300.0, .AftEdgeAllowance = 600.0 },
		};

		/**
		 * MaxTailAft and MaxNoseFwd's AUTHORED FLOOR per letter, uu - split from FRow above by
		 * #292 because these two, unlike the rest of the row, are FLEET maxima:
		 * UAirsideSettings::ResolveLetterEnvelope raises them past this floor when a loaded
		 * UAircraftType of the letter reaches further, so the floor is what a letter with
		 * nothing modelled - or nothing loaded, as every automation test and
		 * IcaoCode::FloorEnvelopeForLetter's own Model/Tool callers are - keeps.
		 *
		 * THE FIGURES THEMSELVES ARE UNCHANGED BY THE SPLIT - copied here from FRow's old
		 * MaxTailAft/MaxNoseFwd columns verbatim, with their history:
		 *
		 * CODE C AND CODE E WERE MEASURED before this floor existed, and both still are - C is
		 * the 737-800's tail at 3538, and IcaoCodeTest pins it against Build737's own figure so
		 * the two cannot drift. It was 3430 until 2026-09-19, when Build737 was found to be
		 * carrying the A320's nose overhang; correcting the nose moved the tail with it, because
		 * the tail is the nose less the published overall length. Code E was 6700 until
		 * 2026-09-21: DA_Aircraft_Plane6, the 777-300ER, measures 6799.3 uu from its nose-gear
		 * stop mark to its tailcone, so at 6700 a Code E stand would have laid its GSE road and
		 * its aft edge a metre INSIDE the aeroplane parked on it. Raised to 6800 - the
		 * measurement plus a centimetre of rounding, not a round number chosen to be safe.
		 *
		 * MaxNoseFwd's C WAS 509 SINCE 2026-09-25, UP FROM 507, FOR plane9. The paper A320
		 * (BuildA320) types the brochure's 5.07 m; DA_Aircraft_Plane9, traced off Airbus's own
		 * 3-view, measures 508.4 uu - 1.4 cm longer, which is the drawing's and left as it is,
		 * the same "built to the letter" call plane6 forced on Code E. Both still fit.
		 * ENFORCED BY: Airside.Content.MeasuredTypesFitTheirLettersRow
		 *
		 * CODE D HAS A TYPE TO MEASURE IT AGAINST SINCE 2026-09-25, and no figure moved.
		 * DA_Aircraft_Plane13, the 757-300, measures 4878 uu aft of its stop mark and 589 uu
		 * forward - inside by 622 and 111. Loose, unlike C and E: the row was authored for
		 * aeroplanes up to 52 m and nothing that large is modelled.
		 * ENFORCED BY: Airside.Content.MeasuredTypesFitTheirLettersRow
		 *
		 * MaxTailAft's E WAS 6902 SINCE 2026-09-25, UP FROM 6800, FOR plane11: DA_Aircraft_Plane11,
		 * the A350-1000, has its rudder overhang 6901.3 uu aft of the stop mark, a metre past the
		 * 777's tailcone - the same measurement-plus-a-centimetre. That put E past F's AUTHORED
		 * 6900 (the A380 measures 6775), and the column's ORDER must hold (now pinned by
		 * Airside.Solve.IcaoCode.EnvelopeFloorOrderedByLetter - see #292), so F went to 7000 with
		 * it: an authored value revised because a type exceeded the letter below it, not because
		 * an A380 grew. ENFORCED BY: Airside.Content.MeasuredTypesFitTheirLettersRow
		 *
		 * CODE F HAS A TYPE TO MEASURE IT AGAINST SINCE 2026-09-23, and no figure moved on its
		 * own account. DA_Aircraft_Plane8, the A380-800, measures 6775 uu from its nose-gear stop
		 * mark to its tailcone - 225 uu inside MaxTailAft's 7000 (originally 125 uu inside 6900,
		 * before E's 2026-09-25 rise above forced F up with it).
		 * ENFORCED BY: Airside.Content.MeasuredTypesFitTheirLettersRow
		 *
		 * IT WAS NOT A TEST THAT FOUND PLANE6'S GAP, AND THAT IS WORTH KNOWING ABOUT.
		 * Airside.Entities.EveryAirframeFitsItsLettersRow builds its cases from the C++
		 * BUILDERS - A320, Build737, the Piper - and says so; a type that exists only as a
		 * DA_Aircraft_* asset is invisible to it, and plane6 is such a type. The asset side is
		 * covered by Airside.Content.MeasuredTypesFitTheirLettersRow instead, which loads the DAs
		 * and asserts this same floor (through UAirsideSettings::ResolveLetterEnvelope, since
		 * #292 - it is guaranteed >= every asset's measurement by construction now, but the test
		 * still catches the AssetRegistry scan failing to reach one, or Parse disagreeing with a
		 * mistyped Code). Two tests because there are two kinds of type, not because one of them
		 * is redundant.
		 */
		struct FEnvelopeFloorRow
		{
			double MaxTailAft;
			double MaxNoseFwd;
		};

		static const FEnvelopeFloorRow EnvelopeFloors[] = {
			/* A */ { 1000.0, 300.0 },
			/* B */ { 2000.0, 400.0 },
			/* C */ { 3538.0, 509.0 },
			/* D */ { 5500.0, 700.0 },
			/* E */ { 6902.0, 800.0 },
			/* F */ { 7000.0, 900.0 },
		};

		/**
		 * The NARROWEST stand a row admits, uu. The ONE place the derivation is written.
		 *
		 * THE LARGER OF THE SPAN DERIVATION AND THE ROW'S TowLaneWidth, since 2026-09-26: the
		 * aeroplane decides it for C-F, the utility tow's lane for B - see that column. NOT A,
		 * since the 2026-09-27 merge: row A carries no TowLaneWidth of its own any more (nor is
		 * it ever asked to - StandWidthForLetter reads RowFor(StandLetterFor(Code)), so Code A's
		 * question lands on this same row through the alias instead).
		 *
		 * The lane is in it twice because there is one down each side: a vehicle cannot cross
		 * under the aeroplane, so each side of the stand is reached and left on its own lane,
		 * and a stand with room for only one of them has a serviceable side and a dead one.
		 */
		static double WidthOf(const FRow& Row)
		{
			return FMath::Max(Row.TowLaneWidth,
				Row.MaxWingspan + 2.0 * (Row.WingtipClearance + IcaoCode::ServiceLaneWidth())
					+ Row.AftEdgeAllowance);
		}

		/**
		 * The row for Code, by INDEX - Rows is declared A, B, C, D, E, F in that order, the
		 * same order EIcaoCode declares its values, so the enum's ordinal IS the row index.
		 *
		 * NO NULL CASE, and that is the whole point of the enum: FindRow(const FString&) used
		 * to return nullptr for anything that did not match A-F, and every caller here turned
		 * that into a silent fallback to CodeC(). An EIcaoCode cannot name a letter outside
		 * A-F, so there is nothing left to fall back from - Parse is where an unrecognised
		 * STRING is refused now, once, rather than here on every lookup.
		 */
		static const FRow& RowFor(EIcaoCode Code)
		{
			const int32 Index = static_cast<int32>(Code);
			check(Index >= 0 && Index < UE_ARRAY_COUNT(Rows));
			return Rows[Index];
		}

		/** A row sizes stands only if it has a stand depth - see row A. */
		static bool HasStands(const FRow& Row) { return Row.StandDepth > 0.0; }
	}

	TOptional<EIcaoCode> Parse(const FString& Letter)
	{
		// TRIMMED THEN UPPERCASED, IN THAT ORDER, ONCE - see the header for why both matter
		// and why anything left over that is not exactly one of A-F is nullopt rather than a
		// guess. This is the only place in the file that still compares a string.
		const FString Trimmed = Letter.TrimStartAndEnd().ToUpper();
		for (const FRow& Row : Rows)
		{
			if (Trimmed == Row.Letter)
			{
				return static_cast<EIcaoCode>(&Row - &Rows[0]);
			}
		}
		return TOptional<EIcaoCode>();
	}

	const TCHAR* ToLetter(EIcaoCode Code)
	{
		return RowFor(Code).Letter;
	}

	FString StandNameForLetter(EIcaoCode Code)
	{
		return FString::Printf(TEXT("Code %s stand"), ToLetter(Code));
	}

	EIcaoCode CodeForWingspan(double WingspanUu)
	{
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Rows); ++Index)
		{
			if (WingspanUu < Rows[Index].MaxWingspan)
			{
				return static_cast<EIcaoCode>(Index);
			}
		}
		// Wider than every row: still F, the widest letter the table has.
		return static_cast<EIcaoCode>(UE_ARRAY_COUNT(Rows) - 1);
	}

	FString LetterForWingspan(double WingspanUu)
	{
		return ToLetter(CodeForWingspan(WingspanUu));
	}

	double MaxWingspanForLetter(EIcaoCode Code)
	{
		return RowFor(Code).MaxWingspan;
	}

	double DesignSpanForLetter(EIcaoCode Code)
	{
		// F HAS NOTHING ABOVE IT TO ROLL INTO - see MaxWingspanForLetter's own comment and
		// Airside.Solve.MaxWingspanForLetterMatchesTheBandEdges: LetterForWingspan reads "F"
		// both a hair under F's own ceiling and exactly AT it, so the ceiling itself is
		// already the widest span this table still calls F.
		if (Code == EIcaoCode::F)
		{
			return MaxWingspanForLetter(Code);
		}

		// EVERY OTHER LETTER: one uu under its own ceiling, which LetterForWingspan's
		// strict-less-than row test has already handed to the NEXT letter at the ceiling
		// itself. The 1.0 has no meaning beyond "not the ceiling" - any value in the open
		// interval below it would do, and this is the smallest step the table's uu unit has.
		return MaxWingspanForLetter(Code) - 1.0;
	}

	double MaxWingspanForWidth(double TotalWidth)
	{
		const FRow* Nearest = &Rows[0];
		for (const FRow& Row : Rows)
		{
			const double Dist = FMath::Abs(Row.RunwayWidth - TotalWidth);
			const double BestDist = FMath::Abs(Nearest->RunwayWidth - TotalWidth);

			// Strictly nearer wins outright. A TIE only breaks toward the later row when
			// both rows share the same RunwayWidth (D and E, both 45 m) - there the wider
			// letter is the figure that width was actually chosen for. A tie between rows
			// of DIFFERENT widths (an odd width exactly between two codes) keeps the
			// earlier, narrower one, as it always has.
			if (Dist < BestDist || (Dist == BestDist && Row.RunwayWidth == Nearest->RunwayWidth))
			{
				Nearest = &Row;
			}
		}
		return Nearest->MaxWingspan;
	}

	double TaxiwayWidthForLetter(EIcaoCode Code)
	{
		return RowFor(Code).TaxiwayWidth;
	}

	EIcaoCode TaxiwayLetterForWidth(double PavementWidthUu)
	{
		// Rows run A to F - RowFor indexes them by the enum, so they must - and the last row whose
		// minimum is met is the largest. Half a uu of slack so a width typed as exactly a
		// minimum, then carried through a sum of band widths, still meets it.
		EIcaoCode Best = EIcaoCode::A;
		for (const FRow& Row : Rows)
		{
			if (Row.TaxiwayWidth <= PavementWidthUu + 0.5)
			{
				Best = Parse(Row.Letter).GetValue();
			}
		}
		return Best;
	}

	double TaxiwayStripFor(EIcaoCode Letter, double PavementWidthUu)
	{
		const double Overhang = FMath::Max(0.0, 0.5 * MaxWingspanForLetter(Letter) - 0.5 * PavementWidthUu);
		return Overhang + WingtipClearanceForLetter(Letter);
	}

	double TaxiwayStripForWidth(double PavementWidthUu)
	{
		return TaxiwayStripFor(TaxiwayLetterForWidth(PavementWidthUu), PavementWidthUu);
	}

	double RadiusForLetter(EIcaoCode Code)
	{
		return RowFor(Code).StandTurnRadius;
	}

	double ServiceLaneWidth()
	{
		// FOUR METRES, a service road's own lane, and the figure FStandLaneBuild carried as
		// LaneWidth before the stand's minimum width was derived from it.
		return 400.0;
	}

	EIcaoCode StandLetterFor(EIcaoCode Code)
	{
		// THE ONE ALIAS - see the header. A parks on B's ground now; every other letter is itself.
		return Code == EIcaoCode::A ? EIcaoCode::B : Code;
	}

	EIcaoCode SmallestStandLetter()
	{
		return StandLetterFor(EIcaoCode::A);
	}

	double StandWidthForLetter(EIcaoCode Code)
	{
		return WidthOf(RowFor(StandLetterFor(Code)));
	}

	double WingtipClearanceForLetter(EIcaoCode Code)
	{
		return RowFor(Code).WingtipClearance;
	}

	double StandDepthForLetter(EIcaoCode Code)
	{
		return RowFor(StandLetterFor(Code)).StandDepth;
	}

	FLetterEnvelope FloorEnvelopeForLetter(EIcaoCode Code)
	{
		const int32 Index = static_cast<int32>(Code);
		check(Index >= 0 && Index < UE_ARRAY_COUNT(EnvelopeFloors));
		const FEnvelopeFloorRow& Row = EnvelopeFloors[Index];

		FLetterEnvelope Envelope;
		Envelope.MaxTailAft = Row.MaxTailAft;
		Envelope.MaxNoseFwd = Row.MaxNoseFwd;
		return Envelope;
	}

	double WingFwdForLetter(EIcaoCode Code)
	{
		return RowFor(Code).WingFwd;
	}

	double WingAftForLetter(EIcaoCode Code)
	{
		return RowFor(Code).WingAft;
	}

	bool WingKeepOutContains(EIcaoCode Code, const FVector2D& Local)
	{
		const FRow& Row = RowFor(Code);
		return Local.X >= Row.WingAft && Local.X <= Row.WingFwd
			&& FMath::Abs(Local.Y) <= 0.5 * Row.MaxWingspan;
	}

	bool WingKeepOutCrossedBy(EIcaoCode Code, const FVector2D& A, const FVector2D& B)
	{
		const FRow& Row = RowFor(Code);
		const double HalfSpan = 0.5 * Row.MaxWingspan;

		// LIANG-BARSKY against the box, which answers "does any part of this segment lie
		// inside" rather than "is either end inside". A leg that clips a corner of the wing
		// between two of its samples is exactly the crossing a per-point test would miss, and
		// missing it is how a truck ends up driving through a wing in PIE with a green suite.
		double Enter = 0.0;
		double Leave = 1.0;
		const FVector2D Delta = B - A;

		const double P[4] = { -Delta.X, Delta.X, -Delta.Y, Delta.Y };
		const double Q[4] = {
			A.X - Row.WingAft, Row.WingFwd - A.X,
			A.Y + HalfSpan, HalfSpan - A.Y };

		for (int32 Side = 0; Side < 4; ++Side)
		{
			if (FMath::IsNearlyZero(P[Side]))
			{
				// PARALLEL TO THIS EDGE. Outside it means the whole segment is outside the
				// slab and no amount of the other three can bring it back.
				if (Q[Side] < 0.0)
				{
					return false;
				}
				continue;
			}

			const double T = Q[Side] / P[Side];
			if (P[Side] < 0.0)
			{
				Enter = FMath::Max(Enter, T);
			}
			else
			{
				Leave = FMath::Min(Leave, T);
			}
		}

		// STRICTLY GREATER excludes ONE case and it is worth naming, because the first draft of
		// this comment claimed a larger one: a segment touching the box at a single POINT - a
		// corner, or a tangent - is not a crossing. A segment lying ALONG an edge still
		// overlaps it over a range and IS reported, which is right for a keep-out: a lane laid
		// exactly on the wingtip is a lane under the wingtip. Measured, not assumed - the test
		// asserting otherwise failed on its first run.
		return Leave > Enter;
	}

	FString LetterForStandSize(double WidthUu, double DepthUu)
	{
		// LARGEST THAT FITS, walked backwards, and it must be a search rather than the first
		// row that fails: the rows are ordered by span, and a stand can be D-wide while only
		// B-deep, so the letters that fit are not a prefix of the table.
		for (int32 Index = UE_ARRAY_COUNT(Rows) - 1; Index >= 0; --Index)
		{
			const FRow& Row = Rows[Index];
			if (!HasStands(Row)) { continue; }   // Smaller than the smallest stand letter.
			if (WidthUu >= WidthOf(Row) && DepthUu >= Row.StandDepth)
			{
				return Row.Letter;
			}
		}
		// Smaller than the smallest stand letter. Empty, not a letter: see the header for why a
		// letter here would admit an aircraft to a space it does not fit.
		return FString();
	}

	bool StandAdmits(double StandDesignSpanUu, double AircraftSpanUu)
	{
		if (StandDesignSpanUu <= 0.0 || AircraftSpanUu <= 0.0)
		{
			return true;
		}
		if (AircraftSpanUu > MaxWingspanForLetter(EIcaoCode::F))
		{
			return false;
		}
		return static_cast<uint8>(CodeForWingspan(StandDesignSpanUu))
			>= static_cast<uint8>(CodeForWingspan(AircraftSpanUu));
	}

	int32 StandRank(double StandDesignSpanUu)
	{
		if (StandDesignSpanUu <= 0.0)
		{
			return static_cast<int32>(EIcaoCode::C);
		}
		return static_cast<int32>(CodeForWingspan(StandDesignSpanUu));
	}
}
