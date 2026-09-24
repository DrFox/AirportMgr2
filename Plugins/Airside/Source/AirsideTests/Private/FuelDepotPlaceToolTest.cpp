#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Content/AirsideContent.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "InputCoreTypes.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/BuildSession.h"
#include "Tool/RoadEditTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelDepotPlaceToolTest,
	"Airside.Tool.FuelDepotPlaced",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelDepotPlaceToolTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("the actor"), Actor)) { return false; }

	// Assigned by hand on the ACTOR - the per-level override, which takes precedence over the
	// content set, so this test says nothing about what is authored until the last block.
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	Actor->StandDefinition = UEntityDefinition::MakeStandTransient();

	// THROUGH THE EDIT TARGET, not a tool. This drove FStandPlaceTool constructed with each
	// kind until 2026-09-23, when that press-drag-release tool was deleted for the drawn stand
	// (FStandPlotTool) - but what it asserted was always the FACADE's: that PlaceEntity
	// resolves the definition by kind and gives each its own pose role. That survives the
	// tool, so the facade is now called directly and nothing here depends on a gesture.
	IRoadEditTarget* Target = Actor;
	Target->PlaceEntity(FVector2D(3000.0, 3000.0), 0.0, EPlaceableEntity::FuelDepot);

	if (!TestEqual(TEXT("one entity placed"), Actor->Network->GetEntities().Num(), 1)) { return false; }

	// THE POINT: the tool placed the DEPOT definition, and the instance carries the depot's
	// pose role - so FAnchorLink casts its lead-in at a road rather than at a taxiway.
	{
		const FEntityInstance& Placed = Actor->Network->GetEntities()[0];
		TestEqual(TEXT("it is the depot"), Placed.Definition.Get(), Actor->FuelDepotDefinition.Get());
		TestEqual(TEXT("and its pose is a vehicle's"),
			static_cast<int32>(Placed.PoseRole), static_cast<int32>(EServiceRole::Fuel));
		TestEqual(TEXT("a depot has no anchors - the pose IS its road connection"),
			Placed.ResolvedAnchors.Num(), 0);
	}

	// The SAME call with the other kind places a stand: only the definition differs by kind.
	Target->PlaceEntity(FVector2D(-3000.0, -3000.0), 0.0, EPlaceableEntity::Stand);

	if (!TestEqual(TEXT("two entities now"), Actor->Network->GetEntities().Num(), 2)) { return false; }
	{
		const FEntityInstance& Placed = Actor->Network->GetEntities()[1];
		TestEqual(TEXT("the second is a stand"), Placed.Definition.Get(), Actor->StandDefinition.Get());
		TestEqual(TEXT("with an aircraft's pose"),
			static_cast<int32>(Placed.PoseRole), static_cast<int32>(EServiceRole::Aircraft));
		TestTrue(TEXT("and its fixtures"), Placed.ResolvedAnchors.Num() > 0);
	}

	// The PREVIEW resolves the same object the placement does - which is what
	// GetStandDefinition's own comment has always warned about, now that there are two.
	TestEqual(TEXT("the depot tool previews the depot"),
		Actor->GetEntityDefinition(EPlaceableEntity::FuelDepot),
		static_cast<const UEntityDefinition*>(Actor->FuelDepotDefinition.Get()));
	TestEqual(TEXT("and GetStandDefinition still answers for the stand"),
		Actor->GetStandDefinition(),
		static_cast<const UEntityDefinition*>(Actor->StandDefinition.Get()));

	// CLEARING THE ACTOR'S OVERRIDE FALLS BACK TO THE CONTENT SET, which since this slice
	// names DA_FuelDepot. That is the resolver's whole contract, and what a player who never
	// touches the Details panel gets.
	Actor->FuelDepotDefinition = nullptr;
	Target->PlaceEntity(FVector2D(9000.0, 9000.0), 0.0, EPlaceableEntity::FuelDepot);
	if (!TestEqual(TEXT("the content default places a third entity"),
		Actor->Network->GetEntities().Num(), 3)) { return false; }
	TestEqual(TEXT("and it is a depot, by its pose role"),
		static_cast<int32>(Actor->Network->GetEntities()[2].PoseRole),
		static_cast<int32>(EServiceRole::Fuel));

	// A REFUSAL, NOT A STAND, when there is no depot definition ANYWHERE.
	//
	// The configured content set is cleared for this assertion and restored straight after:
	// once DA_AirsideContent names the depot there is no other way to reach the branch, and
	// the branch matters - substituting the OTHER kind would drop a stand where a building
	// was asked for, which on screen reads as the tool working.
	{
		UAirsideSettings* Settings = GetMutableDefault<UAirsideSettings>();
		const TSoftObjectPtr<UAirsideContent> Configured = Settings->Content;
		Settings->Content.Reset();
		ON_SCOPE_EXIT { Settings->Content = Configured; };

		Target->PlaceEntity(FVector2D(15000.0, 15000.0), 0.0, EPlaceableEntity::FuelDepot);
		TestEqual(TEXT("no depot definition anywhere places nothing"),
			Actor->Network->GetEntities().Num(), 3);
	}

	// The registry is ONE list: the tool exists under key 0, named Fuel depot, and its own
	// display name agrees with the table.
	bool bFound = false;
	for (const FToolRegistration& Entry : ToolRegistry())
	{
		if (Entry.Key == EKeys::Zero)
		{
			bFound = true;
			TestEqual(TEXT("key 0 is the depot tool"), Entry.Name.ToString(), FString(TEXT("Fuel depot")));
			TestEqual(TEXT("and its display name agrees"),
				Entry.Make()->GetDisplayName().ToString(), Entry.Name.ToString());
		}
	}
	TestTrue(TEXT("key 0 is registered"), bFound);
	return true;
}

#endif
