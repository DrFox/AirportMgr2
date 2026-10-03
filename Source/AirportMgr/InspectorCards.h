#pragma once

#include "CoreMinimal.h"
#include "Model/FacilityPurchases.h"
#include "Model/FuelSupply.h"
#include "Model/InspectFacts.h"
#include "Model/OpsAlerts.h"
#include "Tool/Selection.h"
#include "UObject/WeakObjectPtr.h"

class ARoadNetworkActor;
class UFlight;
class UFlightBoard;
class UGroundTraffic;
class UJobBoard;
class ULedger;
class UOpsRuntime;
class UPricing;
class URoadNetwork;
class USimClock;

/**
 * THE INSPECTOR'S CARDS (issue #441), one per thing it can describe - a STRATEGY per card, where UInspectorWidget used to be
 * one 600-line Refresh with five memo keys and each card's outputs saved and restored by hand.
 *
 * Not ESelectionKind: a Stand selection is two cards, FStandCard or FDepotCard, told apart by the entity's PoseRole
 * (FInspectorCards::CardFor). The table in FInspectorCards asks for one card of EACH of these, and its static_assert stops a
 * value appended here without a card from compiling. None is "no card" - what CardFor says of a selection nothing can
 * describe - never a value a card reports.
 */
enum class EInspectorCard : uint8
{
	None,
	Aircraft,
	Runway,
	Taxiway,
	Stand,
	Depot,
	/** NOT a card - how many there are. Stays last. */
	Count
};

/**
 * WHICH VERBS a card offers - the widget shows exactly these buttons and collapses the rest, so per-kind button visibility is
 * the card's to say, not a branch in the widget that every new card has to learn about. A bit set, not an enum, because a card
 * offers several at once (an aircraft: Depart, Follow and Unstick, and Show while it waits).
 */
enum class EInspectorVerbs : uint8
{
	None = 0,
	Depart = 1 << 0,
	Follow = 1 << 1,
	Unstick = 1 << 2,
	/** "Show G-HDVK" - selects the agent this one waits for. */
	WaitingFor = 1 << 3,
	/** The runway card's "Use 27" (selection.runway_in_use). */
	Runway = 1 << 4,
	/** The runway's mode, beside the flip (selection.runway_use). */
	RunwayUse = 1 << 5,
};
ENUM_CLASS_FLAGS(EInspectorVerbs)

/**
 * THE DEPOT CARD'S FUEL ROW, worded (2026-10-03): the stock line, and the three fuel buttons' captions, reasons and tooltips - all
 * composed from ONE FFuelQuote by FDepotCard::FuelViewOf, so the line and the buttons under it cannot disagree, and the BuildActions
 * rows behind the buttons gate on the same quote's refusals. Default (bShown false) collapses the row: every card but a depot's.
 */
struct FDepotFuelView
{
	bool bShown = false;
	/** "Fuel 12,000 / 30,000 L · contract 5,000 L/day, 6 days" - the figures at the quote's own rounding. */
	FString Line;
	/** The verdicts, None when the verb may run; the rows grey a button on anything else, with the reason on its caption. */
	EFuelOrderRefusal Spot = EFuelOrderRefusal::None;
	EFuelOrderRefusal Sign = EFuelOrderRefusal::None;
	EFuelOrderRefusal Cancel = EFuelOrderRefusal::None;
	/** Captions as shown - a refused one carries its reason ("Order 10,000 L - No room in the tanks - buy a tank"). */
	FText SpotCaption;
	FText SignCaption;
	FText CancelCaption;
	/** What a click would do, in figures: the spot price and delay, the contract's daily cost and term, the cancel charge. */
	FText SpotTip;
	FText SignTip;
	FText CancelTip;
	/** A contract runs: the contract button UPGRADES to the next tier (ruled 2026-10-03) and Cancel is offered beside it. With none,
	 *  the button signs tier 0 and Cancel is not shown - a greyed "Cancel - No contract" beside every uncontracted depot taught nothing. */
	bool bContracted = false;
};

/**
 * WHAT A CARD SAYS: the whole of it, in one value. The widget paints this and nothing else, so the outputs of a card cannot be
 * saved and restored field by field: the card keeps its last view as one object (FInspectorKeyedCard) and hands it out whole,
 * which is what made "a missed restore field shows the previous card's caption" unrepresentable.
 *
 * Defaulted for a card that has nothing to say in a field - a runway has no deadlock line, a stand no verbs.
 */
