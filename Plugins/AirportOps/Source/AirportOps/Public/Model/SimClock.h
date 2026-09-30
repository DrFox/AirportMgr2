#pragma once

#include "CoreMinimal.h"
#include "Model/OpsSave.h"
#include "UObject/Object.h"
#include "SimClock.generated.h"

/**
 * The player's speed setting. AN ENUM, NOT A FLOAT: the game offers exactly these steps,
 * and a float would let two code paths disagree about what "faster" means. Paused lives
 * here rather than as a separate bool because "paused AND x4" is not a state the game has.
 */
UENUM(BlueprintType)
enum class ESimSpeed : uint8
{
	Paused,
	X1,
	X2,
	X4,
	X8,
	X16,
	X32
};

/**
 * The one authority on game time.
 *
 * Two independent scalings meet here and it matters which is which:
 *  - DAY COMPRESSION (RealSecondsDaylight, RealSecondsNight): how many real seconds the
 *    daylight hours (DawnHour..DuskHour) and the night hours take. TWO BANDS, each at its own
 *    rate (spec 2026-09-28 section 1): a night is a lull, and a lull the player has to sit
 *    through at a busy morning's pace is dead time. This scales the CLOCK only - upkeep
 *    ticks, contract deliveries, off-block times.
 *  - SPEED (ESimSpeed): the player's x1..x32. This scales the clock AND the movement layer.
 *
 * Airside agents run on Multiplier() alone, never on TimeScale(): a truck that drove 72x
 * faster because the day is 40 real minutes long would be unwatchable. Turnaround
 * durations are therefore authored in game time knowing a truck covers "real" distance
 * while the clock runs compressed - the standard sim compromise, stated here so nobody
 * tries to fix it by scaling movement.
 *
 * World-free: built with NewObject and advanced by hand in tests. The subsystem that
 * ticks it (UOpsRuntimeSubsystem) is in Present/ and is a forwarder only.
 *
 * SCHEDULER ENTRIES ARE NOT SAVED, on purpose. They hold TFunctions, which cannot be
 * serialised, and every system that schedules something owns the knowledge of what to
 * schedule - so on load each system re-registers from its own saved state. Saving the
 * queue would duplicate that knowledge and desync the two on the first edit.
 */
UCLASS()
class AIRPORTOPS_API USimClock : public UObject, public IOpsPersistent
{
	GENERATED_BODY()

public:
	/** Game seconds in one game day. A definition, not a tunable. */
	static constexpr double SecondsPerDay = 86400.0;

	// --- IOpsPersistent ---------------------------------------------------------------
	/** "Clock": the name OpsSave.h's shim already expects for a pre-v4 save's bytes. */
	virtual FName SaveBlobName() const override { return TEXT("Clock"); }
	virtual UObject& AsPersistentObject() override { return *this; }

	/**
	 * Real seconds the daylight hours (DawnHour to DuskHour) take at x1, and the night hours
	 * (DuskHour to DawnHour). Tunables, set from the scenario by UOpsRuntime::ApplyScenarioFigures;
	 * the defaults here are only what a bare NewObject gets.
	 *
	 * TRANSIENT, NOT SAVED (#449) - UAirlineRoster::Tuning's ruling: a design figure is the scenario's,
	 * and a save made before a retune must not carry the old one forward. It WAS saved "like every
	 * figure on this object"; Now and the speed still are, being the game's rather than the design's.
	 * ENFORCED BY: AirportOps.Model.Save.DesignFiguresAreNotSaved (not saved), AirportOps.Present.RuntimeLoad.DesignFiguresAreTheScenarios (re-applied)
	 *
	 * REPLACED RealSecondsPerGameDay (spec 2026-09-28): one figure for the whole day made the
	 * night as slow as the morning peak. SetUniformDay gives the old single-rate behaviour.
	 */
	UPROPERTY(Transient) double RealSecondsDaylight = 2400.0;
	UPROPERTY(Transient) double RealSecondsNight = 480.0;

	/** Hours of day, 0-24, at which daylight starts and ends. Dawn < Dusk. Transient - see above. */
	UPROPERTY(Transient) double DawnHour = 6.0;
	UPROPERTY(Transient) double DuskHour = 20.0;

	/** Split RealSecondsPerDay across the two bands so the rate is the same all day - the
	 *  old single-figure clock, which most tests want so their arithmetic reads plainly. */
	void SetUniformDay(double RealSecondsPerDay);

	/** True between DawnHour (inclusive) and DuskHour (exclusive). */
	bool IsDaylight(double TimeOfDaySeconds) const;
	bool IsDaylight() const { return IsDaylight(TimeOfDay()); }

	bool IsPaused() const { return Speed == ESimSpeed::Paused; }

	/** Game seconds per real second at x1, in the band TimeOfDaySeconds falls in. */
	double GameSecondsPerRealSecond(double TimeOfDaySeconds) const;

	/**
	 * MOVEMENT seconds - real time x the speed multiplier, what agents run on (FAgentHold::StalledSeconds counts in it, and so does a
	 * drive's length in a bid) - as GAME seconds: x the day's compression at this clock's time of day NOW. The ONE conversion between
	 * the two time bases (#447): a widget's public static and the job board's bid each wrote it, and a widget is no place for a rule two
	 * layers depend on.
	 *
	 * NOT TimeScale(): that is Multiplier x day rate, and movement seconds already carry the multiplier (agents run on it). Only the day's
	 * compression is missing. THE RATE NOW, not integrated over the span: a span that straddles dawn or dusk reads at the current band's
	 * rate, an error of one band change against a figure shown to the minute. A caller converting a long FUTURE span should Advance.
	 * ENFORCED BY: Check-Architecture rule 4's 'movement seconds convert on the clock' row (no caller outside SimClock re-derives it),
	 * AirportOps.Model.SimClock.MovementSecondsConvertAtTheBandsRate
	 */
	double GameSecondsOfMovement(double MovementSeconds) const;

