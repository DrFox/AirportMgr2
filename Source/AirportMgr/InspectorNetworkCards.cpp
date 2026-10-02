#include "InspectorCards.h"

#include "Model/GroundTraffic.h"
#include "Model/JobBoard.h"
#include "Model/Ledger.h"
#include "Model/OpsNames.h"
#include "Model/RoadNetwork.h"
#include "Model/RunwayFacts.h"
#include "Present/OpsRuntime.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"
#include "Solve/GuidelineGeom.h"

namespace
{
	/** The game time the current game minute began at - what the depot card describes at and keys on (see
	 *  FInspectorCardKey). Prefixed against the unity build. */
	double InspectorMinuteStart(const UOpsRuntime& Runtime)
	{
		const double Now = Runtime.GetClock() != nullptr ? Runtime.GetClock()->Now() : 0.0;
		return FMath::FloorToDouble(Now / 60.0) * 60.0;
	}

	/**
	 * The half of every network card's key they share: which card, which network OBJECT, and the two revisions every read of
	 * segments, nodes, profiles, entities and the guideline graph moves (see FInspectorCardKey). A card adds what ITS Compose reads
	 * beyond them, beside that Compose.
	 */
	FInspectorCardKey InspectorNetworkKey(const FInspectorCardInput& In)
	{
		FInspectorCardKey Key;
		Key.Kind = In.Selection.Kind;
		Key.Id = In.Selection.Id;
		const URoadNetwork* Network = In.Target->GetNetwork();
		Key.Network = Network;
		if (Network != nullptr)
		{
			Key.EditRevision = Network->GetEditRevision();
			Key.GuidelineRevision = Network->GetGuidelineRevision();
		}
		return Key;
	}

	/**
	 * A runway's or taxiway's Locate: the SELECTED SEGMENT's curve midpoint, as a Point. A Point and not a new focus kind: the
	 * segment is already selected (it is the card), so Locate only has the camera to move, which is all Point does. The curve's
	 * own evaluator (GuidelineGeom::Eval), not the chord's midpoint, so a bent taxiway centres on its pavement.
	 */
	FAlertFocus InspectorSegmentFocus(const URoadNetwork& Network, int32 SegmentIndex)
	{
		FAlertFocus Out;
		if (!Network.GetSegments().IsValidIndex(SegmentIndex))
		{
			return Out;
		}
		const FRoadSegment& Segment = Network.GetSegments()[SegmentIndex];
		const FRoadNode* A = Network.GetNode(Segment.A);
		const FRoadNode* B = Network.GetNode(Segment.B);
		if (A == nullptr || B == nullptr)
		{
			return Out;
		}
		Out.Kind = EAlertFocusKind::Point;
		Out.Id = SegmentIndex;
		Out.Point = GuidelineGeom::Eval(A->Position, Segment.Control, B->Position, 0.5);
		return Out;
	}

	/** A stand's or depot's Locate: the entity by index, which SelectAndFocus re-checks is alive at the click. */
	FAlertFocus InspectorEntityFocus(int32 EntityIndex)
	{
		FAlertFocus Out;
		Out.Kind = EAlertFocusKind::Entity;
		Out.Id = EntityIndex;
		return Out;
	}
}

const FInspectorCardView* FInspectorKeyedCard::Describe(const FInspectorCardInput& In)
{
	// THE NETWORK CARDS' GATE (ops batch 3 PR E): a runway, taxiway, stand or depot card is described again only when
	// something its Describe reads has moved - see FInspectorCardKey for each card's inputs. Keyed BEFORE the walk so an
	// unchanged card skips its Describe; the result is kept AFTER it, whole, and only once one has succeeded.
	const FInspectorCardKey Key = KeyFor(In);
	if (bValid && Key == LastKey)
	{
		// NOTHING IT READS HAS MOVED: what the last Describe said, handed back as it was.
		return &LastView;
	}
	++DescribeCalls;
	FInspectorCardView Fresh;
	Fresh.Card = Id();
	if (!Compose(In, Fresh))
	{
		return nullptr;
	}
	LastView = MoveTemp(Fresh);
	LastKey = Key;
	bValid = true;
	return &LastView;
}

// --- Runway ---------------------------------------------------------------------------------------------------------

FInspectorCardKey FRunwayCard::KeyFor(const FInspectorCardInput& In) const
{
	// A RUNWAY READS NO OCCUPANCY: its facts, strip and designators are the network's. A second ask of it - the runway in use
	// (#386's indicator) - adds its revision HERE, beside the Compose that reads it.
	return InspectorNetworkKey(In);
}

