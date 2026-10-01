#pragma once

#include "CoreMinimal.h"
#include "Model/ExhaustiveSwitch.h"
#include "AgentPhase.generated.h"

// MOVED HERE FROM RoadAgent.h (issue #444), with every word of its comment, so the enum and what each of its values
// MEANS sit in one small header: AirportOps and the game module ask the traits below, and a phase added is decided in
// this file rather than at a dozen consumers' default: arms. RoadAgent.h includes this, so nothing that named
// EAgentPhase through it has to change its includes.

/**
 * Where an agent has got to. Replaces five independent bools - bArriving, bDeparting,
 * bDepartOnArrival, bParked, plus the implicit "none of the above means taxiing" - that
 * could represent illegal combinations (bArriving && bDeparting) nothing ever checked for.
 *
 * See FRoadAgent for why the states stay separate structs (FLandingRun, FRouteFollower,
 * FTakeoffRun) rather than becoming subclasses of one.
 *
 * WHAT A PHASE MEANS is FAgentPhaseTraits' row for it, below - not a consumer's own list of
 * phases. A phase added here is a build error at AgentPhaseTraits' count until it has a row.
 */
UENUM()
enum class EAgentPhase : uint8
{
	/** The landing drives it, before the taxi to the stand. */
	Arriving,

	/** The follower drives it. Also the phase between a plain dispatch and any handover. */
	Taxiing,

	/** The take-off run drives it, after a taxi that ended on a runway with one armed. */
	Departing,

	/** At the stand, taxi over, running down the post-arrival shutdown pause. */
	Parked,

	/**
	 * Coming off the stand: backed down the lead-in and swung onto the taxiway. The push
	 * drives it - see FPushbackRun.
	 *
	 * NOT CALLED Pushback, and that distinction is the point of the feature rather than
	 * pedantry: a Twin Otter reverses under its own power and is not being pushed by
	 * anything. The PHASE is the manoeuvre; the SERVICE that performs it for an aeroplane
	 * which cannot manage alone is pushback, and that is the job board's business.
	 */
	Manoeuvring,

	/**
	 * A GROUND VEHICLE backing into its service bay. FReverseRun drives it.
	 *
	 * SEPARATE FROM Manoeuvring, which is an aeroplane coming off a stand. The two look alike
	 * from outside - something reversing - and are different kinematics: a pushed aeroplane
	 * pivots about its NOSE GEAR because that is where the tug couples, so the steered axle
	 * still leads. A truck backing up pivots about its FIXED axle, and can hold a tighter arc
	 * for it. Sharing a phase would mean sharing a motion law they do not share.
	 */
	Reversing,

	/** The take-off has cleared. FRoadAgent::Advance returns false from here on. */
	Gone,

	/**
	 * The route died under it (a rebuild's Strand): it stands where it stopped, at no goal, until
	 * the player retires it or a stand re-offer redirects it.
	 *
	 * NOT PARKED, and that difference is issue #396. A stranded plan counts as arrived
	 * (FRouteFollower::HasArrived: not drivable), and Parked at that moment told the ops layer -
	 * which reads GoalNode as the stand an aircraft parked on - that an aeroplane half way along a
	 * taxiway was on its stand: a truck fuelled the empty stand and the pushback started from it,
	 * a jump. APPENDED, after Gone, so no existing value moves.
	 */
	Stranded
};

/**
 * WHAT A STOPPED AGENT IS WAITING FOR, beside the phase it waits in (issue #444). Replaces two of
 * the three representations the issue found of one idea - a flag and a goal (bAwaitingStand) and a
 * flag triple (bTaxiOutStale, bTaxiOutHoldSaid, TaxiOutRefusedAt), each with its own retry loop and
 * its own UI story; the third, the Stranded phase, stays (below). ONE ENUM, so an agent waiting for
 * a stand AND for a way to the runway - which the two flags let a rebuild arm on a held departure -
 * is not a state anything can be in.
 *
 * NOT A PHASE, and that is why it is a second enum rather than three more EAgentPhase values. A
 * wait is INTENT DATA, like FDepartureOrder: a waiting aircraft is Taxiing to its route's end, or
 * Parked there, or Manoeuvring at the end of a push, and the phase goes on driving it exactly as
 * it would - the wait says only what the one retry pass (UGroundTraffic::RetryWaiters) asks for it.
 * STRANDED STAYS A PHASE: it is a motion state (nothing drives it), and a stranded aircraft can be
 * waiting for a stand as well.
 *
 * Written only through FRoadAgent::WaitFor / EndWait - the one door, which keeps each wait's
 * payload (below) and the enum together. The fields are private, but UGroundTraffic and FClaimPass
 * are FRoadAgent's friends, so the compiler alone does not hold this.
 * ENFORCED BY: Check-Architecture rule 4 ('agent wait written outside its door (rule 82)');
 * Airside.Model.RoadAgent.OneWaitAtATime
 */
UENUM()
enum class EAgentWait : uint8
{
	/** Not waiting for anything. */
	None,

	/**
	 * No stand could be found for this aircraft: it stops at the end of what remains of its route
	 * and is re-offered one whenever a stand may have freed. PAYLOAD: the node the re-offer searches
	 * from, which is the agent's GoalNode - WaitFor sets the two together.
	 */
	ForStand,

	/**
	 * Its taxi out can no longer be driven from where it is (a rebuild stranded it, or it does not
	 * begin where the push ends): it holds, and a new way to the runway is planned from where it
	 * stands. PAYLOAD: whether its hold line has been said, and the guideline revision a replan was
	 * last refused at (FRoadAgent::HasSaidWait, WasWaitRefusedAt).
	 */
	ForTaxiOutRoute
};

