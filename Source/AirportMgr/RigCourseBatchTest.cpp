#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "RigTestCourse.h"
#include "RigYard.h"

#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Present/RoadSurfacePresenter.h"
#include "Solve/GuidelineGeom.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/RoadEditTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------------------
// FRoadRebuildBatch's "no behaviour change" claim, MEASURED (URoadEditFacade's REBUILD
// BATCHES): the rig course and yard laid in one batch must derive EXACTLY the airport they
// derive laid one rebuild per edit. Its own file, not RigTestCourseTest.cpp, so the course's
// behaviour tests and this build-equivalence test can move independently.
// ---------------------------------------------------------------------------------------

// NAMED, NOT ANONYMOUS: the module is a unity build.
namespace RigCourseBatchTest
{
	/**
	 * The actor with its batch calls cut off: every call Lay makes forwards, and
	 * Begin/EndRebuildBatch stay FNullEditTarget's no-ops - so Lay runs its real body against
	 * the real facade, rebuilding once per edit, exactly as it did before the batch existed.
	 * The unbatched reference without a second copy of Lay to drift from the first.
	 */
	struct FUnbatchedTarget : FNullEditTarget
	{
		ARoadNetworkActor& Actor;
		explicit FUnbatchedTarget(ARoadNetworkActor& InActor) : Actor(InActor) {}

		virtual const URoadNetwork* GetNetwork() const override { return Actor.GetNetwork(); }
		virtual int32 PlaceNode(FVector2D Where) override { return Actor.PlaceNode(Where); }
		virtual bool ConnectNodes(int32 A, int32 B, ERoadKind Kind, int32 WidthIndex, EPavement Surface) override
		{
			return Actor.ConnectNodes(A, B, Kind, WidthIndex, Surface);
		}
		using FNullEditTarget::ConnectNodes;
		virtual bool AddReverseTurn(int32 Node, int32 FromFar, int32 IntoFar) override
		{
			return Actor.AddReverseTurn(Node, FromFar, IntoFar);
		}
		virtual bool MakeLiveNodeId(int32 Index, FRoadNodeId& OutId) const override
		{
			return Actor.MakeLiveNodeId(Index, OutId);
		}
		virtual int32 GetWidthCount(ERoadKind Kind) const override { return Actor.GetWidthCount(Kind); }
		virtual URoadProfile* ResolveWidthProfile(ERoadKind Kind, int32 Index) const override
		{
			return Actor.ResolveWidthProfile(Kind, Index);
		}
	};

	/** A double's exact bits - "identical" means bitwise here, not within a tolerance. */
	FString Bits(double Value)
	{
		uint64 Raw = 0;
		FMemory::Memcpy(&Raw, &Value, sizeof(Raw));
		return FString::Printf(TEXT("%016llx"), Raw);
	}

	FString Bits(const FVector2D& V) { return Bits(V.X) + TEXT(",") + Bits(V.Y); }

