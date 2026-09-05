#include "CoreMinimal.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/AirsideCapability.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAirsideCapabilityTest,
	"Airside.Model.Capability",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAirsideCapabilityTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>();
	URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Runway->bContinuousThroughJunctions = true;
	URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);

	TestEqual(TEXT("an empty network has no runways"), AirsideCapability::Summarise(*Net).Runways.Num(), 0);

	// A 60000 uu runway SPLIT at 20000 by a taxiway junction: two collinear segments, one strip.
	const FRoadNodeId R0 = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId R1 = Net->AddNode(FVector2D(20000.0, 0.0));
	const FRoadNodeId R2 = Net->AddNode(FVector2D(60000.0, 0.0));
	Net->AddStraightSegment(R0, R1, Runway);
	Net->AddStraightSegment(R1, R2, Runway);
	const FRoadNodeId T = Net->AddNode(FVector2D(20000.0, 15000.0));
	Net->AddStraightSegment(R1, T, Taxiway);

	FAirsideCapability Cap = AirsideCapability::Summarise(*Net);
	TestEqual(TEXT("two collinear runway segments are ONE runway"), Cap.Runways.Num(), 1);
	if (Cap.Runways.Num() == 1)
	{
		TestEqual(TEXT("whose length is the whole strip"), Cap.Runways[0].Length, 60000.0, 1.0);
		TestEqual(TEXT("and whose profile is the runway's"),
			Cap.Runways[0].Profile.Get(), static_cast<const URoadProfile*>(Runway));
	}
	TestEqual(TEXT("LongestRunway reads the same figure"), Cap.LongestRunway(), 60000.0, 1.0);

	// A second, separate runway.
	const FRoadNodeId S0 = Net->AddNode(FVector2D(0.0, 100000.0));
	const FRoadNodeId S1 = Net->AddNode(FVector2D(30000.0, 100000.0));
	Net->AddStraightSegment(S0, S1, Runway);
	Cap = AirsideCapability::Summarise(*Net);
	TestEqual(TEXT("a separate strip is a second runway"), Cap.Runways.Num(), 2);
	TestEqual(TEXT("longest is still the 60000 one"), Cap.LongestRunway(), 60000.0, 1.0);

	// Stands: wingspan captured at placement, roles read from resolved anchors. The
	// definition is a bare NewObject - PlaceEntity refuses a null one but never reads it.
	UEntityDefinition* Definition = NewObject<UEntityDefinition>(GetTransientPackage());
	TArray<FEntityAnchor> Anchors;
	{ FEntityAnchor A; A.Id = TEXT("nose"); A.Role = EServiceRole::Aircraft; Anchors.Add(A); }
	{ FEntityAnchor A; A.Id = TEXT("fuel"); A.Role = EServiceRole::Fuel; A.LocalPosition = FVector2D(0.0, 800.0); Anchors.Add(A); }
	const FEntityInstanceId Small = Net->PlaceEntity(Definition, Anchors, FVector2D(5000.0, 30000.0), 0.0, 1100.0);
	const FEntityInstanceId Big = Net->PlaceEntity(Definition, Anchors, FVector2D(15000.0, 30000.0), 0.0, 3600.0);
	if (!TestTrue(TEXT("stands placed"), Small.IsSet() && Big.IsSet())) { return false; }

	Cap = AirsideCapability::Summarise(*Net);
	TestEqual(TEXT("two stands"), Cap.Stands.Num(), 2);
	const FStandSummary* BigSummary = Cap.Stands.FindByPredicate([Big](const FStandSummary& S) { return S.Entity == Big; });
	if (TestNotNull(TEXT("the big stand is summarised under the handle it was placed with"), BigSummary))
	{
		TestEqual(TEXT("with the wingspan it was placed for"), BigSummary->DesignWingspan, 3600.0, 1e-9);
		TestTrue(TEXT("and the Fuel role its anchors carry"), BigSummary->AnchorRoles.Contains(EServiceRole::Fuel));
		TestTrue(TEXT("and the Aircraft role"), BigSummary->AnchorRoles.Contains(EServiceRole::Aircraft));
	}

	// A removed stand drops out; the survivor keeps its handle.
	Net->RemoveEntity(Small);
	Cap = AirsideCapability::Summarise(*Net);
	TestEqual(TEXT("a removed stand is not summarised"), Cap.Stands.Num(), 1);
	TestTrue(TEXT("the survivor is still the big one"), Cap.Stands.Num() == 1 && Cap.Stands[0].Entity == Big);
	return true;
}

#endif
