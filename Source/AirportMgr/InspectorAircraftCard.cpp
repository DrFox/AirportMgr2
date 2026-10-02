#include "InspectorCards.h"

#include "ArrivalViewModels.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/JobBoard.h"
#include "Model/OpsAlerts.h"
#include "Model/RoadAgent.h"
#include "Model/GameTimeText.h"
#include "Model/SimClock.h"
#include "OfferViewModels.h"
#include "Present/OpsRuntime.h"
#include "Present/RoadNetworkActor.h"

bool FInspectorTurnaround::Refresh(const UFlight& Of, double Now)
{
	// WHAT THE SENTENCE PRINTS, computed for two subtractions: the contract, and the whole
	// minutes left or late - GameTimeText::Duration's own rounding of the flight's ContractSecondsLeft.
	const double Left = Of.ContractSecondsLeft(Now);
	const bool bNowLate = Of.IsLate(Now);
	const int32 NowMinutes = GameTimeText::WholeMinutes(FMath::Abs(Left));
	if (Flight.Get() == &Of && Contract == Of.ContractSeconds && bLate == bNowLate && Minutes == NowMinutes)
	{
		return false;
	}
	Line = UArrivalRowViewModel::DescribeTurnaround(Of, Now).ToString();
	Flight = &Of;
	Contract = Of.ContractSeconds;
	bLate = bNowLate;
	Minutes = NowMinutes;
	return true;
}

FString FAircraftCard::NameOfVehicle(const UJobBoard* Jobs, int32 AgentId)
{
	const FServiceVehicle* Vehicle = Jobs != nullptr ? Jobs->VehicleForAgent(AgentId) : nullptr;
	if (Vehicle == nullptr)
	{
		return FString();
	}
	// THE KIND'S NAME AND THE VEHICLE'S OWN ID - what the depot card's fleet rows and the vehicle's fuel line say - not the type code
	// and the agent id the title printed ("FUEL  #37") before #478. FServiceFleet::NameOf is the one resolver of the name (#430).
	return FString::Printf(TEXT("%s #%d"), *FServiceFleet::NameOf(*Jobs, Vehicle->TypeCode).ToString(), Vehicle->Id);
}

FString FAircraftCard::NameOfAgent(const UFlightBoard* Flights, const UGroundTraffic* Traffic, const UJobBoard* Jobs, int32 AgentId)
{
	if (Flights != nullptr)
	{
		if (const UFlight* Flight = Flights->FlightForAgent(AgentId); Flight != nullptr && !Flight->Callsign.IsEmpty())
		{
			return Flight->Callsign;
		}
	}
	// A SERVICE VEHICLE, before the type-and-agent-id fallback: the fallback is for what no vehicle owns, and a bowser that is
	// named "FUEL #37" on a hold line while its own card says "Bowser #2" is two names for one thing.
	if (const FString Vehicle = NameOfVehicle(Jobs, AgentId); !Vehicle.IsEmpty())
	{
		return Vehicle;
	}
	if (const FRoadAgent* Agent = Traffic != nullptr ? Traffic->FindAgent(AgentId) : nullptr)
	{
		return FString::Printf(TEXT("%s #%d"), *InspectFacts::TypeNameOf(*Agent), AgentId);
	}
	return FString::Printf(TEXT("aircraft %d"), AgentId);
}

const UFlight* FAircraftCard::LookUpFlight(const UFlightBoard* Board, int32 AgentId)
{
	if (Board != nullptr && (FlightLookupBoard.Get() != Board || FlightLookupRevision != Board->Revision()
		|| FlightLookupAgent != AgentId))
	{
		++FlightLookups;
		FlightLookup = Board->FlightForAgent(AgentId);
		FlightLookupBoard = Board;
		FlightLookupRevision = Board->Revision();
		FlightLookupAgent = AgentId;
	}
	return Board != nullptr ? FlightLookup.Get() : nullptr;
}

