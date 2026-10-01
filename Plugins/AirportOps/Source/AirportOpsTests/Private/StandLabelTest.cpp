#include "CoreMinimal.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/InspectFacts.h"
#include "Model/FacilityPurchases.h"
#include "Model/JobBoard.h"
#include "Model/OpsAlerts.h"
#include "Model/OpsEventBus.h"
#include "Model/OpsNames.h"
#include "Model/RoadNetwork.h"

#if WITH_DEV_AUTOMATION_TESTS

// ONE NAME FOR A STAND (#447): the stand card and the digits painted at its turn-off say StandNumber (1..N, never reused); the
// depot card's backlog and the JobUnserviceable alert printed the ENTITY INDEX, which RoadSlot recycles and which starts at 0 - so
// "No fuel for stand 0" beside a sign reading 4. The fixture is the issue's own pin: three stands, #1 deleted, another placed.

namespace
{
	struct FRecycledStandField
	{
		URoadNetwork* Net = nullptr;
		/** The stand placed AFTER the first was deleted: number 4, in the first one's recycled slot. */
		FEntityInstanceId Fourth;
		FEntityInstanceId FirstGone;
		/** A fuel depot on the same network - a real facility, so UFacilityPurchases::Quote has a card to fill. */
		FEntityInstanceId Depot;

