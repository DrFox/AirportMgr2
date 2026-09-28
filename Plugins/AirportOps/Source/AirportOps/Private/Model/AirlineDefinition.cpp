#include "Model/AirlineDefinition.h"

double UAirlineDefinition::CurveAt(double TimeOfDaySeconds) const
{
	// FLAT WHEN UNAUTHORED, not zero: an airline with no curve typed in should behave like the
	// old fixed cadence, not vanish from the game.
	if (DemandCurve.Num() == 0)
	{
		return 1.0;
	}
	// LINEAR BETWEEN HOURS, wrapping 23:00 -> 00:00. A step on the hour would make 07:59 and
	// 08:00 two different airports. A curve of the wrong length is read modulo its own length
	// rather than refused - the content test (TheAssetManagerScansThem) is where length is
	// policed, and a crash in play is not the way to say so.
	const int32 N = DemandCurve.Num();
	const double Hours = FMath::Fmod(FMath::Max(TimeOfDaySeconds, 0.0) / 3600.0, 24.0);
	const int32 Lo = FMath::FloorToInt32(Hours) % N;
	const int32 Hi = (Lo + 1) % N;
	const double T = Hours - FMath::FloorToDouble(Hours);
	return FMath::Clamp(FMath::Lerp(DemandCurve[Lo], DemandCurve[Hi], T), 0.0, 1.0);
}
