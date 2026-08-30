#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Tool/StandPlaceTool.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FToolContext StandAt(ARoadNetworkActor* Actor, const FVector2D& Where)
	{
		FToolContext Context;
		Context.Target = Actor;
		Context.Cursor = Where;
		Context.SnapRadius = 150.0;
		Context.Snap.Kind = ERoadSnapKind::Free;
		Context.Snap.Position = Where;
		return Context;
	}

	int32 LiveEntities(const ARoadNetworkActor* Actor)
	{
		int32 Alive = 0;
		for (const FEntityInstance& Entity : Actor->Network->GetEntities())
		{
			if (Entity.bAlive) { ++Alive; }
		}
		return Alive;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlaceToolTest,
	"RoadNet.Tool.StandPlace",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlaceToolTest::RunTest(const FString& Parameters)
{
	ARoadNetworkActor* Actor = NewObject<ARoadNetworkActor>(GetTransientPackage());
	if (!TestNotNull(TEXT("actor constructed"), Actor))
	{
		return false;
	}

	// The actor resolves DA_Stand_CodeC in its constructor. A test that silently placed
	// nothing because the asset was missing would pass every assertion below vacuously.
	if (!TestNotNull(TEXT("the actor found a stand definition"), Actor->StandDefinition.Get()))
	{
		return false;
	}
	TestTrue(TEXT("and its anchors are all named and distinct"),
		UEntityDefinition::HasUsableAnchorIds(Actor->StandDefinition));

	// --- Stand and aircraft are different things --------------------------------------
	//
	// A Code C stand takes an A320 AND a 737-800, and their doors are metres apart. Where a
	// service is REQUIRED therefore belongs to the aircraft; what the ground can PROVIDE
	// belongs to the stand. Baking one type's geometry into the stand is the bug this split
	// exists to prevent, and this is the assertion that would catch it coming back.
	{
		const UAircraftType* Design = Actor->StandDefinition->DesignAircraft.Get();
		if (!TestNotNull(TEXT("the stand names a design aircraft"), Design))
		{
			return false;
		}

		TestTrue(TEXT("the aircraft carries the plan footprint"), Design->Footprint.IsSet());
		TestTrue(TEXT("and its service points are all named and distinct"),
			UAircraftType::HasUsableServiceIds(Design));
		TestTrue(TEXT("the stand provides fuel"),
			Actor->StandDefinition->Provides(EServiceRole::Fuel));

		// The stand's own anchors are GROUND fixtures. None of them may be a place on an
		// airframe, so none should sit where a door does.
		TestTrue(TEXT("the stand declares ground fixtures"),
			Actor->StandDefinition->Anchors.Num() > 0);

		// THE PROPERTY THE SPLIT BUYS. Two Code C types, same stand, and their hold doors
		// must NOT land in the same place - if they did, the stand could have carried the
		// geometry after all and this whole change bought nothing.
		UAircraftType* Boeing = NewObject<UAircraftType>(GetTransientPackage());
		UAircraftType::Build737(Boeing);

		const FEntityAnchor* AirbusHold = Design->ServicePoints.FindByPredicate(
			[](const FEntityAnchor& Each) { return Each.Id == FName(TEXT("HoldFwd")); });
		const FEntityAnchor* BoeingHold = Boeing->ServicePoints.FindByPredicate(
			[](const FEntityAnchor& Each) { return Each.Id == FName(TEXT("HoldFwd")); });

		if (TestNotNull(TEXT("the A320 has a forward hold"), AirbusHold)
			&& TestNotNull(TEXT("and so does the 737"), BoeingHold))
		{
			TestTrue(TEXT("their forward holds are at DIFFERENT stations"),
				FMath::Abs(AirbusHold->LocalPosition.X - BoeingHold->LocalPosition.X) > 100.0);
		}

		TestTrue(TEXT("and the two types are not the same length"),
			FMath::Abs(Design->Footprint.TailX - Boeing->Footprint.TailX) > 100.0);

		// Four sides of the envelope plus fuselage, wing and tailplane: seven segments.
		TArray<FVector2D> Outline;
		UAircraftType::BuildFootprintLines(Design->Footprint, Outline);
		TestEqual(TEXT("the outline is seven segments"), Outline.Num(), 14);

		// Every service point must fall within the aircraft it belongs to, or the outline
		// is decoration rather than orientation.
		const double HalfSpan = Design->Footprint.Wingspan * 0.5;
		for (const FEntityAnchor& Point : Design->ServicePoints)
		{
			TestTrue(FString::Printf(TEXT("service point %s lies within the wingspan"),
				*Point.Id.ToString()),
				FMath::Abs(Point.LocalPosition.Y) <= HalfSpan);
			TestTrue(FString::Printf(TEXT("service point %s lies along the fuselage"),
				*Point.Id.ToString()),
				Point.LocalPosition.X <= Design->Footprint.NoseX
				&& Point.LocalPosition.X >= Design->Footprint.TailX);
		}

		// An unauthored footprint draws nothing rather than a degenerate dot at the origin.
		FEntityFootprint Empty;
		TestFalse(TEXT("an unauthored footprint reports itself unset"), Empty.IsSet());
		UAircraftType::BuildFootprintLines(Empty, Outline);
		TestEqual(TEXT("and produces no segments at all"), Outline.Num(), 0);
	}

	// --- Press, drag to aim, release --------------------------------------------------
	{
		Actor->ClearNetwork();
		FStandPlaceTool Tool;
		TestTrue(TEXT("a fresh tool is idle"), Tool.IsIdle());

		// Press at the stop position, then drag due north to aim it that way.
		Tool.OnDragBegin(StandAt(Actor, FVector2D(1000.0, 2000.0)));
		TestFalse(TEXT("a press that has begun aiming is not idle"), Tool.IsIdle());
		TestEqual(TEXT("and nothing is committed while aiming"), LiveEntities(Actor), 0);

		Tool.OnDragEnd(StandAt(Actor, FVector2D(1000.0, 9000.0)));
		TestTrue(TEXT("releasing commits the stand"), Tool.IsIdle());
		TestEqual(TEXT("exactly one"), LiveEntities(Actor), 1);

		const FEntityInstance& Placed = Actor->Network->GetEntities()[0];

		// THE POSE. The stand sits where the press landed, NOT where the release did - the
		// drag says which way it faces and nothing else. Getting this backwards would drag
		// the stand along with the cursor and make it impossible to aim one at all.
		TestTrue(TEXT("the stand sits where the press landed"),
			Placed.Position.Equals(FVector2D(1000.0, 2000.0), 0.01));
		TestTrue(TEXT("and faces the way the drag pointed"),
			FMath::IsNearlyEqual(Placed.Heading, UE_DOUBLE_PI * 0.5, 1e-6));

		TestEqual(TEXT("every anchor the definition declares resolved"),
			Placed.ResolvedAnchors.Num(), Actor->StandDefinition->Anchors.Num());
	}

	// A press that never travelled still places one, facing the way the last did. A row of
	// stands on one pier all face the same way, and re-aiming each would be tedious.
	{
		FStandPlaceTool Tool;
		Tool.OnDragBegin(StandAt(Actor, FVector2D(0.0, 0.0)));
		Tool.OnDragEnd(StandAt(Actor, FVector2D(0.0, 6000.0)));

		const int32 Before = LiveEntities(Actor);
		Tool.OnClick(StandAt(Actor, FVector2D(5000.0, 0.0)));

		TestEqual(TEXT("a click with no drag still places a stand"), LiveEntities(Actor), Before + 1);

		const TArray<FEntityInstance>& All = Actor->Network->GetEntities();
		TestTrue(TEXT("facing the way the previous drag aimed"),
			FMath::IsNearlyEqual(All[All.Num() - 1].Heading, UE_DOUBLE_PI * 0.5, 1e-6));
	}

	// Cancelling mid-aim commits nothing. Nothing is ever part-placed, so there is no
	// half-built stand to clean up - which is why this tool needs no state machine.
	{
		Actor->ClearNetwork();
		FStandPlaceTool Tool;

		Tool.OnDragBegin(StandAt(Actor, FVector2D(2000.0, 2000.0)));
		Tool.OnCancel(StandAt(Actor, FVector2D(4000.0, 2000.0)));
		TestTrue(TEXT("cancelling ends the aim"), Tool.IsIdle());

		Tool.OnDragEnd(StandAt(Actor, FVector2D(4000.0, 2000.0)));
		TestEqual(TEXT("and a release after it places nothing"), LiveEntities(Actor), 0);
	}

	// Ctrl+click removes the stand under the cursor, and its anchor nodes go with it.
	{
		Actor->ClearNetwork();
		FStandPlaceTool Tool;
		Tool.OnClick(StandAt(Actor, FVector2D(0.0, 0.0)));
		if (!TestEqual(TEXT("a stand to remove"), LiveEntities(Actor), 1))
		{
			return false;
		}

		TArray<FGuidelineNodeId> Owned;
		for (const FResolvedAnchor& Anchor : Actor->Network->GetEntities()[0].ResolvedAnchors)
		{
			Owned.Add(Anchor.Node);
		}

		FToolContext Remove = StandAt(Actor, FVector2D(50.0, 50.0));
		Remove.bRemoveModifier = true;
		Tool.OnClick(Remove);

		TestEqual(TEXT("ctrl-click removes the stand"), LiveEntities(Actor), 0);
		for (const FGuidelineNodeId& Node : Owned)
		{
			TestNull(TEXT("and its anchor nodes went with it"),
				Actor->Network->GetGuidelineNode(Node));
		}
	}

	// Undo puts a stand back, anchors and all. Placing is one edit, not nine.
	{
		Actor->ClearNetwork();
		FStandPlaceTool Tool;
		Tool.OnClick(StandAt(Actor, FVector2D(3000.0, 3000.0)));

		TestEqual(TEXT("placed"), LiveEntities(Actor), 1);
		TestEqual(TEXT("as a single named edit"), Actor->PeekUndoLabel(), FString(TEXT("place stand")));

		TestTrue(TEXT("undo takes it back"), Actor->Undo());
		TestEqual(TEXT("all of it"), LiveEntities(Actor), 0);

		TestTrue(TEXT("and redo returns it"), Actor->Redo());
		TestEqual(TEXT("with its anchors"),
			Actor->Network->GetEntities()[0].ResolvedAnchors.Num(),
			Actor->StandDefinition->Anchors.Num());
	}

	// --- THE PROPERTY THE WHOLE DESIGN RESTS ON ---------------------------------------
	//
	// FRoadGuidelineBuilder::Build destroys and re-adds every DERIVED guideline node on
	// each run, advancing its generation - so anything holding a derived node's handle
	// across a rebuild finds it dangling. A stand's anchors are exactly that: handles,
	// stored, and expected to outlive every edit made elsewhere on the airport.
	//
	// They survive because they are created NON-DERIVED and the orphan sweep requires
	// bDerived. Asserted here through the TOOL, because that is the path a player takes.
	{
		Actor->ClearNetwork();
		URoadProfile* Taxi = URoadProfile::MakeTransient(800.0, 200.0);

		FStandPlaceTool Tool;
		Tool.OnClick(StandAt(Actor, FVector2D(40000.0, 40000.0)));
		if (!TestEqual(TEXT("a stand well away from any taxiway"), LiveEntities(Actor), 1))
		{
			return false;
		}

		TArray<FGuidelineNodeId> Anchors;
		TArray<FVector2D> Positions;
		for (const FResolvedAnchor& Anchor : Actor->Network->GetEntities()[0].ResolvedAnchors)
		{
			Anchors.Add(Anchor.Node);
			const FGuidelineNode* Node = Actor->Network->GetGuidelineNode(Anchor.Node);
			Positions.Add(Node != nullptr ? Node->Position : FVector2D::ZeroVector);
		}

		// A taxiway network for the builder to churn, drawn after the stand exists.
		const int32 Hub = Actor->PlaceNode(FVector2D(0.0, 0.0));
		const int32 East = Actor->PlaceNode(FVector2D(12000.0, 0.0));
		Actor->ConnectNodes(Hub, East);

		// Twice, because the sweep only bites on a rebuild that finds nodes from a previous
		// one - a single pass would leave the interesting case untested.
		const FRoadSolveResult First = FRoadNetworkSolver::SolveAll(*Actor->Network);
		FRoadGuidelineBuilder::Build(*Actor->Network, First);

		const FRoadSolveResult Second = FRoadNetworkSolver::SolveAll(*Actor->Network);
		FRoadGuidelineBuilder::Build(*Actor->Network, Second);

		for (int32 Index = 0; Index < Anchors.Num(); ++Index)
		{
			const FGuidelineNode* Node = Actor->Network->GetGuidelineNode(Anchors[Index]);
			if (!TestNotNull(TEXT("an anchor handle still resolves after two rebuilds"), Node))
			{
				continue;
			}

			// And to the SAME node, not merely to something. A rebuild that re-pointed an
			// anchor would satisfy a null check and still send the fuel truck elsewhere.
			TestTrue(TEXT("at the position it was placed at"),
				Node->Position.Equals(Positions[Index], 0.01));
			TestFalse(TEXT("and it is still not owned by the derivation"), Node->bDerived);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
