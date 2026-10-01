#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/AirsideCapability.h"
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
	URoadProfile* Runway = TestProfiles::Runway();
	URoadProfile* Taxiway = TestProfiles::Taxiway();

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
		TestEqual(TEXT("whose length is the whole strip"), Cap.Runways[0].End.Length, 60000.0, 1.0);
		TestEqual(TEXT("and whose profile is the runway's"),
			Cap.Runways[0].Profile.Get(), static_cast<const URoadProfile*>(Runway));
	}

	// A second, separate runway.
	const FRoadNodeId S0 = Net->AddNode(FVector2D(0.0, 100000.0));
	const FRoadNodeId S1 = Net->AddNode(FVector2D(30000.0, 100000.0));
	Net->AddStraightSegment(S0, S1, Runway);
	Cap = AirsideCapability::Summarise(*Net);
	TestEqual(TEXT("a separate strip is a second runway"), Cap.Runways.Num(), 2);

	// (The stand half - a FStandSummary per placed stand and a LongestRunway() - went with its production
	// code in #462: the last production reader of either went in b6926198 and only this test kept them.)

	return true;
}

#endif
