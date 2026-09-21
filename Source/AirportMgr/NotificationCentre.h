#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "NotificationCentre.generated.h"

/**
 * How much the thing that happened matters. Drives colour and icon and nothing else - a
 * severity that changed behaviour would be doing a different job than this one.
 */
UENUM()
enum class ENotificationSeverity : uint8
{
	/** It happened. No judgement. */
	Info,
	/** It worked. */
	Success,
	/** It did not work, or it will not, and the player may want to act. */
	Warning
};

USTRUCT()
struct FNotificationEntry
{
	GENERATED_BODY()

	UPROPERTY() ENotificationSeverity Severity = ENotificationSeverity::Info;
	UPROPERTY() FText Text;
	UPROPERTY() double RaisedAtRealSeconds = 0.0;

	/**
	 * Stable identity, assigned once by PostFeed and never reused. Added for issue #186: the
	 * toast widget keeps one card per entry across ticks, and telling "the same entry, still
	 * here" from "a different entry that happens to sit at the same array index" needs a key
	 * that outlives the entry's position in List - an index shifts every time the FRONT is
	 * trimmed, an Id does not.
	 */
	UPROPERTY() int32 Id = INDEX_NONE;
};

/**
 * Everything the player is told, and the rule for how long an entry stays.
 *
 * World-free: built with NewObject and advanced by hand in tests, like USimClock and
 * UFlightBoard. The widget that draws it is a forwarder.
 *
 * Replaces a single UTextBlock that every notification OVERWROTE and nothing ever cleared,
 * so two events in one second left only the second, permanently.
 *
 * USED TO ALSO CARRY "Alert" - a standing condition kind meant to persist until cleared,
 * keyed by source so a re-detected condition did not stack. Issue #105 review found it had
 * shipped with no producer (nothing ever called RaiseAlert in play) and no surface (the
 * toast stack explicitly skipped drawing it, "alerts have their own surface" pointing at
 * nothing) - nine months of dead code covered only by its own tests. Cut rather than wired:
 * a real producer (FuelService "no depot" and friends) needs its own bus event and its own
 * persistent-banner surface, which is a feature, not a cleanup. Re-add Kind/RaiseAlert/
 * ClearAlert from this issue's history if that feature is actually built.
 */
UCLASS()
class AIRPORTMGR_API UNotificationCentre : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * How long an entry stays, in REAL seconds.
	 *
	 * REAL, not game, and NOT SCALED BY SPEED EITHER. SimClock.h's class comment draws the
	 * first distinction and this adds the second, because both would shorten a toast: the
	 * clock runs at the day compression (72x) times the player's speed, which now reaches
	 * x32, so a toast timed in game seconds would be gone before the eye arrived - and one
	 * merely scaled by Multiplier() would live a quarter of a second at x32. A toast is a
	 * piece of UI a HUMAN reads; its lifetime belongs to the human, not to the simulation.
	 * Advance() is therefore handed raw frame time, and the test at a high multiplier
	 * asserts the entry is still up.
	 */
	UPROPERTY() double FeedLifetimeRealSeconds = 8.0;

	/** Oldest are dropped past this, so a long session cannot grow the list without bound. */
	UPROPERTY() int32 MaxEntries = 50;

	void PostFeed(const FText& Text, ENotificationSeverity Severity = ENotificationSeverity::Info);

	/** RAW frame seconds in - never multiplied by Multiplier() and never by TimeScale(). */
	void Advance(double RealDeltaSeconds);

	/** Real seconds since this centre was created, as Advance has been told them. */
	double Now() const { return NowRealSeconds; }

	TConstArrayView<FNotificationEntry> Entries() const { return List; }

private:
	UPROPERTY() TArray<FNotificationEntry> List;
	double NowRealSeconds = 0.0;

	/** Never reused, never reset: see FNotificationEntry::Id. */
	int32 NextEntryId = 0;
};
