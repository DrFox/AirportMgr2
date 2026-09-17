#include "Solve/IcaoCode.h"

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
			 * How deep a stand of this letter is, uu - nose to the back of its GSE road.
			 *
			 * AUTHORED, not derived, and it is the only figure here that is. Width follows
			 * from span and clearance; depth follows from aircraft LENGTH and the room an
			 * equipment area and a service road need, and no clean rule produces it. Standard
			 * aerodrome design values, as the header says of the rest - the first thing to
			 * check if a real layout looks wrong.
			 */
			double StandDepth;

			/**
			 * The longest airframe this letter admits, as uu AFT of the nose-gear stop mark.
			 *
			 * CODE C IS MEASURED and the rest are authored. C is the 737-800's tail at 3430,
			 * which is the longest type this project ships, and IcaoCodeTest pins it against
			 * Build737's own figure so the two cannot drift. No type is authored at any other
			 * letter, so those are standard design values in the sense this file's header
			 * gives for the rest of the table - revise one when a type arrives that exceeds
			 * it, and the drift test in StandLayoutTest is what will say so.
			 */
			double MaxTailAft;
		};

		// D and E deliberately share RunwayWidth (45 m serves both) - see MaxWingspanForWidth.
		static const FRow Rows[] = {
			{ TEXT("A"), 1500.0, 1800.0, 1500.0,  300.0,  2000.0,  1000.0 },
			{ TEXT("B"), 2400.0, 2300.0, 2000.0,  300.0,  3000.0,  2000.0 },
			{ TEXT("C"), 3600.0, 3000.0, 2500.0,  450.0,  5500.0,  3430.0 },
			{ TEXT("D"), 5200.0, 4500.0, 4000.0,  750.0,  7000.0,  5500.0 },
			{ TEXT("E"), 6500.0, 4500.0, 5000.0,  750.0,  9000.0,  6700.0 },
			{ TEXT("F"), 8000.0, 6000.0, 6000.0,  750.0, 10000.0,  6900.0 },
		};

		/** The stand width a row implies, uu. The ONE place the derivation is written. */
		static double WidthOf(const FRow& Row)
		{
			return Row.MaxWingspan + 2.0 * Row.WingtipClearance;
		}

		/**
		 * The row for a letter, or null. Case-insensitive, as the letter arrives from a
		 * data asset a human typed.
		 */
		static const FRow* FindRow(const FString& Letter)
		{
			const FString Upper = Letter.ToUpper();
			for (const FRow& Row : Rows)
			{
				if (Upper == Row.Letter)
				{
					return &Row;
				}
			}
			return nullptr;
		}

		/**
		 * Code C's row, the fallback every letter-keyed lookup here shares. Looked up by
		 * letter rather than indexed, so inserting a row cannot silently move the fallback
		 * to a neighbouring code.
		 */
		static const FRow& CodeC()
		{
			const FRow* Row = FindRow(TEXT("C"));
			check(Row != nullptr);
			return *Row;
		}
	}

	FString LetterForWingspan(double WingspanUu)
	{
		for (const FRow& Row : Rows)
		{
			if (WingspanUu < Row.MaxWingspan)
			{
				return Row.Letter;
			}
		}
		// Wider than every row: still F, the widest letter the table has.
		return Rows[UE_ARRAY_COUNT(Rows) - 1].Letter;
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

	double RadiusForLetter(const FString& Letter)
	{
		if (const FRow* Row = FindRow(Letter))
		{
			return Row->StandTurnRadius;
		}
		// No code, or one nobody recognises. Code C is the commonest stand in the world, and
		// erring to Code F instead would put a 60 m curve on a light-aircraft apron.
		return CodeC().StandTurnRadius;
	}

	double StandWidthForLetter(const FString& Letter)
	{
		if (const FRow* Row = FindRow(Letter))
		{
			return WidthOf(*Row);
		}
		// Same fallback and the same reason as RadiusForLetter: an unknown letter gets the
		// commonest stand rather than the biggest, which would swallow the apron beside it.
		return WidthOf(CodeC());
	}

	double StandDepthForLetter(const FString& Letter)
	{
		if (const FRow* Row = FindRow(Letter))
		{
			return Row->StandDepth;
		}
		return CodeC().StandDepth;
	}

	double MaxTailAftForLetter(const FString& Letter)
	{
		if (const FRow* Row = FindRow(Letter))
		{
			return Row->MaxTailAft;
		}
		return CodeC().MaxTailAft;
	}

	FString LetterForStandSize(double WidthUu, double DepthUu)
	{
		// LARGEST THAT FITS, walked backwards, and it must be a search rather than the first
		// row that fails: the rows are ordered by span, and a stand can be D-wide while only
		// B-deep, so the letters that fit are not a prefix of the table.
		for (int32 Index = UE_ARRAY_COUNT(Rows) - 1; Index >= 0; --Index)
		{
			const FRow& Row = Rows[Index];
			if (WidthUu >= WidthOf(Row) && DepthUu >= Row.StandDepth)
			{
				return Row.Letter;
			}
		}
		// Smaller than Code A in one dimension or both. Empty, not "A": see the header for
		// why a letter here would admit an aircraft to a space it does not fit.
		return FString();
	}
}