FAircraftDisplay FAircraftCard::DisplayOf(const FAgentFacts& F, const FAircraftNames& Names)
{
	// MAGNITUDE. FAgentMotion::GroundSpeed became signed on 2026-09-20 so the view could
	// roll a reversing vehicle's wheels backwards, and a readout is not that view: an
	// aircraft on a pushback would otherwise report "-1.5 m/s (-3 kt)", which reads as a
	// fault rather than as a direction. Which way it is going is the Status line's job.
	const double Shown = FMath::Abs(F.GroundSpeed);

	// THE GATE, ONE LEVEL EARLIER THAN the SetText gate (issue #309): every figure is rounded to what the sentence PRINTS, so
	// two facts that would compose to an identical sentence are one display and never miss this cheaper check first. See
	// FAircraftDisplay for why Status holds F.Status rather than F.Phase, and why speed is held as tenths of m/s AND whole
	// knots (PR #329 review: a change that moves the m/s decimal without moving the rounded knot integer was composing
	// nothing, leaving the m/s line stale).
	FAircraftDisplay D;
	D.Id = F.Id;
	D.TypeName = F.TypeName;
	D.Status = F.Status;
	D.bStatusIsHold = F.bStatusIsHold;
	D.HoldAt = F.Hold.At;
	D.HoldRunwayPair = F.Hold.RunwayPair;
	D.WaitedForId = F.Hold.WaitingOn;
	D.HeadingDegrees = FMath::RoundToInt(F.HeadingDegrees);
	D.SpeedTenths = FMath::RoundToInt(Shown / 10.0);
	D.SpeedKnots = FMath::RoundToInt(Shown / 100.0 * 1.94384);
	D.VerticalTenths = FMath::RoundToInt(F.VerticalSpeed / 10.0);
	D.VerticalFpm = FMath::RoundToInt(F.VerticalSpeed / 100.0 * 196.850);   // ft/min, as a VSI reads
	D.AltitudeMetres = FMath::RoundToInt(F.Altitude / 100.0);
	D.Destination = F.Destination;
	D.On = F.On;
	D.bEngineRunning = F.bEngineRunning;
	D.Fuel = F.Fuel;
	D.Pushback = F.Pushback;
	D.Turnaround = F.Turnaround;
	D.Registration = Names.Registration;
	D.Airline = Names.Airline;
	D.Vehicle = Names.Vehicle;
	D.Blocker = Names.Blocker;
	D.Partners = Names.Partners;
	D.Waited = Names.Waited;
	D.bCanDepart = F.bCanDepart;
	return D;
}

