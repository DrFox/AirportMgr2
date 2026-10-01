#pragma once

#include "CoreMinimal.h"

class FOpsEventBus;
class USimClock;

/**
 * THE SAFETY NET for every pass that polls something no event announces (ops event bus spec §2), as one object beside the
 * bus (#445). While a pass WANTS it - flights are holding, a due turnaround's departure is refused - ONE clock entry fires every
 * PeriodSeconds and marks each wanting pass dirty with EPassCause::SafetyNet, so the pass runs anyway. If THAT run - one no
 * event asked for - finds work (clears a flight, gets an aircraft away), an event that should have covered it is missing,
 * and the pass says so as a Warning (FPassRun::IsSafetyOnly), which a test fails on. A missing event becomes a named defect,
 * not a stuck airport.
 *
 * WHY AN OBJECT AND NOT FIELDS ON UOpsRuntime. The net used to be a handle, two "wanted" bools, two "safety due" bools and two
 * "covered" bools on the composition root, plus a funnel function per pass and a lint rule (36) whose only job was to keep
 * the funnels used: a pass that needed to know WHY it ran reinvented that bookkeeping, and review corrected it three times. The
 * "why" is the bus's now (EPassCause, carried by FPassRun), the "wanted" state is here keyed by pass name, so a third
 * net-watched pass is one Want() call and no field anywhere.
 *
 * ONE CLOCK ENTRY, NOT ONE PER PASS: the passes share the period and the places that must cancel them (a load, a detach),
 * and a second handle is a second thing to forget at each. A paused clock fires nothing. Each pass says whether it wants the net after every
 * run it makes (FQueueTick::Waiting for the arrival queue, UJobBoard::HasRefusedDeparture for the job board, 2026-09-30) and REMOVED ONCE QUIET IN
 * PLAY: the entry is cancelled the moment no pass wants it, so a quiet airport books nothing.
 * ENFORCED BY: AirportOps.Model.Bus.ThirdNetWatchedPassNeedsNoRuntimeField, AirportOps.Present.ArrivalQueue.SafetyNetCatchesAMissedEvent,
 * AirportOps.Present.PushGroundFreed.SafetyNetDepartsAMissedOne
 *
 * PLAIN C++: it holds a clock handle and names, and nothing about it is saved. Owned by value by UOpsRuntime, which owns the
 * bus and the clock it points at.
 */
class AIRPORTOPS_API FOpsSafetyNet
{
public:
	/** The bus the net marks, the clock it books on and the game seconds between runs. Both outlive it (the runtime owns all three). */
	void Bind(FOpsEventBus& InBus, USimClock& InClock, double InPeriodSeconds);

	/**
	 * Whether Pass wants the net, asked after EVERY run of the pass (it is the pass that knows whether it is still waiting on
	 * something no event will announce). Wanted by any pass arms the entry; wanted by none cancels it. A pass that is no longer
	 * wanted stops being marked - a stale "wanted" would run it for nothing every period.
	 */
	void Want(FName Pass, bool bWanted);

	/** Nobody wants it: a load (its clock is another - the entry was booked against the old one) or a detach (no airport to guard). */
	void CancelAll();

	bool IsArmed() const { return Handle != INDEX_NONE; }
	bool IsWantedBy(FName Pass) const { return Wanting.Contains(Pass); }

private:
	FOpsEventBus* Bus = nullptr;
	USimClock* Clock = nullptr;
	double PeriodSeconds = 30.0;
	int32 Handle = INDEX_NONE;
	TArray<FName> Wanting;

	void Fire();
	void Rearm();
};