bool FRunwayCard::Compose(const FInspectorCardInput& In, FInspectorCardView& Out)
{
	const URoadNetwork* Network = In.Target->GetNetwork();
	FRunwayCardFacts R;
	if (Network == nullptr || !InspectFacts::DescribeRunway(*Network, In.Selection.Id, R))
	{
		return false;
	}
	// THE RUNWAY IN USE CARD (spec 2026-09-28-runway-in-use). Not FAircraftDisplay's gate - that
	// one is the aircraft's. It was composed every tick, the SetText gate making an
	// unchanged sentence free; since ops batch 3 PR E it is composed only when
	// FInspectorCardKey moves (FInspectorKeyedCard::Describe), since nothing on it moves otherwise.
	const FString InUse = FString::Printf(TEXT("%02d"), R.InUse);
	const FString Other = FString::Printf(TEXT("%02d"), R.Other);
	Out.Title = FString::Format(*NSLOCTEXT("AirportMgr", "InspectorRunwayTitle", "Runway {0}").ToString(), { R.Pair });
	Out.Facts = FString::Format(
		*NSLOCTEXT("AirportMgr", "InspectorRunwayFacts", "In use: {0}\nTakes: {4}\n{1}, {2} approach\n{3} m long").ToString(),
		{ InUse, FString(Pavement::Name(R.Surface)), FString(RunwayApproachName(R.Approach)),
			FString::Printf(TEXT("%.0f"), R.Length / 100.0), FString(RunwayUse::Name(R.Use)) });
	Out.Status = FString::Format(*NSLOCTEXT("AirportMgr", "InspectorRunwayStatus",
		"Landing and taking off {0}. A change reaches the next flight planned.").ToString(), { InUse });
	Out.Verbs = EInspectorVerbs::Runway | EInspectorVerbs::RunwayUse;
	Out.Locate = InspectorSegmentFocus(*Network, In.Selection.Id);
	Out.RunwayCaption = FText::Format(NSLOCTEXT("AirportMgr", "InspectorRunwayUse", "Use {0}"), FText::FromString(Other));
	// The CURRENT mode, selection.runway_use's own caption rule - see its row in BuildActions.
	Out.RunwayUseCaption = R.Use == ERunwayUse::ArrivalsOnly ? NSLOCTEXT("AirportMgr", "InspectorRunwayArrivals", "Arrivals only")
		: R.Use == ERunwayUse::DeparturesOnly ? NSLOCTEXT("AirportMgr", "InspectorRunwayDepartures", "Departures only")
		: NSLOCTEXT("AirportMgr", "InspectorRunwayMixed", "Mixed ops");
	return true;
}

// --- Taxiway --------------------------------------------------------------------------------------------------------

FInspectorCardKey FTaxiwayCard::KeyFor(const FInspectorCardInput& In) const
{
	// Segments, nodes, profiles and the stored restriction letters: the network's two revisions. A reading beyond the
	// network adds its revision HERE, beside the Compose that reads it.
	return InspectorNetworkKey(In);
}