	/**
	 * Every derived fact the course's drivers read, one line per live guideline node / edge,
	 * SORTED - slot order and handle generations legitimately differ between a graph rebuilt
	 * thirty-odd times and one rebuilt once (FRoadGuidelineBuilder reallocates every node each
	 * time), so identity is compared by CONTENT: positions, the sampled polyline the follower
	 * walks, widths, clearances, direction, traffic, provenance.
	 */
	TArray<FString> Derived(const ARoadNetworkActor& Actor)
	{
		TArray<FString> Lines;
		const URoadNetwork& Net = *Actor.Network;
		int32 LiveNodes = 0;
		for (const FGuidelineNode& Node : Net.GetGuidelineNodes())
		{
			if (!Node.bAlive) { continue; }
			++LiveNodes;
			Lines.Add(FString::Printf(TEXT("N %s hold=%d derived=%d arms=%d"), *Bits(Node.Position),
				static_cast<int32>(Node.HoldingPosition), Node.bDerived ? 1 : 0, Node.Incident.Num()));
		}
		int32 LiveEdges = 0;
		for (const FGuidelineEdge& Edge : Net.GetGuidelineEdges())
		{
			if (!Edge.bAlive) { continue; }
			++LiveEdges;
			const FGuidelineNode* A = Net.GetGuidelineNode(Edge.A);
			const FGuidelineNode* B = Net.GetGuidelineNode(Edge.B);
			FString Line = FString::Printf(TEXT("E %s>%s c=%s dir=%d w=%s span=%s r=%s in=%s out=%s tr=%d from=%d derived=%d"),
				A != nullptr ? *Bits(A->Position) : TEXT("?"), B != nullptr ? *Bits(B->Position) : TEXT("?"),
				*Bits(Edge.Control), static_cast<int32>(Edge.Direction), *Bits(Edge.Width), *Bits(Edge.MaxWingspan),
				*Bits(Edge.MinRadius), *Bits(Edge.ClearInner), *Bits(Edge.ClearOuter), Edge.AllowedTraffic.Bits,
				Edge.DerivedFrom.Index, Edge.bDerived ? 1 : 0);
			for (const float At : Edge.ClearInnerAt) { Line += FString::Printf(TEXT(" i%s"), *Bits(At)); }
			for (const float At : Edge.ClearOuterAt) { Line += FString::Printf(TEXT(" o%s"), *Bits(At)); }
			// THE SAMPLES, not just the curve's inputs: GuidelineGeom::Sample is the one array the
			// search costs, the overlay draws and the follower walks (CLAUDE.md, "samples ONCE").
			if (A != nullptr && B != nullptr)
			{
				TArray<FVector2D> Points;
				GuidelineGeom::Sample(A->Position, Edge.Control, B->Position, Points);
				for (const FVector2D& P : Points) { Line += TEXT(" s") + Bits(P); }
			}
			Lines.Add(MoveTemp(Line));
		}
		for (int32 Index = 0; Index < Net.GetReverseTurns().Num(); ++Index)
		{
			const FGuidelineNode* End = Net.GetGuidelineNode(Net.GetReverseTurnEnd(Index));
			Lines.Add(FString::Printf(TEXT("R %d %s"), Index, End != nullptr ? *Bits(End->Position) : TEXT("none")));
		}
		Lines.Sort();
		// THE COUNTS, as their own lines, so a mismatch names them before the first differing line.
		Lines.Insert(FString::Printf(TEXT("# nodes=%d segments=%d gnodes=%d gedges=%d turns=%d surfaceTris=%d holdTris=%d"),
			Net.GetNodes().Num(), Net.GetSegments().Num(), LiveNodes, LiveEdges, Net.GetReverseTurns().Num(),
			Actor.GetPresenter()->SurfaceTriangleCountForTest(), Actor.GetPresenter()->HoldingPaintTriangleCountForTest()), 0);
		return Lines;
	}

	/** One island's layout, on whichever target the caller is comparing through. */
	using FLayFn = TFunction<int32(IRoadEditTarget&)>;

	/** The loop; returns its waypoint count. */
	int32 LayCourse(IRoadEditTarget& Target)
	{
		return FRigCourseLayout::Lay(Target).Waypoints.Num();
	}

	/** The reversing yard; returns its reverse-turn count. */
	int32 LayYard(IRoadEditTarget& Target)
	{
		TArray<FReverseTurn> Turns;
		FRoadNodeId StartNode;
		FRoadNodeId StartFrom;
		int32 Laid = 0;
		int32 Refused = 0;
		FRigYardLayout::Lay(Target, Turns, StartNode, StartFrom, Laid, Refused);
		return Turns.Num();
	}

	/** What one build left behind: the derived facts, the rebuilds it cost, what Lay returned. */
	struct FBuild
	{
		TArray<FString> Facts;
		int32 Rebuilds = 0;
		int32 Laid = 0;
	};

