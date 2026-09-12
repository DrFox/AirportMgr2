#include "NotificationCentre.h"

DEFINE_LOG_CATEGORY_STATIC(LogNotify, Log, All);

void UNotificationCentre::PostFeed(const FText& Text)
{
	FNotificationEntry Entry;
	Entry.Kind = ENotificationKind::Feed;
	Entry.Text = Text;
	Entry.RaisedAtRealSeconds = NowRealSeconds;
	List.Add(Entry);

	// Oldest first, and only feed entries are candidates: an alert dropped because the feed
	// was busy would hide a live problem behind a run of chatter.
	while (List.Num() > MaxEntries)
	{
		const int32 Oldest = List.IndexOfByPredicate(
			[](const FNotificationEntry& E) { return E.Kind == ENotificationKind::Feed; });
		if (Oldest == INDEX_NONE)
		{
			break;
		}
		List.RemoveAt(Oldest);
	}

	UE_LOG(LogNotify, Log, TEXT("Feed: %s"), *Text.ToString());
}

void UNotificationCentre::RaiseAlert(FName SourceId, const FText& Text)
{
	// Keyed, because a condition re-detected every tick would otherwise stack a copy a frame.
	for (FNotificationEntry& Entry : List)
	{
		if (Entry.Kind == ENotificationKind::Alert && Entry.SourceId == SourceId)
		{
			Entry.Text = Text;
			return;
		}
	}

	FNotificationEntry Entry;
	Entry.Kind = ENotificationKind::Alert;
	Entry.Text = Text;
	Entry.SourceId = SourceId;
	Entry.RaisedAtRealSeconds = NowRealSeconds;
	List.Add(Entry);
	UE_LOG(LogNotify, Warning, TEXT("Alert raised [%s]: %s"), *SourceId.ToString(), *Text.ToString());
}

void UNotificationCentre::ClearAlert(FName SourceId)
{
	const int32 Removed = List.RemoveAll([SourceId](const FNotificationEntry& Entry)
	{
		return Entry.Kind == ENotificationKind::Alert && Entry.SourceId == SourceId;
	});
	if (Removed > 0)
	{
		UE_LOG(LogNotify, Log, TEXT("Alert cleared [%s]"), *SourceId.ToString());
	}
}

void UNotificationCentre::Advance(double RealDeltaSeconds)
{
	NowRealSeconds += RealDeltaSeconds;

	// ALERTS ARE EXEMPT. They last as long as the condition holds, not as long as a reading
	// time - that is the whole difference between the two kinds.
	List.RemoveAll([this](const FNotificationEntry& Entry)
	{
		return Entry.Kind == ENotificationKind::Feed
			&& (NowRealSeconds - Entry.RaisedAtRealSeconds) >= FeedLifetimeRealSeconds;
	});
}