bool FTaxiwayCard::Compose(const FInspectorCardInput& In, FInspectorCardView& Out)
{
	const URoadNetwork* Network = In.Target->GetNetwork();
	FTaxiwayCardFacts T;
	if (Network == nullptr || !InspectFacts::DescribeTaxiway(*Network, In.Selection.Id, T))
	{
		return false;
	}
	// THE TAXIWAY CARD (strip stage 6): its letter, strip and the widest span it admits -
	// every taxiway limits wingspan to its letter (user 2026-09-29) - and, restricted, what
	// restricts it (spec: "max span 65 m - restricted by building at ..."). Composed like the
	// runway card: when FInspectorCardKey moves, and the SetText gate still makes an
	// unchanged one free.
	// "TAXIWAY A" (spec): the name when there is one; an unnamed network (a raw-built fixture) keeps the segment index (plan D14).
	Out.Title = !T.Name.IsEmpty()
		? FString::Format(*NSLOCTEXT("AirportMgr", "InspectorTaxiwayNamed", "Taxiway {0}").ToString(), { T.Name })
		: FString::Format(*NSLOCTEXT("AirportMgr", "InspectorTaxiwayTitle", "Taxiway {0}").ToString(), { T.Index });
	// THE WHOLE CHAIN'S LENGTH, not this segment's: the card names the taxiway, so its figures are the taxiway's too.
	const FText ChainFormat = T.Connectors == 1
		? NSLOCTEXT("AirportMgr", "InspectorTaxiwayChainOne", "Length {0} m, {1} connector\n")
		: NSLOCTEXT("AirportMgr", "InspectorTaxiwayChain", "Length {0} m, {1} connectors\n");
	const FString Chain = T.Name.IsEmpty() ? FString() : FString::Format(*ChainFormat.ToString(),
		{ FString::Printf(TEXT("%.0f"), T.Length / 100.0), FString::FromInt(T.Connectors) });
	Out.Facts = Chain + FString::Format(
		*NSLOCTEXT("AirportMgr", "InspectorTaxiwayFacts", "Code {0}, {1} m wide, {2}\nStrip {3} m each side\nMax span {4} m").ToString(),
		{ T.Letter, FString::Printf(TEXT("%.1f"), T.Width / 100.0), FString(Pavement::Name(T.Surface)),
			FString::Printf(TEXT("%.1f"), T.Strip / 100.0), FString::Printf(TEXT("%.0f"), T.MaxWingspan / 100.0) });
	if (T.RestrictedTo.IsSet())
	{
		Out.Facts += FString::Format(*NSLOCTEXT("AirportMgr", "InspectorTaxiwayRestricted",
			"\nRestricted to Code {0} by {1} - move it clear of the strip").ToString(),
			{ T.RestrictedTo.GetValue(), T.RestrictedBy.IsEmpty() ? FString(TEXT("something in its strip")) : T.RestrictedBy });
	}
	Out.Status = T.RestrictedTo.IsSet()
		? NSLOCTEXT("AirportMgr", "InspectorTaxiwayStatusRestricted", "Restricted").ToString()
		: NSLOCTEXT("AirportMgr", "InspectorTaxiwayStatusOpen", "Open to its letter").ToString();
	Out.Locate = InspectorSegmentFocus(*Network, In.Selection.Id);
	Out.RenameTaxiwayId = T.Taxiway;
	return true;
}

// --- Stand ----------------------------------------------------------------------------------------------------------

FInspectorCardKey FStandCard::KeyFor(const FInspectorCardInput& In) const
{
	FInspectorCardKey Key = InspectorNetworkKey(In);
	// A STAND ALSO READS who holds its pose node and whether that agent is parked - see FInspectorCardKey.
	if (const UGroundTraffic* Traffic = In.Target->GetGroundTraffic())
	{
		Key.Traffic = Traffic;
		Key.OccupancyRevision = Traffic->OccupancyRevision();
		Key.StandHolds = Traffic->StandHoldChangeCount();
	}
	return Key;
}

bool FStandCard::Compose(const FInspectorCardInput& In, FInspectorCardView& Out)
{
	const URoadNetwork* Network = In.Target->GetNetwork();
	FStandFacts S;
	if (Network == nullptr || !InspectFacts::DescribeStand(In.Target->GetGroundTraffic(), *Network, In.Selection.Id, S)
		|| S.PoseRole != EServiceRole::Aircraft)
	{
		return false;
	}
	Out.Locate = InspectorEntityFocus(In.Selection.Id);
	// FString::Format, not Printf - see the aircraft card's own comment on why
	// (issue #192, UE 5.8's compile-time Printf format check).
	// THE NUMBER, not the index - the one painted at the stand's turn-off; an index is
	// recycled by the next stand placed after a delete (FEntityInstance::StandNumber).
	Out.Title = FString::Format(
		*NSLOCTEXT("AirportMgr", "InspectorStandTitle", "Stand {0}").ToString(), { S.Number });
	const FText Reachability = S.bReachable
		? NSLOCTEXT("AirportMgr", "InspectorStandReachable", "Reachable by taxiway")
		: NSLOCTEXT("AirportMgr", "InspectorStandUnreachable", "NOT reachable - no taxiway joins it");
	// SERVICE ROAD, beside Reachability (far-side-entry spec §2): a stand is placed
	// with no service road at all, so this NAMES THE FIX rather than refusing the
	// stand - the same choice Reachability itself already made for a taxiway.
	const FText ServiceRoad = S.bServiceable
		? NSLOCTEXT("AirportMgr", "InspectorStandServiceable", "Service road: joined")
		: NSLOCTEXT("AirportMgr", "InspectorStandUnserviceable",
			"Service road: not joined - draw a service road along the far edge");
	Out.Facts = FString::Format(
		*NSLOCTEXT("AirportMgr", "InspectorStandFacts", "Code {0} ({1} m span)\n{2} service anchors\n{3}\n{4}").ToString(),
		{
			S.SizeClass,
			FString::Printf(TEXT("%.0f"), S.DesignWingspan / 100.0),
			FString::FromInt(S.AnchorCount),
			Reachability.ToString(),
			ServiceRoad.ToString(),
		});
	// CLOSED BY A STRIP (strip stage 6): the reason, and the figures to fix it by.
	if (!S.ClosedBecause.IsEmpty())
	{
		Out.Facts += FString::Format(*NSLOCTEXT("AirportMgr", "InspectorStandClosed",
			"\nClosed to new arrivals: {0}").ToString(), { S.ClosedBecause });
	}
	Out.Status = S.OccupantAgent == 0
		? NSLOCTEXT("AirportMgr", "InspectorStandEmpty", "Empty").ToString()
		: S.bOccupantParked
			? FString::Format(*NSLOCTEXT("AirportMgr", "InspectorStandOccupied",
				"Occupied by aircraft #{0}").ToString(), { S.OccupantAgent })
			: FString::Format(*NSLOCTEXT("AirportMgr", "InspectorStandReserved",
				"Reserved for aircraft #{0}").ToString(), { S.OccupantAgent });
	return true;
}