struct FInspectorCardView
{
	EInspectorCard Card = EInspectorCard::None;
	FString Title;
	FString Facts;
	FString Status;
	/** "Deadlocked with G-HDVK - ..." - empty while the line is collapsed. */
	FString Deadlock;
	EInspectorVerbs Verbs = EInspectorVerbs::None;
	/** Depart's lit state: the aircraft is parked (FAgentFacts::bCanDepart). False for every other card. */
	bool bCanDepart = false;
	/** The agent "Show" selects, 0 for none - and its caption, meaningful while Verbs has WaitingFor. */
	int32 WaitedForId = 0;
	FText WaitingForCaption;
	/** The runway card's two captions: "Use 27", and the CURRENT mode ("Mixed ops"). */
	FText RunwayCaption;
	FText RunwayUseCaption;
	/**
	 * A DEPOT'S purchase quote, laid over the card by the purchase rows - default (NotAFacility) for every other card, which
	 * collapses them. Part of the view, not asked beside it, so the status line "No vehicles - buy one" and the rows under it
	 * are one answer from one key and cannot disagree (spec §4: the card cannot disagree with the rules).
	 */
	FFacilityQuote Quote;
	/** A DEPOT'S fuel row (FDepotFuelView), composed in the same Describe as the quote, under the same key - default collapses it. */
	FDepotFuelView Fuel;
	/**
	 * WHERE THE SUBJECT IS, for the Locate button (2026-10-02, every card): the alert Go's own FAlertFocus, so Locate goes through
	 * ARoadBuildController::SelectAndFocus like Go and Show - an agent by id (found where it is NOW, at the click), a stand or depot
	 * by entity index, a runway or taxiway as a Point. None collapses the button. Filled by each card's Compose, so a new card says
	 * where its subject is or has no Locate - the widget learns no kinds.
	 * ENFORCED BY: AirportMgr.Inspector.EveryCardLocatesItsSubject
	 */
	FAlertFocus Locate;
};

/**
 * What a card is asked against: the selection and everything a card reads that is not the model it describes. The widget
 * builds one per tick; a test builds one by hand.
 */
struct FInspectorCardInput
{
	/** The ops runtime: the world's (OpsRuntimeResolver::Resolve); null for none (a headless test's world has no game instance). */
	const UOpsRuntime* Runtime = nullptr;
	/** Never null - the widget hides before asking when there is no airport. */
	const ARoadNetworkActor* Target = nullptr;
	FSelection Selection;
	/**
	 * The aircraft's facts when the caller already asked for them: the controller computes them for the bar's Depart row the
	 * same frame (issue #187), so the aircraft card does not call InspectFacts::DescribeAgent a second time. Null asks for itself.
	 */
	const FAgentFacts* PrecomputedAgentFacts = nullptr;
	/** The registrations' flight board and the game clock the hold duration converts by - the widget's test seams
	 *  (UseFlightBoardForTest, UseClockForTest) already resolved against Runtime. */
	const UFlightBoard* Flights = nullptr;
	const USimClock* Clock = nullptr;
};

/**
 * ONE CARD: the STRATEGY UInspectorWidget asks for the view of a selection. An interface with state behind it (a card keeps
 * what it last described from), which is why it is objects and not a table of free functions - the codebase's IBuildTool is the
 * same shape, and for the same reason.
 */
class IInspectorCard
{
public:
	virtual ~IInspectorCard() = default;

	virtual EInspectorCard Id() const = 0;

	/**
	 * The card's view of In's selection, or null when there is nothing to show (the thing is gone, or its facts cannot be
	 * described). The pointer is the card's OWN memo - valid until this card's next Describe: the widget paints it and lets go,
	 * so a depot's quote is not copied every tick.
	 */
	virtual const FInspectorCardView* Describe(const FInspectorCardInput& In) = 0;
};

