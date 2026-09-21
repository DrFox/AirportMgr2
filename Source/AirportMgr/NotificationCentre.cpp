#include "NotificationCentre.h"

DEFINE_LOG_CATEGORY_STATIC(LogNotify, Log, All);

void UNotificationCentre::PostFeed(const FText& Text, ENotificationSeverity Severity)
{
	FNotificationEntry Entry;
	Entry.Severity = Severity;
	Entry.Text = Text;
	Entry.RaisedAtRealSeconds = NowRealSeconds;
	Entry.Id = NextEntryId++;
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

bool UNotificationCentre::RemoveEntryForTest(int32 EntryId)
{
	const int32 FoundIndex = List.IndexOfByPredicate(
		[EntryId](const FNotificationEntry& Entry) { return Entry.Id == EntryId; });
	if (FoundIndex == INDEX_NONE)
	{
		return false;
	}
	List.RemoveAt(FoundIndex);
	return true;
}