// --- Depot ----------------------------------------------------------------------------------------------------------

FInspectorCardKey FDepotCard::KeyFor(const FInspectorCardInput& In) const
{
	FInspectorCardKey Key = InspectorNetworkKey(In);
	if (In.Runtime == nullptr)
	{
		return Key;
	}
	// A DEPOT reads its vehicles and their jobs, the minute, and - through the quote - the balance: see FInspectorCardKey.
	if (const UJobBoard* Board = In.Runtime->GetJobBoard())
	{
		Key.JobBoard = Board;
		Key.JobRevision = static_cast<uint32>(Board->Revision());
		Key.Minute = FMath::FloorToInt64(InspectorMinuteStart(*In.Runtime) / 60.0);
	}
	if (const ULedger* Ledger = In.Runtime->GetLedger())
	{
		Key.Ledger = Ledger;
		Key.LedgerRevision = Ledger->Revision();
	}
	return Key;
}

bool FDepotCard::Compose(const FInspectorCardInput& In, FInspectorCardView& Out)
{
	const URoadNetwork* Network = In.Target->GetNetwork();
	FStandFacts S;
	if (Network == nullptr || !InspectFacts::DescribeStand(In.Target->GetGroundTraffic(), *Network, In.Selection.Id, S)
		|| S.PoseRole == EServiceRole::Aircraft)
	{
		return false;
	}
	Out.Locate = InspectorEntityFocus(In.Selection.Id);
	// bReachable is the pose node having line on it, which for a depot means a
	// SERVICE ROAD within its lead-in reach. The message names the fix rather than
	// the symptom: the road is the thing the player goes and draws.
	// THE DEPOT BY ITS NUMBER (OpsNames::DepotLabel, #490), as the job board's vehicle lines say it - not the entity INDEX (S.Index), which a bulldoze
	// recycles into a different depot. The handle is read off the network by index, which is how DescribeStand was asked.
	const FString DepotName = OpsNames::DepotLabel(Network, Network->EntityIdAt(S.Index));
	Out.Title = FString::Format(
		*NSLOCTEXT("AirportMgr", "InspectorDepotTitle", "Fuel depot {0}").ToString(), { DepotName });
	Out.Facts = S.bReachable
		? NSLOCTEXT("AirportMgr", "InspectorDepotOnRoad", "On a service road").ToString()
		: NSLOCTEXT("AirportMgr", "InspectorDepotNotOnRoad", "Fuel depot: not on a road").ToString();
	Out.Status = S.bReachable
		? NSLOCTEXT("AirportMgr", "InspectorDepotReady", "Ready").ToString()
		: NSLOCTEXT("AirportMgr", "InspectorDepotCannotDispatch", "Cannot dispatch").ToString();

	// HOW FAR BEHIND IT IS (user, 2026-09-28): its vehicles and their jobs, and a summary
	// that replaces "Ready" - through the ops subsystem, for the fuel line's reason above:
	// the airport actor is Airside's and may not know what a job is. An off-road depot
	// keeps "Cannot dispatch", which names the fix; its backlog is empty anyway.
	// THE ONE selection->depot WALK (ruling C4) - the controller's verbs ask the same function.
	const FEntityInstanceId DepotId = ARoadBuildController::DepotForSelection(In.Target, In.Selection);
	const UOpsRuntime* Runtime = In.Runtime;
	// THE STATUS IS THE BOARD'S, "No vehicles - buy one" INCLUDED (#447): this card laid that sentence over the summary in a wording of its
	// own beside RefusalText's. OFF THE ROAD, THE ROAD IS THE FIX, so an unreachable depot keeps "Cannot dispatch" and asks the board nothing.
	if (Runtime != nullptr && S.bReachable && DepotId.IsSet())
	{
		if (const UJobBoard* Board = Runtime->GetJobBoard())
		{
			// AT THE MINUTE, not at Now: the card's key holds the game minute (FInspectorCardKey), so what it
			// shows must be a function of the minute - DescribeDepot's own rounding from Now would move a
			// figure mid-minute that the key could not see. THE NETWORK names the stands by the number on their signs.
			++BacklogCalls;
			const FDepotBacklog Backlog = Board->DescribeDepot(DepotId, InspectorMinuteStart(*Runtime), Network);
			Out.Status = Backlog.Summary;
			if (!Backlog.Detail.IsEmpty())
			{
				Out.Facts += TEXT("\n") + Backlog.Detail;
			}
		}
	}
	// THE PURCHASE ROWS, from the one quote (facility-upgrades spec §4). It WAS asked EVERY TICK, outside the
	// card's key, because the quote reads the balance and the key held none of it - although ULedger::Revision exists (#441), and
	// is in the key now. The quote reads the balance (a Buy greys when it is unaffordable), the fleet, the sheds and the bays:
	// the ledger, the job board and the network, each keyed.
	if (Runtime != nullptr && DepotId.IsSet())
	{
		++QuoteCalls;
		Out.Quote = Runtime->QuoteFacility(DepotId);
	}
	return true;
}

