#include "Model/GameTimeText.h"

#include "Model/SimClock.h"

#define LOCTEXT_NAMESPACE "GameTimeText"

int32 GameTimeText::WholeMinutes(double Seconds)
{
	return FMath::Max(0, FMath::RoundToInt(Seconds / 60.0));
}

FText GameTimeText::Duration(double Seconds)
{
	const int32 Minutes = WholeMinutes(Seconds);
	if (Minutes < 60)
	{
		return FText::Format(LOCTEXT("DurationMin", "{0} min"), FText::AsNumber(Minutes));
	}
	const int32 Hours = Minutes / 60;
	const int32 Rest = Minutes % 60;
	return Rest == 0
		? FText::Format(LOCTEXT("DurationH", "{0} h"), FText::AsNumber(Hours))
		: FText::Format(LOCTEXT("DurationHMin", "{0} h {1} min"), FText::AsNumber(Hours), FText::AsNumber(Rest));
}

FString GameTimeText::TimeOfDay(double TimeOfDaySeconds)
{
	const int32 Hour = static_cast<int32>(TimeOfDaySeconds / 3600.0);
	const int32 Minute = static_cast<int32>(FMath::Fmod(TimeOfDaySeconds, 3600.0) / 60.0);
	return FString::Printf(TEXT("%02d:%02d"), Hour, Minute);
}

FString GameTimeText::Stamp(double GameSeconds)
{
	const int32 Day = static_cast<int32>(GameSeconds / USimClock::SecondsPerDay) + 1;
	return FString::Printf(TEXT("Day %d  %s"), Day, *TimeOfDay(FMath::Fmod(GameSeconds, USimClock::SecondsPerDay)));
}

#undef LOCTEXT_NAMESPACE
