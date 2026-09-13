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
		};

		// D and E deliberately share RunwayWidth (45 m serves both) - see MaxWingspanForWidth.
		static const FRow Rows[] = {
			{ TEXT("A"), 1500.0, 1800.0, 1500.0 },
			{ TEXT("B"), 2400.0, 2300.0, 2000.0 },
			{ TEXT("C"), 3600.0, 3000.0, 2500.0 },
			{ TEXT("D"), 5200.0, 4500.0, 4000.0 },
			{ TEXT("E"), 6500.0, 4500.0, 5000.0 },
			{ TEXT("F"), 8000.0, 6000.0, 6000.0 },
		};
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
		const FString Upper = Letter.ToUpper();
		for (const FRow& Row : Rows)
		{
			if (Upper == Row.Letter)
			{
				return Row.StandTurnRadius;
			}
		}
		// No code, or one nobody recognises. Code C is the commonest stand in the world, and
		// erring to Code F instead would put a 60 m curve on a light-aircraft apron.
		return 2500.0;
	}
}