/**
 * What a NETWORK card - runway, taxiway, stand, depot - was last described from (ops batch 3 PR E). Its Describe walked the
 * network every tick; now it runs when one of these moves, and the card is repainted from what it said. Each card fills its
 * OWN half in its own KeyFor, beside the Compose that reads those inputs; a field a card does not read stays zero.
 *
 * EVERY INPUT, BY CARD (checked against InspectFacts.cpp and UJobBoard::DescribeDepot, 2026-09-30):
 *  - all four read segments, nodes and profiles (EditRevision - every node, segment and profile mutator bumps it,
 *    a drag included) and runway facts, entities, the stored restriction letters and the guideline graph. In play
 *    those four change only through the facade, whose Topology notify rebuilds the guideline graph - every derived
 *    edge removed and re-made - so GuidelineRevision moves for each. The network OBJECT too: a clear or a load is a
 *    new one, counting from its own zero - held as a weak pointer, whose serial a recycled address cannot match.
 *  - a STAND also reads who holds its pose node and whether that agent is parked: OccupancyRevision (goal claims,
 *    holds, every phase change) and UGroundTraffic::StandHoldChangeCount (a body on or off the pose, which moves no
 *    revision). Runway and taxiway cards read no occupancy, so they leave these zero and a moving airport does not
 *    recompose them.
 *  - a DEPOT reads its vehicles and their jobs (UJobBoard::Revision) and the clock, which the card reads AT THE
 *    MINUTE (Now floored to 60 s) so its text is a function of Minute exactly: DescribeDepot rounds its minutes
 *    from Now, and a key on the minute alone would have held a figure that moved mid-minute. "No vehicles - buy one" is
 *    the board's own status (DescribeDepot's Summary), so it moves with that revision. Its PURCHASE ROWS
 *    (UOpsRuntime::QuoteFacility - the balance, the fleet, the sheds) are in the same view and so
 *    under the same key: the balance moves no revision of the job board or the network, but ULedger::Revision
 *    (every Post, every RollUp, every load) is the one that does, and the quote was being asked EVERY TICK for want of
 *    keying on it (#441).
 * ENFORCED BY: AirportMgr.Inspector.Cache.* - one test per revision and the minute, each red when its field is left
 * out of == (2026-09-30); AirportMgr.Inspector.CardMemoMatchesFreshDescribe (a fixed-seed walk over every model input, each
 * card against a fresh one); Check-Architecture rule 35 (facts-through-facade) for "only through the facade". The object
 * identities are not pinned here: no card test holds every revision equal across two objects (the Land key's and
 * the held taxi out's are, AirportMgr.UI.LandChoicesKeyNamesTheNetwork and Airside.Model.Traffic.HeldTaxiOut.ANewNetworkAsksAgain).
 */
struct FInspectorCardKey
{
	ESelectionKind Kind = ESelectionKind::None;
	int32 Id = INDEX_NONE;
	/** Identity only, never dereferenced: FWeakObjectPtr compares index AND serial, so a new object at a freed
	 *  address is not the old one (a raw pointer could be), and it needs no complete type in this header. */
	FWeakObjectPtr Network;
	uint32 EditRevision = 0;
	uint32 GuidelineRevision = 0;
	/** A stand's only - see the struct comment. */
	FWeakObjectPtr Traffic;
	uint32 OccupancyRevision = 0;
	uint32 StandHolds = 0;
	/** A depot's only - see the struct comment. */
	FWeakObjectPtr JobBoard;
	uint32 JobRevision = 0;
	int64 Minute = 0;
	FWeakObjectPtr Ledger;
	int32 LedgerRevision = 0;
	/** A depot's only: THE FUEL QUOTE the row is worded from, at the litres the line prints (FDepotCard::ShownFuel) - so a tanker
	 *  arriving or a bowser drawing moves it with no ledger post, and pumping recomposes once per printed step, not per tick. */
	FFuelQuote Fuel;

	/** DEFAULTED: a field added above is compared without anyone remembering to add it to a hand-written list. */
	bool operator==(const FInspectorCardKey& Other) const = default;
};

/**
 * A NETWORK CARD: Describe is the gate, KeyFor and Compose are what a card fills in - a TEMPLATE METHOD, the shape
 * UAirportMgrPanelWidget::Initialize has, so the "compare the key, reuse or describe, remember whole" steps exist once and
 * a fourth card cannot forget one. The memo is the whole view, so nothing is saved or restored per field.
 *
 * A FAILED Compose returns null and leaves the last good memo: its key cannot match the key that failed (it differed, or
 * Compose would not have been asked), and a later state that does match it is one the memo describes exactly.
 */
