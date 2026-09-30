#pragma once

#include "CoreMinimal.h"
#include "Model/RoadAgent.h"
#include "Model/RoadEntity.h"
#include "Model/RunwayFacts.h"

class UGroundTraffic;
class URoadNetwork;

/**
 * WHAT an agent is held at, in the card's terms - the refused FTrafficResource's kind. Its own
 * enum rather than ETrafficResourceKind itself: the card words a Node as "a crossing" and an
 * Edge as "behind", and a panel that switched on the arbiter's enum would have to learn that
 * a holding-position node is expanded to a runway Surface before it reaches here.
 */
enum class EHoldAt : uint8
{
	None,
	/** A runway segment - the strip is occupied. */
	Runway,
	/** A guideline edge - a queue: someone is ahead on the lane. */
	Behind,
	/** A guideline node - a junction or crossing someone else holds. */
	Crossing,
};

/**
 * Why an agent is stopped, RAW: whom it waits for (an agent ID - Airside knows no
 * registrations), what it is held at and for how long. The game module names the blocker and
 * composes the sentence through InspectFacts::HoldLine, so the words have one source whichever
 * layer supplies the name.
 */
struct FAgentHold
{
	/** FRoadAgent::GetWaitingOn - 0 when nothing refuses it. */
	int32 WaitingOn = 0;
	EHoldAt At = EHoldAt::None;
	/** "09/27" when At is Runway and a network was given to name the strip; empty otherwise. */
	FString RunwayPair;
	/**
	 * FRoadAgent::GetStalledSeconds - how long it has stood waiting, in MOVEMENT seconds: real
	 * time x the speed multiplier, what agents run on (USimClock's header). NOT game time - the
	 * clock also compresses the day (~72x at the default daylight rate), and a reader showing
	 * this beside game-time figures must convert it (UInspectorWidget::GameSecondsOfStall).
	 */
	double StalledSeconds = 0.0;

	bool IsSet() const { return WaitingOn != 0; }
};

/**
 * What the inspector shows for an aircraft. PLAIN STRUCTS, not USTRUCTs: built every frame
 * for one panel, never saved, never Blueprint-bound - the Blueprint restyle binds to text
 * blocks the C++ widget fills, not to these.
 *
 * THE SEAM TO M3: the panel reads this and never FRoadAgent, so when UFlight exists it fills
 * the same struct (airline, scheduled off-block) and the panel does not change shape.
 */
struct FAgentFacts
{
	int32 Id = 0;
	/** FAirframe::TypeCode, or the traversal class when none ("Aircraft", "Vehicle"). */
	FString TypeName;
	EAgentPhase Phase = EAgentPhase::Gone;
	/** Compass degrees, 0 north, clockwise. */
	double HeadingDegrees = 0.0;
	/** uu per second; the panel formats. */
	double GroundSpeed = 0.0;
	/** uu above the surface. */
	double Altitude = 0.0;
	/** "Stand 3", "Runway 09", or "Node 41". */
	FString Destination;
	/** See InspectFacts::StatusOf for the precedence. */
	FString Status;
	bool bEngineRunning = false;
	/** Phase == Parked - the one precondition DepartAgent checks. */
	bool bCanDepart = false;

	/**
	 * What fuelling is doing for this aircraft - "truck en route", "no fuel depot" - or
	 * empty when nothing is.
	 *
	 * FILLED BY AirportOps, NOT BY DescribeAgent, which leaves it empty. Airside must never
	 * learn what a truck is FOR (see UJobBoard), so the FIELD is here - because the panel
	 * reads FAgentFacts and never FRoadAgent - and the SENTENCE comes from the layer that
	 * knows. The same seam M3's UFlight fills its airline and off-block time through.
	 */
	FString Fuel;

	/**
	 * What gets it off its stand - "reverses itself", "needs a tug" - from its airframe. Empty
	 * for a vehicle. Airside's to say: EPushbackNeed is an airframe fact, and nothing yet
	 * services it (no tug depot), so there is no progress to report.
	 */
	FString Pushback;

	/**
	 * The turnaround contract - "Turnaround 2 h - 47 min left" - or empty for an aircraft no
	 * flight owns. FILLED BY THE GAME MODULE from the flight board, for Fuel's reason: Airside
	 * does not know what a flight or a contract is. The seam this struct's header promised UFlight.
	 */
	FString Turnaround;

	/** What it is waiting for, if anything - the Unstick card's "held at what, for whom, how long". */
	FAgentHold Hold;