/**
 * WHAT EACH PHASE MEANS, in one table (issue #444). Membership used to be spelled by each consumer
 * - 27 sites in 12 files behind default: arms - and two phases were missed somewhere on arrival:
 * Reversing (2026-09-17: a backing truck was handed HoldRunwayOnly, issue #434) and Stranded
 * (2026-09-28). A consumer asks a column here; a phase added has a row or the build fails at the
 * count below.
 *
 * ONLY COLUMNS SOMETHING READS. bLeavesStand, which the review named, is not one: its one asker
 * (UJobBoard::OnAgentPhase) decides on the transition's CAUSE since #436, and is #427's to reshape.
 */
struct FAgentPhaseTraits
{
	/** Which phase this row is - the table is indexed by it, and the count below checks the order. */
	EAgentPhase Phase;

	/** A route-walking phase is driving: a taxi, a push off a stand, a vehicle backing along a span
	 *  of its route. See FRoadAgent::IsOnRoute for what each reader does with it. */
	bool bOnRoute;

	/** It may be sent along another route while it moves: a taxi, and nothing else. See
	 *  FRoadAgent::IsReplannable. */
	bool bReplannable;

	/** UGroundTraffic::RedirectAgent may restart it on a new route: it is taxiing, or standing still
	 *  where a route can begin (Parked, or Stranded). See FRoadAgent::IsRedirectable. */
	bool bRedirectable;

	/** Its BODY moves backwards along the plan it walks - a push or a back-out - so the claim pass
	 *  holds ground behind it and the route view draws its line as a reverse run. */
	bool bBodyBacks;

	/** The card's word for the phase (InspectFacts::StatusOf), before any sub-state refines it: the
	 *  ground half of a landing or take-off, a parked aircraft past its shutdown pause. */
	const TCHAR* DisplayText;
};

namespace AgentPhaseTraits
{
	/** One row per EAgentPhase, IN DECLARATION ORDER - Of() indexes by the enum's value. */
	inline constexpr FAgentPhaseTraits Rows[] = {
		//  Phase                     OnRoute Replan Redirect Backs  DisplayText
		{ EAgentPhase::Arriving,    false,  false, false,   false, TEXT("Landing roll") },
		{ EAgentPhase::Taxiing,     true,   true,  true,    false, TEXT("Taxiing") },
		{ EAgentPhase::Departing,   false,  false, false,   false, TEXT("Rolling") },
		{ EAgentPhase::Parked,      false,  false, true,    false, TEXT("Parked") },
		// The flight board's own word for this (EFlightPhase::Manoeuvring), so the card and the board agree.
		{ EAgentPhase::Manoeuvring, true,   false, false,   true,  TEXT("Manoeuvring") },
		{ EAgentPhase::Reversing,   true,   false, false,   true,  TEXT("Reversing") },
		{ EAgentPhase::Gone,        false,  false, false,   false, TEXT("Gone") },
		{ EAgentPhase::Stranded,    false,  false, true,    false, TEXT("Stranded - retire it") },
	};
	inline constexpr int32 RowCount = UE_ARRAY_COUNT(Rows);

	/**
	 * Every EAgentPhase, BY NAME, with no default - inside AIRSIDE_EXHAUSTIVE_SWITCH, so a phase
	 * added to the enum is a build error here (C4062 is off on this toolchain unless a function
	 * opts in - see ExhaustiveSwitch.h). Naming it here is what makes DeclaredCount count it, and
	 * the static_assert below then fails until Rows has its row.
	 */
	AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
	constexpr bool IsDeclared(EAgentPhase Phase)
	{
		switch (Phase)
		{
		case EAgentPhase::Arriving:
		case EAgentPhase::Taxiing:
		case EAgentPhase::Departing:
		case EAgentPhase::Parked:
		case EAgentPhase::Manoeuvring:
		case EAgentPhase::Reversing:
		case EAgentPhase::Gone:
		case EAgentPhase::Stranded:
			return true;
		}
		return false;
	}
	AIRSIDE_EXHAUSTIVE_SWITCH_END

	/** How many values of the underlying byte IsDeclared names - the enum's count, measured rather than typed. */
	constexpr int32 DeclaredCount()
	{
		int32 Count = 0;
		for (int32 Value = 0; Value <= 0xFF; ++Value)
		{
			if (IsDeclared(static_cast<EAgentPhase>(Value)))
			{
				++Count;
			}
		}
		return Count;
	}

	/** Row N describes the phase whose value is N - the order Of() reads them in. */
	constexpr bool RowsInDeclarationOrder()
	{
		for (int32 Row = 0; Row < RowCount; ++Row)
		{
			if (static_cast<int32>(Rows[Row].Phase) != Row)
			{
				return false;
			}
		}
		return true;
	}

	static_assert(RowCount == DeclaredCount(),
		"FAgentPhaseTraits: every EAgentPhase needs exactly one row in AgentPhaseTraits::Rows (issue #444)");
	static_assert(RowsInDeclarationOrder(),
		"FAgentPhaseTraits: AgentPhaseTraits::Rows must list the phases in EAgentPhase's declaration order");

	/** Phase's row. Every declared value has one - the two static_asserts above hold that. */
	constexpr const FAgentPhaseTraits& Of(EAgentPhase Phase)
	{
		return Rows[static_cast<uint8>(Phase)];
	}
}
