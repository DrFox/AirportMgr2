#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Prefixed against the UNITY build - these test files share one translation unit. */
	struct FDisconnectFixture
	{
		ARoadNetworkActor* Actor = nullptr;
		int32 Junction = INDEX_NONE;
		int32 TaxiwayEnd = INDEX_NONE;

		/** A straight runway with one taxiway joining it partway along - the shape the
		 *  report was made on: "once you have connected a runway to a taxiway there is no
		 *  way to disconnect it". */
		bool Build(ARoadNetworkActor* In)
		{
			Actor = In;
			Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
			Actor->MinimumRunwayLength = 10000.0;

			URoadProfile* Profile = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
			Profile->bContinuousThroughJunctions = true;
			if (!Actor->PlaceRunway(FVector2D(0.0, 0.0), FVector2D(40000.0, 0.0), Profile))
			{
				return false;
			}

			const URoadNetwork* Network = Actor->GetNetwork();
			int32 Strip = INDEX_NONE;
			for (int32 Index = 0; Index < Network->GetSegments().Num(); ++Index)
			{
				if (Network->GetSegments()[Index].bAlive
					&& Network->IsRunwaySegment(Network->SegmentIdAt(Index)))
				{
					Strip = Index;
					break;
				}
			}
			if (Strip == INDEX_NONE)
			{
				return false;
			}

			// Split then connect - the two steps a taxiway click into a runway performs.
			Junction = Actor->SplitSegment(Strip, FVector2D(20000.0, 0.0));
			TaxiwayEnd = Actor->PlaceNode(FVector2D(20000.0, 12000.0));
			return Junction != INDEX_NONE
				&& Actor->ConnectNodes(Junction, TaxiwayEnd, ERoadKind::Taxiway);
		}

		/** Runway metres still on the ground, end to end along the strip. */
		double RunwayLength() const
		{
			const URoadNetwork* Network = Actor->GetNetwork();
			double Total = 0.0;
			for (int32 Index = 0; Index < Network->GetSegments().Num(); ++Index)
			{
				const FRoadSegmentId Id = Network->SegmentIdAt(Index);
				if (!Network->GetSegments()[Index].bAlive || !Network->IsRunwaySegment(Id))
				{
					continue;
				}
				const FRoadSegment& Segment = Network->GetSegments()[Index];
				const FRoadNode* A = Network->GetNode(Segment.A);
				const FRoadNode* B = Network->GetNode(Segment.B);
				if (A != nullptr && B != nullptr)
				{
					Total += FVector2D::Distance(A->Position, B->Position);
				}
			}
			return Total;
		}

		bool TaxiwayStillThere() const
		{
			const URoadNetwork* Network = Actor->GetNetwork();
			for (int32 Index = 0; Index < Network->GetSegments().Num(); ++Index)
			{
				const FRoadSegmentId Id = Network->SegmentIdAt(Index);
				if (Network->GetSegments()[Index].bAlive && !Network->IsRunwaySegment(Id))
				{
					return true;
				}
			}
			return false;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDisconnectingATaxiwayKeepsTheRunwayTest,
	"Airside.Model.DisconnectingATaxiwayKeepsTheRunway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDisconnectingATaxiwayKeepsTheRunwayTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	FDisconnectFixture Fix;
	if (!TestTrue(TEXT("a runway with a taxiway joined to it was laid"), Fix.Build(Actor)))
	{
		return false;
	}
	TestTrue(TEXT("the runway is 400 m to begin with"),
		FMath::Abs(Fix.RunwayLength() - 40000.0) < 1.0);
	TestTrue(TEXT("and the taxiway is there"), Fix.TaxiwayStillThere());

	// THE REPORTED DEFECT. Deleting the junction is the gesture the player makes, and it
	// used to be refused outright - the heal tried to graft the TAXIWAY onto a runway
	// THRESHOLD, because at this junction every neighbour keeps zero roads and the anchor
	// was simply the nearest one. Nothing ever rejoined the runway to itself.
	const FRoadDeletionPlan Plan = Actor->PlanNodeDeletion(Fix.Junction);
	if (!TestTrue(TEXT("deleting a runway junction is planned rather than refused"), Plan.bValid))
	{
		return false;
	}

	TestTrue(TEXT("the junction deletes"), Actor->DeleteNode(Fix.Junction));

	// THE RUNWAY SURVIVES WHOLE. Measured end to end rather than by counting segments: a
	// deletion that severed it would leave two pieces whose lengths happen to sum, and a
	// deletion that healed it with the wrong profile would leave no runway metres at all.
	TestTrue(*FString::Printf(TEXT("the runway is still 400 m end to end, not severed "
		"(measured %.0f)"), Fix.RunwayLength()),
		FMath::Abs(Fix.RunwayLength() - 40000.0) < 1.0);

	// AND THE TAXIWAY IS GONE, which is the whole point - "delete the taxiway, keep the
	// runway".
	TestFalse(TEXT("the taxiway is gone"), Fix.TaxiwayStillThere());

	// THE HEALED PIECE IS STILL A RUNWAY. HealProfile takes the first incident arm, which
	// at this junction may be the taxiway - and relaying a runway with a taxiway's
	// cross-section would quietly stop it being one, since IsRunwaySegment reads exactly
	// that. RunwayLength() above counts only runway segments, so this is already implied -
	// stated separately because it is a different mistake.
	const URoadNetwork* Network = Actor->GetNetwork();
	int32 RunwaySegments = 0;
	for (int32 Index = 0; Index < Network->GetSegments().Num(); ++Index)
	{
		RunwaySegments += (Network->GetSegments()[Index].bAlive
			&& Network->IsRunwaySegment(Network->SegmentIdAt(Index))) ? 1 : 0;
	}
	TestEqual(TEXT("and it is one runway segment again, relaid with the runway's own profile"),
		RunwaySegments, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDeletingAPlainJunctionIsUnchangedTest,
	"Airside.Model.DeletingAPlainJunctionIsUnchanged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDeletingAPlainJunctionIsUnchangedTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	// THE CONTROL FOR THE RULE ABOVE. A runway is now named as a through-route, and the
	// risk of naming one is that every OTHER junction quietly starts behaving like it.
	// A plain taxiway tee must heal exactly as it always did: the stranded arms rejoin the
	// neighbour that keeps the most roads.
	const int32 Centre = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 West = Actor->PlaceNode(FVector2D(-12000.0, 0.0));
	const int32 East = Actor->PlaceNode(FVector2D(12000.0, 0.0));
	const int32 North = Actor->PlaceNode(FVector2D(0.0, 12000.0));
	Actor->ConnectNodes(Centre, West);
	Actor->ConnectNodes(Centre, East);
	Actor->ConnectNodes(Centre, North);

	const FRoadDeletionPlan Plan = Actor->PlanNodeDeletion(Centre);
	TestTrue(TEXT("a plain tee still plans a heal"), Plan.bValid);
	TestTrue(TEXT("and still rejoins its stranded arms rather than letting them go - the "
				  "runway rule must not have leaked into every junction"),
		Plan.Rejoin.Num() > 0);
	return true;
}

#endif