void FAircraftCard::Compose(const FAircraftDisplay& D, FInspectorCardView& Out)
{
	Out = FInspectorCardView();
	Out.Card = EInspectorCard::Aircraft;

	// THE REGISTRATION FIRST, as the player speaks of the aircraft - "G-SVBT · C172 · Flying
	// Club". An agent no flight owns (a test's, a debug dispatch) keeps the id title.
	if (!D.Registration.IsEmpty())
	{
		Out.Title = D.Airline.IsEmpty()
			? FString::Format(TEXT("{0} · {1}"), { D.Registration, D.TypeName })
			: FString::Format(TEXT("{0} · {1} · {2}"), { D.Registration, D.TypeName, D.Airline });
	}
	else if (!D.Vehicle.IsEmpty())
	{
		// A SERVICE VEHICLE: its kind's name and its vehicle id, "Bowser #7" (#478) - no flight owns it, so no registration leads.
		Out.Title = D.Vehicle;
	}
	else
	{
		Out.Title = FString::Printf(TEXT("%s  #%d"), *D.TypeName, D.Id);
	}
	// THE RING, in the alert's own words for the fix (UOpsAlerts::DeadlockRemedy) - the card
	// names the partners, the alert counts them, and both ask for the same change.
	Out.Deadlock = D.Partners.IsEmpty() ? FString()
		: FText::Format(NSLOCTEXT("AirportMgr", "InspectorDeadlock", "Deadlocked with {0} - {1}"),
			FText::FromString(D.Partners), UOpsAlerts::DeadlockRemedy()).ToString();
	// LOCTEXT for the words, FString::Format (not Printf) for the sentence - issue #192.
	// UE 5.8's FString::Printf format string must be a compile-time literal
	// (FormatStringSan), so an NSLOCTEXT result cannot be its Fmt argument. Every number is
	// pre-formatted into its own FString, then dropped into the translatable template as a
	// plain {n} string substitution, so only the words around the digits can be translated.
	const FText EngineState = D.bEngineRunning
		? NSLOCTEXT("AirportMgr", "InspectorEngineRunning", "running")
		: NSLOCTEXT("AirportMgr", "InspectorEngineOff", "off");
	Out.Facts = FString::Format(
		*NSLOCTEXT("AirportMgr", "InspectorAircraftFacts",
			"Heading {0}\nSpeed {1} m/s ({2} kt)\nVertical speed {6} m/s ({7} ft/min)\nAltitude {3} m\nTo {4}\nEngine {5}").ToString(),
		{
			FString::Printf(TEXT("%03d"), D.HeadingDegrees),
			FString::Printf(TEXT("%d.%d"), D.SpeedTenths / 10, D.SpeedTenths % 10),
			FString::FromInt(D.SpeedKnots),
			FString::FromInt(D.AltitudeMetres),
			D.Destination,
			EngineState.ToString(),
			// SIGN BY HAND: speed's "%d.%d" would print -3.2 as "-3.-2", and a %.1f of the float reopens the round-half-even
			// gap FAircraftDisplay's comment closed. {6} and {7} come last so the existing indices keep their meaning.
			FString::Printf(TEXT("%s%d.%d"), D.VerticalTenths < 0 ? TEXT("-") : TEXT(""),
				FMath::Abs(D.VerticalTenths) / 10, FMath::Abs(D.VerticalTenths) % 10),
			FString::FromInt(D.VerticalFpm),
		});
	// WHERE IT IS, in names (taxiway naming spec 2026-10-02) - only when the facts know (InspectFacts::WhereIs), so an
	// unnamed airport's card reads as before.
	if (!D.On.IsEmpty())
	{
		Out.Facts += FString::Format(*NSLOCTEXT("AirportMgr", "InspectorOn", "\nOn: {0}").ToString(), { D.On });
	}
	// THE DEMANDS BLOCK (2026-09-28): what the aircraft wants, one line each. The fuel
	// line is AirportOps's whole sentence (it names itself "Fuel ..."); pushback is the
	// airframe's need, which nothing services yet.
	if (!D.Fuel.IsEmpty() || !D.Pushback.IsEmpty())
	{
		Out.Facts += NSLOCTEXT("AirportMgr", "InspectorDemandsHeading", "\n\nDemands").ToString();
		if (!D.Fuel.IsEmpty())
		{
			Out.Facts += TEXT("\n") + D.Fuel;
		}
		if (!D.Pushback.IsEmpty())
		{
			Out.Facts += FString::Format(
				*NSLOCTEXT("AirportMgr", "InspectorPushbackLine", "\nPushback {0}").ToString(), { D.Pushback });
		}
	}
	// THE CONTRACT, after the demands it depends on - see UArrivalRowViewModel::DescribeTurnaround.
	if (!D.Turnaround.IsEmpty())
	{
		Out.Facts += TEXT("\n\n") + D.Turnaround;
	}
	// THE HOLD LINE RE-SAID WITH A NAME - only where StatusOf's precedence chose it
	// (bStatusIsHold), so "Departure armed" still outranks it as before.
	if (D.bStatusIsHold)
	{
		FAgentHold Hold;
		Hold.WaitingOn = D.WaitedForId;
		Hold.At = D.HoldAt;
		Hold.RunwayPair = D.HoldRunwayPair;
		Out.Status = InspectFacts::HoldLine(Hold, D.Blocker, D.Waited);
	}
	else
	{
		Out.Status = D.Status;
	}

	Out.Verbs = EInspectorVerbs::Depart | EInspectorVerbs::Follow | EInspectorVerbs::Unstick;
	// BY ID, not by a position: SelectAndFocus finds the agent where it is at the click, and D.Id is in the display, so this is gated with it.
	Out.Locate.Kind = EAlertFocusKind::Agent;
	Out.Locate.Id = D.Id;
	Out.bCanDepart = D.bCanDepart;
	Out.WaitedForId = D.WaitedForId;
	if (D.WaitedForId != 0)
	{
		Out.Verbs |= EInspectorVerbs::WaitingFor;
		Out.WaitingForCaption = FText::Format(NSLOCTEXT("AirportMgr", "InspectorShowBlockerNamed", "Show {0}"), FText::FromString(D.Blocker));
	}
}

