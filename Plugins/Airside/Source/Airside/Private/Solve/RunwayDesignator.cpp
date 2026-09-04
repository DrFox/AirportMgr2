#include "Solve/RunwayDesignator.h"

namespace RunwayDesignator
{
	int32 Designate(const FVector2D& Direction)
	{
		if (Direction.IsNearlyZero())
		{
			// No bearing at all. Reported as invalid rather than as north: a runway with no
			// length would otherwise be called 36 and look like a real one.
			return 0;
		}

		// +X is north, +Y is east, so a compass bearing is atan2(East, North). Written this
		// way round deliberately - the usual atan2(Y, X) reads as a maths angle from +X and
		// happens to give the same number here only because of that convention.
		const double Bearing = FMath::RadiansToDegrees(FMath::Atan2(Direction.Y, Direction.X));

		// Into (0, 360]. Fmod leaves negatives negative, and 0 must land on 360 so that it
		// rounds to 36 rather than to nothing.
		double Compass = FMath::Fmod(Bearing, 360.0);
		if (Compass <= 0.0)
		{
			Compass += 360.0;
		}

		// Nearest ten degrees. 355 and 004 both round to north and are both runway 36.
		int32 Tens = FMath::RoundToInt(Compass / 10.0);
		if (Tens <= 0 || Tens > 36)
		{
			Tens = 36;
		}
		return Tens;
	}

	int32 Reciprocal(int32 Designator)
	{
		if (Designator < 1 || Designator > 36)
		{
			return 0;
		}

		// Half a turn is eighteen tens. Wrapped into 1..36 rather than 0..35, because 36 is
		// the name for north and 0 is not a runway.
		const int32 Other = Designator + 18;
		return Other > 36 ? Other - 36 : Other;
	}

	FString ToText(int32 Designator)
	{
		if (Designator < 1 || Designator > 36)
		{
			return FString();
		}
		return FString::Printf(TEXT("%02d"), Designator);
	}

	FString ToPairText(const FVector2D& Direction)
	{
		const int32 One = Designate(Direction);
		if (One == 0)
		{
			return FString();
		}

		const int32 Two = Reciprocal(One);
		const int32 Low = FMath::Min(One, Two);
		const int32 High = FMath::Max(One, Two);
		return FString::Printf(TEXT("%s/%s"), *ToText(Low), *ToText(High));
	}
}