		bool Build()
		{
			Net = NewObject<URoadNetwork>(GetTransientPackage());
			UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
			FirstGone = Net->PlaceEntity(Stand, Stand->Anchors, FVector2D(0.0, 0.0), 0.0);
			const FEntityInstanceId Second = Net->PlaceEntity(Stand, Stand->Anchors, FVector2D(10000.0, 0.0), 0.0);
			const FEntityInstanceId Third = Net->PlaceEntity(Stand, Stand->Anchors, FVector2D(20000.0, 0.0), 0.0);
			if (!FirstGone.IsSet() || !Second.IsSet() || !Third.IsSet() || !Net->RemoveEntity(FirstGone))
			{
				return false;
			}
			Fourth = Net->PlaceEntity(Stand, Stand->Anchors, FVector2D(0.0, 0.0), 0.0);
			UEntityDefinition* DepotDef = UEntityDefinition::MakeFuelDepotTransient();
			Depot = Net->PlaceEntity(DepotDef, DepotDef->Anchors, FVector2D(0.0, 30000.0), 0.0, 0.0, EServiceRole::Fuel);
			return Fourth.IsSet() && Depot.IsSet();
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStandLabelAgreeTest, "AirportOps.Model.StandLabel.AlertBacklogAndCardSayTheSameNumber",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FStandLabelAgreeTest::RunTest(const FString&)
{
	FRecycledStandField Field;
	if (!TestTrue(TEXT("three stands placed, the first deleted, a fourth placed"), Field.Build())) { return false; }
	const FEntityInstance* Fourth = Field.Net->GetEntity(Field.Fourth);
	if (!TestNotNull(TEXT("the fourth stand is alive"), Fourth)) { return false; }
	const int32 Number = Fourth->StandNumber;
	TestEqual(TEXT("it is stand 4 - the first's number is retired, not reused"), Number, 4);
	TestEqual(TEXT("it took the DELETED stand's slot - the recycling that makes an index the wrong name for a stand"), Field.Fourth.Index, Field.FirstGone.Index);
	TestNotEqual(TEXT("so its entity index is NOT its number, which is the whole of the bug"), Field.Fourth.Index, Number);

	FStandFacts Card;
	if (!TestTrue(TEXT("the stand card reads it"), InspectFacts::DescribeStand(nullptr, *Field.Net, Field.Fourth.Index, Card))) { return false; }
	const FString Expected = FString::Printf(TEXT("stand %d"), Card.Number);
	TestEqual(TEXT("the card's number is the stand's number"), Card.Number, 4);

	// THE ALERT.
	UJobBoard* Jobs = NewObject<UJobBoard>(GetTransientPackage());
	Jobs->AddJobForTest(1, EServiceJobState::Unserviceable, EServiceRefusal::NoDepot, 0).Stand = Field.Fourth;
	FOpsEventBus Bus;
	UOpsAlerts* Alerts = NewObject<UOpsAlerts>(GetTransientPackage());
	Alerts->Bus = &Bus;
	Bus.BeginWiring();
	Bus.EndWiring();
	FOpsAlertSources Sources;
	Sources.Jobs = Jobs;
	Sources.Network = Field.Net;
	Alerts->Recompute(Sources, 0.0);
	const FOpsAlert* Alert = Alerts->GetAlerts().FindByPredicate([](const FOpsAlert& A) { return A.Key.Kind == EAlertKind::JobUnserviceable; });
	if (TestNotNull(TEXT("the unserviceable job raises its alert"), Alert))
	{
		TestTrue(*FString::Printf(TEXT("the alert names the stand by the sign's number (%s), not its index: '%s'"), *Expected, *Alert->Text.ToString()),
			Alert->Text.ToString().Contains(Expected + TEXT(":")));
	}

	// THE BACKLOG, and the vehicle's own line in it: the same stand, a depot card's two places that name it.
	UJobBoard* Board = NewObject<UJobBoard>(GetTransientPackage());
	const FEntityInstanceId Depot = Field.Depot;
	FServiceJob& Job = Board->AddJobForTest(2, EServiceJobState::Underway, EServiceRefusal::None, 0);
	Job.Stand = Field.Fourth;
	Job.QuantityOwed = 500.0;
	const int32 JobId = Job.Id;
	FServiceVehicle& Bowser = Board->AddVehicleForTest(TEXT("FUEL"), Depot, EServiceVehicleState::ToJob, 1000.0);
	Bowser.CurrentJob = JobId;
	const FDepotBacklog Backlog = Board->DescribeDepot(Depot, 0.0, Field.Net);
	TestTrue(*FString::Printf(TEXT("the vehicle's line says where it is going by number (%s): '%s'"), *Expected, *Backlog.Detail),
		Backlog.Detail.Contains(FString::Printf(TEXT("to %s"), *Expected)));
	TestTrue(*FString::Printf(TEXT("and so does the job's line (%s): '%s'"), *Expected, *Backlog.Detail),
		Backlog.Detail.Contains(FString::Printf(TEXT("  %s ·"), *Expected)));

	// THE FLEET ROWS ARE THE DEPOT CARD'S THIRD PLACE THAT NAMES THE STAND: UFacilityPurchases::Quote builds each row's Line through
	// VehicleLine, which took the network as an optional argument and printed the index when a caller left it off - so one card said "to stand 4"
	// in its backlog and "to stand 0" in its fleet row (#447 review).
	UFacilityPurchases* Shop = NewObject<UFacilityPurchases>(GetTransientPackage());
	Shop->JobBoard = Board;
	const FFacilityQuote Quote = Shop->Quote(*Field.Net, Depot);
	if (TestTrue(TEXT("the depot is a facility with the vehicle as a fleet row"), Quote.IsFacility() && Quote.Fleet.Num() == 1))
	{
		TestTrue(*FString::Printf(TEXT("the fleet row says where it is going by number (%s): '%s'"), *Expected, *Quote.Fleet[0].Line),
			Quote.Fleet[0].Line.Contains(FString::Printf(TEXT("to %s"), *Expected)));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStandLabelFallbackTest, "AirportOps.Model.StandLabel.NamesByNumberAndFallsBackToTheIndex",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FStandLabelFallbackTest::RunTest(const FString&)
{
	FRecycledStandField Field;
	if (!TestTrue(TEXT("three stands placed, the first deleted, a fourth placed"), Field.Build())) { return false; }
	TestEqual(TEXT("the stand placed after the delete is named by its number, 4"), OpsNames::StandLabel(Field.Net, Field.Fourth), FString(TEXT("4")));
	const FEntityInstanceId Second = [&Field]
	{
		for (int32 Index = 0; Index < Field.Net->GetEntities().Num(); ++Index)
		{
			if (Field.Net->GetEntities()[Index].bAlive && Field.Net->GetEntities()[Index].StandNumber == 2)
			{
				FEntityInstanceId Id;
				Id.Index = Index;
				Id.Generation = Field.Net->GetEntities()[Index].Generation;
				return Id;
			}
		}
		return FEntityInstanceId();
	}();
	TestEqual(TEXT("the survivors keep theirs: stand 2 is still 2"), OpsNames::StandLabel(Field.Net, Second), FString(TEXT("2")));

	// THE FALLBACKS, each the index: there is no number to give.
	TestEqual(TEXT("no network (a world-free board test): the index"), OpsNames::StandLabel(nullptr, Field.Fourth), FString::FromInt(Field.Fourth.Index));
	TestEqual(TEXT("the deleted stand's stale handle: its index, not the number of whoever took its slot"),
		OpsNames::StandLabel(Field.Net, Field.FirstGone), FString::FromInt(Field.FirstGone.Index));
	TestEqual(TEXT("an unset handle: INDEX_NONE"), OpsNames::StandLabel(Field.Net, FEntityInstanceId()), FString::FromInt(INDEX_NONE));
	return true;
}

// ONE NAME FOR A DEPOT (#490): the same fault #447 fixed for stands. The depot card's title and the job board's vehicle lines ("to depot /
// refilling at depot / at depot") printed the entity INDEX, which RoadSlot recycles into a different depot after a bulldoze - so a player who
// built depots 1 and 2, bulldozed 1 and built another read "depot 1" beside their memory of the depot that was gone. The fixture is the issue's
// own pin: two depots, #1 deleted, another placed.

namespace
{
	struct FRecycledDepotField
	{
		URoadNetwork* Net = nullptr;
		/** The depot placed AFTER the first was deleted: number 3, in the first one's recycled slot. */
		FEntityInstanceId Third;
		FEntityInstanceId FirstGone;
		FEntityInstanceId Second;

		bool Build()
		{
			Net = NewObject<URoadNetwork>(GetTransientPackage());
			UEntityDefinition* DepotDef = UEntityDefinition::MakeFuelDepotTransient();
			FirstGone = Net->PlaceEntity(DepotDef, DepotDef->Anchors, FVector2D(0.0, 30000.0), 0.0, 0.0, EServiceRole::Fuel);
			Second = Net->PlaceEntity(DepotDef, DepotDef->Anchors, FVector2D(10000.0, 30000.0), 0.0, 0.0, EServiceRole::Fuel);
			if (!FirstGone.IsSet() || !Second.IsSet() || !Net->RemoveEntity(FirstGone))
			{
				return false;
			}
			Third = Net->PlaceEntity(DepotDef, DepotDef->Anchors, FVector2D(0.0, 30000.0), 0.0, 0.0, EServiceRole::Fuel);
			return Third.IsSet();
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDepotLabelVehicleLinesTest, "AirportOps.Model.DepotLabel.VehicleLinesSayTheDepotsNumber",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FDepotLabelVehicleLinesTest::RunTest(const FString&)
{
	FRecycledDepotField Field;
	if (!TestTrue(TEXT("two depots placed, the first deleted, a third placed"), Field.Build())) { return false; }
	const FEntityInstance* Third = Field.Net->GetEntity(Field.Third);
	if (!TestNotNull(TEXT("the third depot is alive"), Third)) { return false; }
	TestEqual(TEXT("it is depot 3 - the first's number is retired, not reused"), Third->DepotNumber, 3);
	TestEqual(TEXT("it took the DELETED depot's slot - the recycling that makes an index the wrong name for a depot"), Field.Third.Index, Field.FirstGone.Index);
	TestNotEqual(TEXT("so its entity index is NOT its number, which is the whole of the bug"), Field.Third.Index, Third->DepotNumber);
	TestEqual(TEXT("the label is the number"), OpsNames::DepotLabel(Field.Net, Field.Third), FString(TEXT("3")));

	// THE THREE LINES THAT NAME A DEPOT, one per vehicle state (UJobBoard::VehicleDoing).
	UJobBoard* Board = NewObject<UJobBoard>(GetTransientPackage());
	FServiceVehicle& Bowser = Board->AddVehicleForTest(TEXT("FUEL"), Field.Third, EServiceVehicleState::ToFacility, 1000.0);
	Bowser.AgentId = 7;
	TestTrue(*FString::Printf(TEXT("heading home names the depot by number: '%s'"), *Board->VehicleLine(Bowser, Field.Net)),
		Board->VehicleLine(Bowser, Field.Net).Contains(TEXT("to depot 3")));
	TestTrue(*FString::Printf(TEXT("and so does the vehicle card's own line: '%s'"), *Board->DescribeAgent(7, 0.0, Field.Net)),
		Board->DescribeAgent(7, 0.0, Field.Net).Contains(TEXT("to depot 3")));
	Bowser.State = EServiceVehicleState::AtFacility;
	TestTrue(*FString::Printf(TEXT("refilling names it by number: '%s'"), *Board->VehicleLine(Bowser, Field.Net)),
		Board->VehicleLine(Bowser, Field.Net).Contains(TEXT("refilling at depot 3")));
	Bowser.State = EServiceVehicleState::Idle;
	TestTrue(*FString::Printf(TEXT("sitting idle names it by number: '%s'"), *Board->VehicleLine(Bowser, Field.Net)),
		Board->VehicleLine(Bowser, Field.Net).Contains(TEXT("at depot 3")));

	// THE DEPOT CARD'S BACKLOG carries the same line for each vehicle, and the FLEET ROW is the card's third place that says it (see the stand
	// test above for why the rows matter: one card said one thing in its backlog and another in its fleet).
	Bowser.State = EServiceVehicleState::ToFacility;
	const FDepotBacklog Backlog = Board->DescribeDepot(Field.Third, 0.0, Field.Net);
	TestTrue(*FString::Printf(TEXT("the backlog's vehicle line says depot 3: '%s'"), *Backlog.Detail), Backlog.Detail.Contains(TEXT("to depot 3")));
	UFacilityPurchases* Shop = NewObject<UFacilityPurchases>(GetTransientPackage());
	Shop->JobBoard = Board;
	const FFacilityQuote Quote = Shop->Quote(*Field.Net, Field.Third);
	if (TestTrue(TEXT("the depot is a facility with the vehicle as a fleet row"), Quote.IsFacility() && Quote.Fleet.Num() == 1))
	{
		TestTrue(*FString::Printf(TEXT("the fleet row says depot 3: '%s'"), *Quote.Fleet[0].Line), Quote.Fleet[0].Line.Contains(TEXT("to depot 3")));
	}

	// THE SURVIVOR keeps its own: depot 2 is still 2, wherever the third went.
	TestEqual(TEXT("the survivor keeps its number"), OpsNames::DepotLabel(Field.Net, Field.Second), FString(TEXT("2")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDepotLabelFallbackTest, "AirportOps.Model.DepotLabel.FallsBackToTheIndex",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FDepotLabelFallbackTest::RunTest(const FString&)
{
	FRecycledDepotField Field;
	if (!TestTrue(TEXT("two depots placed, the first deleted, a third placed"), Field.Build())) { return false; }
	// THE FALLBACKS, each the index: there is no number to give.
	TestEqual(TEXT("no network (a world-free board test): the index"), OpsNames::DepotLabel(nullptr, Field.Third), FString::FromInt(Field.Third.Index));
	TestEqual(TEXT("the deleted depot's stale handle: its index, not the number of whoever took its slot"),
		OpsNames::DepotLabel(Field.Net, Field.FirstGone), FString::FromInt(Field.FirstGone.Index));
	TestEqual(TEXT("an unset handle: INDEX_NONE"), OpsNames::DepotLabel(Field.Net, FEntityInstanceId()), FString::FromInt(INDEX_NONE));

	// A graph-free board test of a vehicle line still reads: no network, the index, exactly as before.
	UJobBoard* Board = NewObject<UJobBoard>(GetTransientPackage());
	FEntityInstanceId Home;
	Home.Index = 4;
	FServiceVehicle& Bowser = Board->AddVehicleForTest(TEXT("FUEL"), Home, EServiceVehicleState::ToFacility, 1000.0);
	TestTrue(TEXT("a vehicle line with no network names the depot by index"), Board->VehicleLine(Bowser, nullptr).Contains(TEXT("to depot 4")));
	return true;
}

#endif