	/**
	 * Status IS the hold line - StatusOf's precedence chose it over "No stand" and "Departure
	 * armed" - so a layer that can name the blocker may re-say it (InspectFacts::HoldLine).
	 * A flag rather than the game module re-deriving the precedence: two copies of that order
	 * would drift the first time a status was added above the hold.
	 */
	bool bStatusIsHold = false;

	/**
	 * The OTHER members of the all-aircraft wait cycle this agent is in, empty when none. From
	 * UGroundTraffic::CurrentDeadlocks - the very list the ops Deadlock alert reads - so the card
	 * and the alert cannot disagree about whether this aircraft is deadlocked.
	 */
	TArray<int32> DeadlockedWith;
};

struct FStandFacts
{
	int32 Index = INDEX_NONE;
	/** The stand's number as the player sees it (FEntityInstance::StandNumber) - the panel's
	 *  title and the digits painted at its turn-off. NOT Index, which RoadSlot recycles. 0 for
	 *  a depot. */
	int32 Number = 0;
	/** ICAO code letter A-F from the design wingspan. */
	FString SizeClass;
	double DesignWingspan = 0.0;
	/** The agent holding this stand's pose node in the traffic occupancy table, 0 when free.
	 *  An inbound holder has RESERVED it; a parked one OCCUPIES it (bOccupantParked). Read
	 *  from the claim, never stored on the entity: an occupancy field there would be a second
	 *  source of truth traffic would have to keep in step. M3's allocator sits above this. */
	int32 OccupantAgent = 0;
	/** The occupant is Parked (else inbound: reserved). */
	bool bOccupantParked = false;
	int32 AnchorCount = 0;
	/** The pose node has at least one guideline edge - the entity's own traffic can be
	 *  routed here. For a stand that means a taxiway; for a depot, a service road. */
	bool bReachable = false;

	/**
	 * Every declared bay entry a service vehicle drives to is joined to a road - derived
	 * like bReachable, and for the same reason: a stored flag would go stale the moment the
	 * player edited the road that made it true.
	 *
	 * FALSE FOR A STAND WITH NO BAYS, deliberately not the vacuous true an empty "every"
	 * would otherwise read as - a stand cannot be serviced through anchors it does not have.
	 *
	 * DECLARED, NOT DESCRIBED, since far-side entry (2026-09-26 spec): a stand is still
	 * PLACED with no service road at all, so this is reported rather than refused, exactly
	 * as bReachable already is for a depot with no road.
	 */
	bool bServiceable = false;

	/**
	 * What this entity's pose is FOR - see FEntityInstance::PoseRole. Aircraft is a stand;
	 * anything else is a service installation, which the panel titles and describes
	 * differently.
	 *
	 * The ROLE rather than a bIsDepot flag: a flag would need a second one the day a second
	 * kind of installation arrives, and the enum already exists and already says it.
	 */
	EServiceRole PoseRole = EServiceRole::Aircraft;

	/**
	 * Why the stand is CLOSED to new arrivals, empty when it is not (strip stage 6): a taxiway's
	 * strip covers it - drawn before the strip existed, or the taxiway upgraded since. From
	 * StandAdmission::StripClosure, the rule admission refuses by, in the placement refusal's
	 * own words (StandAdmission::DescribeClosure). An aircraft already parked finishes.
	 */
	FString ClosedBecause;
};

/** What the inspector shows for a taxiway (ESelectionKind::Taxiway). Plain, like its siblings. */
struct FTaxiwayCardFacts
{
	int32 Index = INDEX_NONE;
	/** The PAVEMENT's letter, "F" - what it was built for. */
	FString Letter;
	/** Pavement width, uu. */
	double Width = 0.0;
	/** The strip each side it operates, uu: its pavement's, or the restricted letter's. */
	double Strip = 0.0;
	/** The letter it operates at when restricted ("E"); unset when it operates at Letter. */
	TOptional<FString> RestrictedTo;
	/** What restricts it, TaxiwayRestriction::Describe's words ("a service road"); empty when not. */
	FString RestrictedBy;
	/** The widest span it admits, uu - its effective letter's (every taxiway limits wingspan). */
	double MaxWingspan = 0.0;
	EPavement Surface = EPavement::Tarmac;
};

/** What the inspector shows for a runway (ESelectionKind::Runway). Plain, like its siblings. */
struct FRunwayCardFacts
{
	/** Low end first, as the strip is spoken of: "09/27". */
	FString Pair;
	/** The designator in use (FRunwayFacts::InUse, resolved: never 0) and the other end's. */
	int32 InUse = 0;
	int32 Other = 0;
	EPavement Surface = EPavement::Tarmac;
	ERunwayApproach Approach = ERunwayApproach::Visual;
	/** What traffic it takes, RESOLVED (RunwayUse::Resolve: never Unset) - the planners' reading. */
	ERunwayUse Use = ERunwayUse::Mixed;
	/** The whole strip, uu. */
	double Length = 0.0;
};