// --- The table ------------------------------------------------------------------------------------------------------

FInspectorCards::FInspectorCards()
{
	// ONE ROW PER CARD. The count is checked at compile time (review fix 3's rule, now over EInspectorCard): appending a card to
	// the enum moves Count and stops this compiling until the card has a row here, and Find asks by Id, so the order written
	// below cannot matter.
	IInspectorCard* const Rows[] = { &AircraftCard, &RunwayCard, &TaxiwayCard, &StandCard, &DepotCard };
	static_assert(UE_ARRAY_COUNT(Rows) == static_cast<int32>(EInspectorCard::Count) - 1,
		"a new EInspectorCard needs a card in this table - add its member to FInspectorCards and a row here");
	Table.Append(Rows, UE_ARRAY_COUNT(Rows));
}

EInspectorCard FInspectorCards::CardFor(const FSelection& Selection, const URoadNetwork* Network)
{
	// ONE CARD PER KIND, counted at compile time (review fix 3): the cases below are None (no card), Aircraft, Runway, Taxiway
	// and Stand (a stand or a depot). Appending a kind to ESelectionKind moves Count and stops this compiling until the kind
	// gets its case and this number.
	static_assert(static_cast<int32>(ESelectionKind::Count) == 5,
		"a new ESelectionKind needs an inspector card - add its case below, then update this count");
	switch (Selection.Kind)
	{
	case ESelectionKind::Aircraft: return EInspectorCard::Aircraft;
	case ESelectionKind::Runway: return EInspectorCard::Runway;
	case ESelectionKind::Taxiway: return EInspectorCard::Taxiway;
	case ESelectionKind::Stand:
		// TOLD APART BY THE ENTITY'S PoseRole, the way DescribeStand tells them (FStandFacts::PoseRole): the same selection kind,
		// two cards, each keyed on what ITS half reads.
		return Network != nullptr && Network->GetEntities().IsValidIndex(Selection.Id)
			&& Network->GetEntities()[Selection.Id].PoseRole != EServiceRole::Aircraft
			? EInspectorCard::Depot : EInspectorCard::Stand;
	case ESelectionKind::None:
	case ESelectionKind::Count:
	default:
		return EInspectorCard::None;
	}
}

IInspectorCard* FInspectorCards::Find(EInspectorCard Card) const
{
	for (IInspectorCard* Each : Table)
	{
		if (Each->Id() == Card)
		{
			return Each;
		}
	}
	return nullptr;
}

int32 FInspectorCards::NetworkDescribeCount() const
{
	return RunwayCard.DescribeCount() + TaxiwayCard.DescribeCount() + StandCard.DescribeCount() + DepotCard.DescribeCount();
}