class FInspectorKeyedCard : public IInspectorCard
{
public:
	virtual const FInspectorCardView* Describe(const FInspectorCardInput& In) override final;

	/** How many times Compose ran - the gate's counter: a quiet frame adds nothing. Failed attempts count. */
	int32 DescribeCount() const { return DescribeCalls; }

protected:
	/** EVERY INPUT Compose reads, at the finest rounding its text prints - see FInspectorCardKey. Cheap: no model walk. */
	virtual FInspectorCardKey KeyFor(const FInspectorCardInput& In) const = 0;
	/** The walk: describes In's selection into Out (whose Card is already set). False: nothing to show. */
	virtual bool Compose(const FInspectorCardInput& In, FInspectorCardView& Out) = 0;

private:
	FInspectorCardKey LastKey;
	FInspectorCardView LastView;
	bool bValid = false;
	int32 DescribeCalls = 0;
};

/** The runway card (spec 2026-09-28-runway-in-use): its designators, what it takes, and the two verbs that change them. */
class FRunwayCard final : public FInspectorKeyedCard
{
public:
	virtual EInspectorCard Id() const override { return EInspectorCard::Runway; }

protected:
	virtual FInspectorCardKey KeyFor(const FInspectorCardInput& In) const override;
	virtual bool Compose(const FInspectorCardInput& In, FInspectorCardView& Out) override;
};

/** The taxiway card (strip stage 6): its letter, strip and the widest span it admits - and what restricts it. */
class FTaxiwayCard final : public FInspectorKeyedCard
{
public:
	virtual EInspectorCard Id() const override { return EInspectorCard::Taxiway; }

protected:
	virtual FInspectorCardKey KeyFor(const FInspectorCardInput& In) const override;
	virtual bool Compose(const FInspectorCardInput& In, FInspectorCardView& Out) override;
};

/** The stand card: its number, code, reachability, the strip that closes it and who occupies it. */
class FStandCard final : public FInspectorKeyedCard
{
public:
	virtual EInspectorCard Id() const override { return EInspectorCard::Stand; }

protected:
	virtual FInspectorCardKey KeyFor(const FInspectorCardInput& In) const override;
	virtual bool Compose(const FInspectorCardInput& In, FInspectorCardView& Out) override;
};

/**
 * The depot card: how far behind its vehicles are, and the purchase quote the rows under it render.
 *
 * ENTITY, NOT ALWAYS A STAND, since the fuel slice: InspectFacts::DescribeStand describes any entity and PoseRole tells the two
 * apart (see FStandFacts::PoseRole) - the depot is the half of the old stand branch whose pose is not an aircraft's.
 */
class FDepotCard final : public FInspectorKeyedCard
{
public:
	virtual EInspectorCard Id() const override { return EInspectorCard::Depot; }

	/** How many times the card asked UJobBoard::DescribeDepot - at most once a game minute on a quiet board. */
	int32 BacklogCount() const { return BacklogCalls; }
	/** How many times it asked UOpsRuntime::QuoteFacility - once per key, not once per tick (#441). */
	int32 QuoteCount() const { return QuoteCalls; }

	/**
	 * The supply's quote, its litres ROUNDED TO WHAT THE LINE PRINTS (the nearest 100 L), with the card's spot order on offer
	 * (OpsDesignDefaults::SpotOrderLitres, or the whole 100 L that fit - UFuelSupply::SpotOfferOf, so it needs no rounding here). THE ONE ROUNDING, read by the key and by the composition, so two quotes that print the
	 * same line are one key. The refusals are the supply's own, unrounded.
	 */
	static FFuelQuote ShownFuel(const UFuelSupply& Supply);

	/** The fuel row's words from Q - see FDepotFuelView. Pricing words the money; null prints bare figures (a headless test). */
	static FDepotFuelView FuelViewOf(const FFuelQuote& Q, const UPricing* Pricing);

protected:
	virtual FInspectorCardKey KeyFor(const FInspectorCardInput& In) const override;
	virtual bool Compose(const FInspectorCardInput& In, FInspectorCardView& Out) override;

private:
	int32 BacklogCalls = 0;
	int32 QuoteCalls = 0;
};

/**
 * The names the aircraft card's title, hold and deadlock lines print (2026-09-30) - the registration and airline off the
 * flight board, the blocker's and ring partners' names. Looked up BEFORE the gate and composed behind it. The stall clock is
 * Waited, below.
 */