namespace InspectFacts
{
	/** False for an unknown agent id; Out untouched. Network may be null (no destination names). */
	AIRSIDE_API bool DescribeAgent(const UGroundTraffic& Traffic, const URoadNetwork* Network, int32 AgentId, FAgentFacts& Out);

	/**
	 * False for a dead or out-of-range entity index. Traffic may be null (no occupant).
	 *
	 * DESCRIBES ANY ENTITY, not only a stand, since the fuel slice: PoseRole says which, and
	 * SizeClass / DesignWingspan / OccupantAgent are meaningless for one with no design
	 * aircraft. Not renamed, because the name is reached from four call sites and a rename
	 * would buy nothing that this sentence does not.
	 */
	AIRSIDE_API bool DescribeStand(const UGroundTraffic* Traffic, const URoadNetwork& Network, int32 EntityIndex, FStandFacts& Out);

	/**
	 * One line, first match wins: No stand - waiting; Departure armed; the hold line (HoldLine,
	 * the blocker named "aircraft N" - DescribeAgent names a vehicle, the game module a flight); Crossing runway;
	 * Shutting down (Ns); Parked; On final / Landing roll; Rolling / Climbing; Taxiing.
	 * A STRING, not an enum: presentation of several orthogonal model facts, and nothing
	 * branches on it.
	 */
	AIRSIDE_API FString StatusOf(const FRoadAgent& Agent);

	/**
	 * False when SegmentIndex is not a live runway segment. The direction is the RESOLVED one
	 * (RunwayQuery::InUseEnd), so a runway saved before the field existed reads its effective
	 * end, not 0 - the card says what the planners will do, not what the struct stores.
	 */
	AIRSIDE_API bool DescribeRunway(const URoadNetwork& Network, int32 SegmentIndex, FRunwayCardFacts& Out);

	/**
	 * False when SegmentIndex is not a live taxiway (TaxiwayStrip::HasStrip). RestrictedTo reads
	 * the STORED restriction (TaxiwayRestriction::EffectiveLetterOf) - what routing uses, so the
	 * card says what the planners do; RestrictedBy re-asks RestrictionOf for the obstruction.
	 * ENFORCED BY: Airside.Model.InspectFacts.Taxiway
	 */
	AIRSIDE_API bool DescribeTaxiway(const URoadNetwork& Network, int32 SegmentIndex, FTaxiwayCardFacts& Out);

	/**
	 * What Agent waits for, raw. Network may be null: the runway pair is then left empty and
	 * HoldLine says "runway" without one.
	 */
	AIRSIDE_API FAgentHold HoldOf(const FRoadAgent& Agent, const URoadNetwork* Network);

	/**
	 * THE hold sentence - "Holding short of runway 09/27 for G-HDVK · 12 min", "Waiting behind
	 * G-HDVK · 1 h 36 min", "Waiting at crossing for G-HDVK" - with the blocker called BlockerName
	 * and " · Duration" appended when Duration is not empty. Empty when Hold is not set. The one
	 * wording StatusOf and the inspector share.
	 *
	 * DURATION IS THE CALLER'S WORDS, IN GAME TIME (ruled 2026-09-30): Hold.StalledSeconds is
	 * movement time, and only a layer with the game clock can say how much GAME time that was - so
	 * Airside, which has none, passes nothing and the inspector passes its game-time duration in
	 * the turnaround line's own format. A figure formatted here would be in the wrong unit.
	 * ENFORCED BY: Airside.Model.InspectFacts.HoldLine; AirportMgr.Inspector.HoldAndDeadlockLines
	 * (a known stall at a known day rate reads as the expected game minutes)
	 */
	AIRSIDE_API FString HoldLine(const FAgentHold& Hold, const FString& BlockerName, const FString& Duration);

	/**
	 * FAgentFacts::TypeName's rule for any agent - the airframe or vehicle type code, else the
	 * traversal class. Public so the inspector names a blocker ("FUEL #7") by the rule its own
	 * card title uses, without describing the blocker whole.
	 */
	AIRSIDE_API FString TypeNameOf(const FRoadAgent& Agent);

	/** The card's pushback words for a need. */
	AIRSIDE_API FString PushbackText(EPushbackNeed Need);
}