	/**
	 * Lays one island in a fresh world, batched (the actor itself, so Lay's own batch takes
	 * effect - under one more outer batch, as ARigTestCourse::BuildCourse nests them) or not
	 * (FUnbatchedTarget, one rebuild per edit, as before the batch existed).
	 */
	bool Build(FAutomationTestBase& Test, const FLayFn& Lay, bool bBatched, FBuild& Out)
	{
		FAirsideTestWorld TestWorld;
		ARoadNetworkActor* Actor = TestWorld.Actor;
		if (!Test.TestNotNull(TEXT("a network actor to lay on"), Actor)) { return false; }
		const int32 Before = Actor->TopologyRebuildCountForTest();
		if (bBatched)
		{
			FRoadRebuildBatch Batch(*Actor);
			Out.Laid = Lay(*Actor);
		}
		else
		{
			FUnbatchedTarget Target(*Actor);
			Out.Laid = Lay(Target);
		}
		Out.Rebuilds = Actor->TopologyRebuildCountForTest() - Before;
		Out.Facts = Derived(*Actor);
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigCourseBatchedLayMatchesUnbatchedTest,
	"AirportMgr.RigCourse.BatchedLayMatchesUnbatched",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRigCourseBatchedLayMatchesUnbatchedTest::RunTest(const FString& Parameters)
{
	using namespace RigCourseBatchTest;

	// EACH ISLAND IN ITS OWN WORLD, not the loop and the yard laid together: the unbatched
	// reference rebuilds once per edit, and each rebuild re-derives EVERYTHING already laid, so
	// the two together cost ~8 s where apart they cost ~2 s (measured 2026-09-27). The yard is
	// here for its reverse turns, which the loop has none of.
	struct FCase { const TCHAR* Name; FLayFn Lay; };
	const FCase Cases[] = { { TEXT("the loop"), &LayCourse }, { TEXT("the yard"), &LayYard } };
	for (const FCase& Case : Cases)
	{
		FBuild Unbatched;
		FBuild Batched;
		if (!Build(*this, Case.Lay, /*bBatched*/ false, Unbatched) || !Build(*this, Case.Lay, /*bBatched*/ true, Batched))
		{
			return false;
		}

		// THE CONTROL: the reference really did rebuild per edit, or "identical" compares two
		// batched builds and proves nothing.
		TestTrue(FString::Printf(TEXT("%s: the unbatched reference ran the derived pass once per edit - dozens of times"), Case.Name),
			Unbatched.Rebuilds > 20);
		// THE SEAM: goes red if Lay drops its batch, or the facade stops folding.
		TestEqual(FString::Printf(TEXT("%s: the batched build ran the derived pass exactly once"), Case.Name),
			Batched.Rebuilds, 1);
		TestEqual(FString::Printf(TEXT("%s: both builds laid the same features"), Case.Name),
			Batched.Laid, Unbatched.Laid);

		// THE EQUIVALENCE: every derived fact, bitwise.
		TestTrue(FString::Printf(TEXT("%s: the reference derived a real graph, not an empty one"), Case.Name),
			Unbatched.Facts.Num() > 20);
		if (!TestEqual(FString::Printf(TEXT("%s: the same number of derived facts, batched or not"), Case.Name),
			Batched.Facts.Num(), Unbatched.Facts.Num()))
		{
			AddInfo(FString::Printf(TEXT("unbatched %s"), Unbatched.Facts.Num() > 0 ? *Unbatched.Facts[0] : TEXT("(none)")));
			AddInfo(FString::Printf(TEXT("batched   %s"), Batched.Facts.Num() > 0 ? *Batched.Facts[0] : TEXT("(none)")));
			continue;
		}
		int32 Differing = 0;
		for (int32 Index = 0; Index < Batched.Facts.Num(); ++Index)
		{
			if (Batched.Facts[Index] != Unbatched.Facts[Index] && Differing++ < 5)
			{
				AddError(FString::Printf(TEXT("%s: derived fact %d differs: unbatched [%s] batched [%s]"),
					Case.Name, Index, *Unbatched.Facts[Index], *Batched.Facts[Index]));
			}
		}
		TestEqual(FString::Printf(TEXT("%s: every derived fact is bitwise identical, batched or not - no behaviour change"), Case.Name),
			Differing, 0);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