struct FAircraftNames
{
	FString Registration;
	FString Airline;
	/**
	 * The selected agent's SERVICE VEHICLE, named as the depot card names it - "Bowser #7": its kind's name (FServiceFleet::NameOf) and
	 * its VEHICLE id, not the type code and the AGENT id ("FUEL  #37") the title printed before #478. Empty for an agent no vehicle owns.
	 * Looked up with the registration, before the gate: it is a display field, and the job board's fleet is what answers it.
	 */
	FString Vehicle;
	FString Blocker;
	FString Partners;
	/** The hold's game-time duration as printed ("12 min") - it moves while Phase, since the hold line left Airside without a
	 *  figure, does not. */
	FString Waited;
};

/**
 * EVERYTHING the aircraft card's Title, Facts, Status, Deadlock and verbs are composed from, as the figures they print - and,
 * compared whole, what decides whether they are composed again. ONE struct for both (#441): FInspectorKey used to be a key
 * beside a composition that read the raw facts, two lists that had to agree by hand, and they did not.
 *
 * RAW FACTS NEVER REACH THE SENTENCE: FAircraftCard::Compose reads nothing but this, and the == is defaulted, so a figure the
 * sentence prints cannot be left out of the key and one rounding feeds both - the two rounded figures cannot part company.
 * Two measured cases of them parting, both gone with the struct: speed keyed in tenths of m/s while the sentence also prints
 * whole knots, whose boundaries do not line up with the tenths' (25.5 and 26 uu/s are one tenth and two knots); and heading and
 * altitude keyed by FMath::RoundToInt while the sentence printed them with printf's round-half-even (an exact half, 250 uu,
 * said "2" on a key that said 3). ENFORCED BY: AirportMgr.Inspector.KeyEqualMeansTextEqual (a fixed-seed walk that snaps to
 * exactly those halves).
 *
 * ROUNDED, not raw - each to the FINEST precision the composed sentence shows for that quantity, so two facts that would
 * compose to the IDENTICAL string never miss this cheaper equality check first (issue #309: Refresh ran every Printf and
 * FString::Format of the aircraft branch - four Printfs and two NSLOCTEXT lookups - EVERY TICK regardless of whether the facts
 * had moved, and only gated the resulting SetText, so a parked aircraft awaiting dispatch rebuilt the same three sentences a
 * tick for a SetText that then did nothing).
 *
 * SpeedTenths is TENTHS OF m/s AND SpeedKnots is whole knots, because the sentence prints BOTH from the same Shown value: m/s to
 * one decimal place and knots to zero, and 1 kt is 0.514 m/s - finer than a whole knot - so keying on the coarser knots figure
 * alone left a real change (3.4 -> 3.5 m/s, same 7 kt) uncomposed and the m/s line stale on screen. PR #329 review caught
 * this: whichever of a quantity's several displayed roundings is FINEST is the one the key must use - and #441 found the
 * finest is not enough when the boundaries differ, so the key now holds every rounding the sentence prints.
 *
 * Status HOLDS F.Status (the exact string the Status line prints), not F.Phase (the enum): phase ALONE (Taxiing, Rolling) sits
 * still for many seconds while heading/speed/altitude keep moving, so gating on the enum would freeze this struct's equality
 * while the sentence it stands for kept changing underneath it.
 *
 * AIRCRAFT ONLY, not the stand/depot cards: issue #309's evidence cites the aircraft composition, which composes far more per
 * call; the network cards are gated by FInspectorCardKey, a revision compare, because their cost is the model walk.
 */