	double Now() const { return GameSeconds; }
	int32 Day() const;
	double TimeOfDay() const;

	ESimSpeed GetSpeed() const { return Speed; }
	void SetSpeed(ESimSpeed NewSpeed);

	/**
	 * The rungs StepSpeed walks, in order, fastest last. Paused is deliberately NOT a rung:
	 * it is TogglePause's business, and "step faster" from paused means resume, not unpause
	 * into the slowest speed.
	 *
	 * Public because it is a list that must agree with ESimSpeed and something has to be
	 * able to check that - see AirportOps.Model.SimClock.SpeedLadderCoversEveryRung, which
	 * walks StaticEnum and fails if the two ever drift apart.
	 *
	 * MOVED FROM UOpsRuntime (issue #191, #98 partial), and ResumeSpeed and StepSpeed/
	 * TogglePause followed it: they are this clock's own arithmetic over its own field, not
	 * something the runtime should reach in to compute. See ResumeSpeed's own comment for the
	 * save bug that made the move worth doing, not just tidier.
	 */
	static TArrayView<const ESimSpeed> SpeedLadder();

	/** Speed control. StepSpeed(+1) climbs SpeedLadder() and stops at the top; -1 descends. */
	void StepSpeed(int32 Delta);
	/** Paused <-> the speed that was set before pausing. */
	void TogglePause();

	/** The speed table: 0, 1, 2, 4, 8. What Airside movement is scaled by. */
	static double Multiplier(ESimSpeed Speed);

	/** Multiplier() * GameSecondsPerRealSecond(TimeOfDay()): game seconds per real second NOW.
	 *  Changes at dawn and dusk - a caller converting a long span should Advance, not multiply. */
	double TimeScale() const;

	/**
	 * Advances game time by RealDeltaSeconds, each part of it at the rate of the band it falls
	 * in (a step across dusk is split there), then fires every scheduled entry
	 * whose due time has passed, oldest first. A repeating entry that falls due several
	 * times inside one step fires that many times.
	 */
	void Advance(double RealDeltaSeconds);

	/**
	 * Put the clock at a time of day on day one, for a NEW GAME.
	 *
	 * Call it before anything is scheduled: Every() books its first firing at
	 * Now() + Interval, so moving the clock afterwards leaves every repeating entry due at
	 * a time that no longer means what it did when it was booked.
	 *
	 * Not for loading a save - Restore deserialises GameSeconds straight off the snapshot,
	 * which is the whole point of saving it.
	 */
	void StartAtHour(double Hour);

	/** Fire once at an absolute game time. Returns a handle for Cancel. A past time fires on the next Advance. */
	int32 At(double GameTime, TFunction<void()> Callback);

	/** Fire every Interval game seconds, first at Now()+Interval. INDEX_NONE if Interval <= 0. */
	int32 Every(double Interval, TFunction<void()> Callback);

	/**
	 * Fire every Interval game seconds, FIRST at FirstDue - for a beat that belongs to a moment and not to the moment it was
	 * booked (#442): the day's upkeep is due at MIDNIGHT, and Every books it a day after the last attach or load, so a load
	 * at 05:59 pushed the 06:00 upkeep to the next day and repeated loads avoided it altogether. A past FirstDue fires on the
	 * next Advance, as At does. INDEX_NONE if Interval <= 0.
	 */
	int32 EveryFrom(double FirstDue, double Interval, TFunction<void()> Callback);

	/**
	 * The game time of the next midnight - the start of the day after this one, strictly later than Now() (a clock exactly at
	 * 00:00 is at the START of a day whose boundary already fired). SecondsPerDay is the day's definition, not the real-time
	 * length the scenario sets, so this does not move when the day's real length does (USimClock::SetUniformDay).
	 * ENFORCED BY: AirportOps.Present.Upkeep.PostsAtMidnightAfterALoad
	 */
	double NextDayStart() const;

	/** True if the handle named a pending entry. */
	bool Cancel(int32 Handle);

	/** How many entries are waiting to fire - for the tests that prove an arrival was DISARMED (#442): a cancelled flight's
	 *  callback is guarded by its phase, so "still cancelled after its ETA" passes whether or not the entry was cancelled. */
	int32 PendingForTest() const { return Entries.Num(); }

private:
	struct FEntry
	{
		int32 Handle = INDEX_NONE;
		double Due = 0.0;
		/** 0 for a one-shot. */
		double Interval = 0.0;
		TFunction<void()> Callback;
	};

	UPROPERTY() double GameSeconds = 0.0;
	UPROPERTY() ESimSpeed Speed = ESimSpeed::X1;

	/**
	 * What TogglePause returns to. X1 if nothing was ever set.
	 *
	 * LIVES HERE, NOT ON UOpsRuntime (issue #191, #98 partial): it used to, and UOpsRuntime is
	 * not in UOpsRuntime::Persistents() - by design, it is Present/ composition, not saved
	 * state (see its own class comment) - so a game saved while paused reloaded with this at
	 * its constructor default no matter what speed the player had actually paused from. It is
	 * just another non-Transient UPROPERTY on the object that IS saved, per OpsSave's own
	 * "the RULE": a model object's non-Transient UPROPERTYs are its saved state.
	 */
	UPROPERTY() ESimSpeed ResumeSpeed = ESimSpeed::X1;

	// Plain members, not UPROPERTY: see the class comment on why the queue is not saved.
	TArray<FEntry> Entries;
	int32 NextHandle = 1;

	int32 Add(double Due, double Interval, TFunction<void()>&& Callback);
};
