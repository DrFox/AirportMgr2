#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "NotificationCentre.generated.h"

/**
 * What a notification needs of the player. AN ENUM, NOT A SET OF BOOLS, for the reason
 * EAgentPhase and ECrossingPhase are: "transient AND persists while true" is not a state.
 *
 * OFFERS ARE NOT HERE. They need a decision and carry a deadline, UFlightBoard already owns
 * them, and a second store would be a second source of truth about what the player has been
 * offered - the failure this codebase has shipped three times. The inbox widget reads the
 * board directly through its existing viewmodel.
 */
UENUM()
enum class ENotificationKind : uint8
{
	/** Happened, worth knowing, no action. Expires. */
	Feed,
	/** A condition that is true now. Persists until cleared. */
	Alert
};

/**
 * How much the thing that happened matters. SEPARATE FROM KIND, because the two answer
 * different questions: Kind is what the notification NEEDS of the player (nothing, or a
 * standing condition), Severity is how the news READS. A feed entry can be routine or
 * alarming without changing what the player must do about it.
 *
 * Drives colour and icon and nothing else - a severity that changed behaviour would be a
 * Kind wearing a disguise.
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

	UPROPERTY() ENotificationKind Kind = ENotificationKind::Feed;
	UPROPERTY() ENotificationSeverity Severity = ENotificationSeverity::Info;
	UPROPERTY() FText Text;
	/** Alerts only: the condition's identity, so re-raising does not stack. */
	UPROPERTY() FName SourceId;
	UPROPERTY() double RaisedAtRealSeconds = 0.0;
};

/**
 * Everything the player is told, and the rules for how long each kind stays.
 *
 * World-free: built with NewObject and advanced by hand in tests, like USimClock and
 * UFlightBoard. The widget that draws it is a forwarder.
 *
 * Replaces a single UTextBlock that every notification OVERWROTE and nothing ever cleared,
 * so two events in one second left only the second, permanently.
 */
UCLASS()
class AIRPORTMGR_API UNotificationCentre : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * How long a feed entry stays, in REAL seconds.
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
	void RaiseAlert(FName SourceId, const FText& Text,
		ENotificationSeverity Severity = ENotificationSeverity::Warning);
	void ClearAlert(FName SourceId);

	/** RAW frame seconds in - never multiplied by Multiplier() and never by TimeScale(). */
	void Advance(double RealDeltaSeconds);

	/** Real seconds since this centre was created, as Advance has been told them. */
	double Now() const { return NowRealSeconds; }

	TConstArrayView<FNotificationEntry> Entries() const { return List; }

private:
	UPROPERTY() TArray<FNotificationEntry> List;
	double NowRealSeconds = 0.0;
};