struct FAircraftDisplay
{
	int32 Id = INDEX_NONE;
	FString TypeName;
	/** The exact string the Status line prints - or, with bStatusIsHold, the hold line's precedence marker. */
	FString Status;
	/** Status IS the hold line (FAgentFacts::bStatusIsHold): the card re-says it with a name (InspectFacts::HoldLine). */
	bool bStatusIsHold = false;
	/** The hold's kind and runway pair - HoldLine's words - and whom it waits for (0 for nobody). */
	EHoldAt HoldAt = EHoldAt::None;
	FString HoldRunwayPair;
	int32 WaitedForId = 0;
	/** Whole degrees, as printed ("%03d"). */
	int32 HeadingDegrees = 0;
	/** Tenths of m/s of the speed's MAGNITUDE (FAgentMotion::GroundSpeed is signed since 2026-09-20 so the view can roll a
	 *  reversing vehicle's wheels backwards; a pushback reading "-1.5 m/s" reads as a fault - the Status line says which way). */
	int32 SpeedTenths = 0;
	int32 SpeedKnots = 0;
	/** Tenths of m/s and whole ft/min of the climb, SIGNED unlike speed: a descent is not a fault, it is the other half of the
	 *  answer. ft/min because that is what a VSI reads. Both roundings held, for SpeedKnots' reason above. */
	int32 VerticalTenths = 0;
	int32 VerticalFpm = 0;
	int32 AltitudeMetres = 0;
	FString Destination;
	bool bEngineRunning = false;
	FString Fuel;
	FString Pushback;
	FString Turnaround;
	FString Registration;
	FString Airline;
	FString Vehicle;
	FString Blocker;
	FString Partners;
	FString Waited;
	/** Depart's lit state. In the key so the verb is part of the view it is gated with. */
	bool bCanDepart = false;

	bool operator==(const FAircraftDisplay& Other) const = default;
};

/**
 * The turnaround sentence, kept until what it prints moves (ops batch 3 PR E): DescribeTurnaround reads the flight's contract and
 * Now, which it shows only as whole minutes left or late - so it is keyed on exactly those, computed from the flight each tick
 * for two subtractions instead of an FText::Format. The minutes are GameTimeText::WholeMinutes of Abs(Left) - the very function
 * Duration calls to round (#446; the key rounded inline, which was a coupling across two files) - and
 * AirportMgr.Inspector.TurnaroundKeyMatchesItsSentence still walks Now across a whole day asking that two times with one key say
 * one sentence: a rounding changed in Duration goes red there, not stale on screen.
 */
struct FInspectorTurnaround
{
	/** Flight, contract, late?, whole minutes shown - DescribeTurnaround's every input at the resolution it prints. */
	TWeakObjectPtr<const UFlight> Flight;
	double Contract = -1.0;
	bool bLate = false;
	int32 Minutes = -1;
	FString Line;

	/** The sentence as of Now. True when it was composed again. */
	bool Refresh(const UFlight& Of, double Now);
};

/**
 * THE AIRCRAFT CARD - an aircraft, or a service vehicle's agent: what it is doing, what it wants (the Demands block), whom it
 * waits for and whether it is in a ring. Stateful in three ways, each kept until what it reads moves (ops batch 3 PR E; they
 * ran every tick, before FAircraftDisplay could be compared, because their answers are fields of it):
 *  - the flight: UFlightBoard::FlightForAgent reads the board's agent index, which moves with Revision.
 *  - the fuel line: UJobBoard::DescribeAgent reads the job board (Revision) and, while pumping only, the clock - which it says
 *    (bFuelLineLive), and then it is asked every tick, as before.
 *  - the turnaround: FInspectorTurnaround.
 * The spec keyed them on "flight board revision + agent phase"; the phase is no input of any of the three (a phase change
 * reaches the board as a Revision), while the job board and the clock are.
 * WEAK, all three pointers: a load or a level change can take either board or the flight away between ticks.
 */
class FAircraftCard final : public IInspectorCard
{
public:
	virtual EInspectorCard Id() const override { return EInspectorCard::Aircraft; }
	virtual const FInspectorCardView* Describe(const FInspectorCardInput& In) override;

	/**
	 * The display figures of F, with its names: the ONE place a raw fact becomes a figure the sentence prints. Public with
	 * Compose, so a test can put two facts through both without a widget.
	 */
	static FAircraftDisplay DisplayOf(const FAgentFacts& F, const FAircraftNames& Names);

	/** The sentences and verbs of D, reading nothing else - see FAircraftDisplay. */
	static void Compose(const FAircraftDisplay& D, FInspectorCardView& Out);

	/**
	 * How the card names another agent: its flight's registration, else its service vehicle as the depot card names it ("Bowser #7" -
	 * the kind's name and the VEHICLE id, FServiceFleet::NameOf; #478), else "<type> #<id>" (the title's own fallback for what no
	 * vehicle owns), else "aircraft <id>" for one already gone. Jobs is the job board that knows the vehicles; null asks none.
	 * ENFORCED BY: AirportMgr.Inspector.Card.ServiceVehicleTitleNamesTheVehicle
	 */
	static FString NameOfAgent(const UFlightBoard* Flights, const UGroundTraffic* Traffic, const UJobBoard* Jobs, int32 AgentId);