const FInspectorCardView* FAircraftCard::Describe(const FInspectorCardInput& In)
{
	const ARoadNetworkActor& Target = *In.Target;
	FAgentFacts F;
	bool bOk;
	if (In.PrecomputedAgentFacts != nullptr)
	{
		F = *In.PrecomputedAgentFacts;
		bOk = true;
	}
	else
	{
		bOk = Target.GetGroundTraffic() != nullptr
			&& InspectFacts::DescribeAgent(*Target.GetGroundTraffic(), Target.GetNetwork(), In.Selection.Id, F);
	}
	if (!bOk)
	{
		return nullptr;
	}
	const UGroundTraffic* Traffic = Target.GetGroundTraffic();
	const UOpsRuntime* Runtime = In.Runtime;
	// THE JOB BOARD, for the fuel line below and for naming a vehicle (NameOfAgent): a hold or a ring names the bowser it waits on.
	const UJobBoard* Jobs = Runtime != nullptr ? Runtime->GetJobBoard() : nullptr;

	// THE FUEL LINE, from the layer that knows what fuel is. Reached through the ops
	// subsystem rather than through Target, because the airport actor is Airside's and
	// must not carry a pointer to a service it is forbidden to know about - see
	// FAgentFacts::Fuel, the field this fills and DescribeAgent deliberately leaves empty.
	//
	// BEFORE THE DISPLAY, NOT GATED BY IT: the RESULT is one of its fields, so it has
	// to be in hand before the display can be compared. It was read every tick for that reason (a
	// FindByPredicate over active trucks, the cheap kind); since ops batch 3 PR E it has a key
	// of its own - the job board's Revision, and the clock only while it says it is live. See
	// FuelLineBoard's comment for the three lookups and what each reads.
	//
	// THE FLIGHT FIRST, outside the runtime test: the title's registration reads it too, and a test's
	// board (UseFlightBoardForTest) comes with no runtime. One keyed lookup serves the contract and the
	// names - a second FlightForAgent for the title would be the per-tick walk PR E removed.
	const UFlight* OwnFlight = LookUpFlight(In.Flights, F.Id);
	if (Runtime != nullptr)
	{
		const double Now = Runtime->GetClock() != nullptr ? Runtime->GetClock()->Now() : 0.0;
		if (const UJobBoard* Fuel = Jobs)
		{
			if (bFuelLineLive || FuelLineBoard.Get() != Fuel || FuelLineRevision != Fuel->Revision() || FuelLineAgent != F.Id)
			{
				++FuelLookups;
				FuelLine = Fuel->DescribeAgent(F.Id, Now, bFuelLineLive, Target.GetNetwork());
				FuelLineBoard = Fuel;
				FuelLineRevision = Fuel->Revision();
				FuelLineAgent = F.Id;
			}
			F.Fuel = FuelLine;
		}
		// THE CONTRACT, from the flight that owns this aircraft - its minute resolution keeps
		// the gate below from recomposing more than once a game minute.
		if (const UFlight* Flight = OwnFlight; Flight != nullptr && Runtime->GetClock() != nullptr)
		{
			if (Turnaround.Refresh(*Flight, Now))
			{
				++TurnaroundComposes;
			}
			F.Turnaround = Turnaround.Line;
		}
	}

	// THE NAMES (2026-09-30): the registration and airline for the title, and whom it waits for
	// or is deadlocked with, by the name the player reads on the other card - never an id. Looked
	// up here, before the gate, for Fuel's reason: they are the display's inputs. Airside hands ids
	// (FAgentFacts::Hold, DeadlockedWith) because it knows no registrations.
	FAircraftNames Names;
	if (OwnFlight != nullptr)
	{
		Names.Registration = OwnFlight->Callsign;
		Names.Airline = OwnFlight->AirlineName.ToString();
	}
	else
	{
		// NO FLIGHT OWNS IT: it may be a service vehicle's agent (#478), titled by its vehicle - asked only then, so an aircraft with a
		// flight (the common card) pays no fleet walk. A plain lookup over the fleet, every tick, for the one selected agent.
		Names.Vehicle = NameOfVehicle(Jobs, F.Id);
	}
	// UNCACHED, and on purpose: whom it waits for and the ring re-derive from this tick's facts
	// (F.Hold, F.DeadlockedWith - InspectFacts::DescribeAgent, every tick), and they are FAircraftDisplay
	// fields, so a changed blocker or ring recomposes the card with no flight or job board revision
	// moving. Asked only while held or deadlocked - a quiet card asks nothing.
	// ENFORCED BY: AirportMgr.Inspector.Cache.WaitingOnRefreshesTheCard
	if (F.Hold.IsSet())
	{
		Names.Blocker = NameOfAgent(In.Flights, Traffic, Jobs, F.Hold.WaitingOn);
	}
	TArray<FString> PartnerNames;
	for (const int32 Partner : F.DeadlockedWith)
	{
		PartnerNames.Add(NameOfAgent(In.Flights, Traffic, Jobs, Partner));
	}
	Names.Partners = FString::Join(PartnerNames, TEXT(", "));
	// HOW LONG, IN GAME TIME and the turnaround line's words (ruled 2026-09-30): the stall clock
	// is movement time, which the day's compression leaves ~72x behind the clock the rest of the
	// card counts in. No clock, no figure - never movement seconds dressed as game minutes.
	if (In.Clock != nullptr && F.Hold.IsSet())
	{
		Names.Waited = GameTimeText::Duration(In.Clock->GameSecondsOfMovement(F.Hold.StalledSeconds)).ToString();
	}

	FAircraftDisplay Display = DisplayOf(F, Names);
	if (!bComposed || Display != LastDisplay)
	{
		++ComposeCalls;   // See ComposeCount.
		Compose(Display, View);
		LastDisplay = MoveTemp(Display);
		bComposed = true;
	}
	return &View;
}
