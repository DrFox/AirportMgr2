#include "NotificationCentre.h"

DEFINE_LOG_CATEGORY_STATIC(LogNotify, Log, All);

void UNotificationCentre::PostFeed(const FText& Text, ENotificationSeverity Severity)
{
	FNotificationEntry Entry;
	Entry.Severity = Severity;
	Entry.Text = Text;
	Entry.RaisedAtRealSeconds = NowRealSeconds;
	List.Add(Entry);

	if (List.Num() > MaxEntries)
	{
		List.RemoveAt(0);
	}

	UE_LOG(LogNotify, Log, TEXT("Feed: %s"), *Text.ToString());
}

void UNotificationCentre::Advance(double RealDeltaSeconds)
{
	NowRealSeconds += RealDeltaSeconds;
	List.RemoveAll([this](const FNotificationEntry& Entry)
	{
		return (NowRealSeconds - Entry.RaisedAtRealSeconds) >= FeedLifetimeRealSeconds;
	});
}