	/**
	 * A service vehicle's name - "Bowser #7" - as the card titles it and other cards name it: its kind's name and its vehicle id. Empty
	 * when Jobs is null or no vehicle of it has AgentId. THE ONE COMPOSITION, so the title and a hold line naming the same vehicle
	 * cannot differ.
	 */
	static FString NameOfVehicle(const UJobBoard* Jobs, int32 AgentId);

	/** How many times the sentences were composed, as opposed to asked for (issue #309's gate, FAircraftDisplay). */
	int32 ComposeCount() const { return ComposeCalls; }
	/** How many times the card looked its flight up (UFlightBoard::FlightForAgent), asked the job board for its fuel line, and
	 *  composed its turnaround sentence - the three lookups PR E keys. */
	int32 FlightLookupCount() const { return FlightLookups; }
	int32 FuelLookupCount() const { return FuelLookups; }
	int32 TurnaroundComposeCount() const { return TurnaroundComposes; }

private:
	const UFlight* LookUpFlight(const UFlightBoard* Board, int32 AgentId);

	/** What Describe composed last time the display changed - reused verbatim on an unchanged tick rather than recomposed,
	 *  which is the entire saving ComposeCount measures - and the display it was composed from, compared BEFORE any Printf or
	 *  FString::Format work, not after: this is the gate ONE STEP EARLIER than the widget's SetText gate. */
	FInspectorCardView View;
	FAircraftDisplay LastDisplay;
	bool bComposed = false;
	int32 ComposeCalls = 0;

	TWeakObjectPtr<const UFlightBoard> FlightLookupBoard;
	uint32 FlightLookupRevision = 0;
	int32 FlightLookupAgent = INDEX_NONE;
	TWeakObjectPtr<const UFlight> FlightLookup;

	TWeakObjectPtr<const UJobBoard> FuelLineBoard;
	uint32 FuelLineRevision = 0;
	int32 FuelLineAgent = INDEX_NONE;
	bool bFuelLineLive = false;
	FString FuelLine;

	FInspectorTurnaround Turnaround;

	int32 FlightLookups = 0;
	int32 FuelLookups = 0;
	int32 TurnaroundComposes = 0;
};

/**
 * THE TABLE: one card of each EInspectorCard, and which one a selection is. Owned by the widget, so every panel keeps its own
 * memos (two panels showing two selections must not evict each other's).
 *
 * The cards are members and the table lists them: Find asks by Id, not by position, so the order the rows are written in cannot
 * matter - the rule CLAUDE.md gives for lists that must agree.
 */
class FInspectorCards
{
public:
	FInspectorCards();
	/** Its members are pointed at by the table: a copy would point at the original. */
	FInspectorCards(const FInspectorCards&) = delete;
	FInspectorCards& operator=(const FInspectorCards&) = delete;

	/**
	 * The card a selection is shown by; None for a kind with no card. Network is read only for a Stand selection, to tell a
	 * stand from a depot by the entity's PoseRole - null reads as a stand, which then finds nothing to describe.
	 */
	static EInspectorCard CardFor(const FSelection& Selection, const URoadNetwork* Network);

	/** The card for Card; null for None or a value no row names. */
	IInspectorCard* Find(EInspectorCard Card) const;

	FAircraftCard& Aircraft() { return AircraftCard; }
	const FAircraftCard& Aircraft() const { return AircraftCard; }
	FRunwayCard& Runway() { return RunwayCard; }
	FTaxiwayCard& Taxiway() { return TaxiwayCard; }
	FStandCard& Stand() { return StandCard; }
	FDepotCard& Depot() { return DepotCard; }
	const FDepotCard& Depot() const { return DepotCard; }

	/** The four network cards' describes summed - what FInspectorCardKey's gate keeps flat on a quiet frame. */
	int32 NetworkDescribeCount() const;

private:
	FAircraftCard AircraftCard;
	FRunwayCard RunwayCard;
	FTaxiwayCard TaxiwayCard;
	FStandCard StandCard;
	FDepotCard DepotCard;
	TArray<IInspectorCard*, TInlineAllocator<8>> Table;
};
