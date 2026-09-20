#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/ApronDrawTool.h"
#include "Tool/BuildSession.h"
#include "Tool/RoadBuildTool.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/SnapGuideChain.h"
#include "Tool/SnapGuideSettings.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE FREE START: guides before the first click, and the tools that CONSUME them.
 *
 * MEASURED AT THE PREVIEW AND AT THE CLICK, never at the anchor. On 2026-09-20 two tools were
 * given DescribeGuideAnchor and tests asserting the anchor was right, and neither tool drew or
 * obeyed the guide: 479 tests were green while the feature did nothing on screen. An anchor is
 * the producer; a dashed line and a placed node are the consumer, and only the second is a
 * feature. Every test in this file goes through FBuildSession::MakeContext - the seam both
 * drivers share, and the only thing that puts a guide on a context at all.
 */
namespace
{
	/** Records the guide's own geometry - the endpoints, because "a dashed line to the thing"
	 *  is a claim about where the line goes and a style count cannot check it. Same shape as
	 *  PlotGuideTest's, which cannot be shared: this module is a unity build and that one is
	 *  in an anonymous namespace. */
	struct FFreeStartSink : IToolPreviewSink
	{
		struct FRecordedLine
		{
			FVector2D From = FVector2D::ZeroVector;
			FVector2D To = FVector2D::ZeroVector;
			EPreviewStyle Style = EPreviewStyle::Pending;
		};

		TArray<FRecordedLine> Lines;
		TArray<FString> GuideLabels;

		virtual void Marker(const FVector2D& At, EPreviewStyle Style) override {}
		virtual void Line(const FVector2D& From, const FVector2D& To, EPreviewStyle Style) override
		{
			Lines.Add({ From, To, Style });
		}
		virtual void CrossMark(const FVector2D& At, const FVector2D& Along, EPreviewStyle Style) override {}
		virtual void Label(const FVector2D& At, const FString& Text, EPreviewStyle Style) override
		{
			if (Style == EPreviewStyle::Guide) { GuideLabels.Add(Text); }
		}

		TArray<FRecordedLine> Of(EPreviewStyle Style) const
		{
			return Lines.FilterByPredicate(
				[Style](const FRecordedLine& L) { return L.Style == Style; });
		}
	};

	/**
	 * A session with one tool selected by ID, and a world with an east-west taxiway on y = 0.
	 *
	 * COLLINEAR SWITCHED ON EXPLICITLY. It is the relation a free start actually has to offer -
	 * every ANGULAR row sits out when the cursor is on the origin, which is the whole mechanism
	 * (see FGuideAnchor::bFreeStart) - and a test that inherited the default would silently
	 * stop measuring anything the day that default moved either way.
	 */
	struct FFreeStart
	{
		FAirsideTestWorld TestWorld;
		FBuildSession Session;
		FBuildSessionTunables Tunables;
		IBuildTool* Tool = nullptr;

		FToolContext At(const FVector2D& Where) const
		{
			return Session.MakeContext(TestWorld.Actor, Where, Tunables, false, false);
		}
	};

	/** The registry index of a tool by ID - never a literal, which the next tool added moves. */
	int32 ToolIndexFor(const TCHAR* Id)
	{
		const TConstArrayView<FToolRegistration> Registry = ToolRegistry();
		for (int32 Index = 0; Index < Registry.Num(); ++Index)
		{
			if (Registry[Index].Id == FName(Id)) { return Index; }
		}
		return INDEX_NONE;
	}

	/** The road every test here lines up against: west to east through the origin. */
	void LayEastWestTaxiway(ARoadNetworkActor* Actor)
	{
		IRoadEditTarget* Target = Actor;
		const int32 West = Target->PlaceNode(FVector2D(-20000.0, 0.0));
		const int32 East = Target->PlaceNode(FVector2D(-4000.0, 0.0));
		Target->ConnectNodes(West, East, ERoadKind::Taxiway, INDEX_NONE);
	}

	bool Begin(FFreeStart& Out, const TCHAR* ToolId)
	{
		if (Out.TestWorld.World == nullptr || Out.TestWorld.Actor == nullptr) { return false; }
		LayEastWestTaxiway(Out.TestWorld.Actor);

		const int32 Index = ToolIndexFor(ToolId);
		if (Index == INDEX_NONE) { return false; }

		Out.Tunables = Out.TestWorld.Actor->MakeTunables(10000.0);
		Out.Tunables.GuideSources.bCollinear = true;
		Out.Tunables.GuideSources.bRoad = true;

		Out.Session.SelectTool(Index);
		Out.Tool = Out.Session.GetActiveTool();
		return Out.Tool != nullptr;
	}

	/**
	 * A cursor east of the road's end and a little off its line: within EFit::Perpendicular's
	 * 300 uu of the extension, so the guide is eligible, and NOT on it, so a test can tell a
	 * guide that moved the point from one that did nothing.
	 *
	 * AND WITHIN FTuning::SearchRadiusUu OF THE ROAD ITSELF - 8000 uu from its east end, where
	 * the reach is 10000. FCollinearGuideSource measures that from the CURSOR (the fix item 1
	 * made), and a first draft of this fixture sat 10000.7 uu out and silently proposed
	 * nothing at all.
	 */
	const FVector2D OffTheLine = FVector2D(4000.0, 120.0);
}

/**
 * THE DRIVER PUTS THE CURSOR IN THE ORIGIN, AND THE ANGULAR ROWS THEN SIT OUT BY THEMSELVES.
 *
 * The mechanism the whole item rests on, measured rather than asserted: with Origin on Cursor
 * SnapGuide::Arbitrate has no direction to measure, so no EFit::Angular candidate can be
 * eligible - and a free start therefore offers the positional guides and nothing meaningless,
 * with no special case anywhere in the chain. World and Parallel are ON in this test for
 * exactly that reason: they are angular, they would otherwise be the loudest thing on the
 * field, and their absence is the measurement.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFreeStartOffersOnlyPositionalGuidesTest,
	"Airside.Tool.FreeStartOffersOnlyPositionalGuides",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFreeStartOffersOnlyPositionalGuidesTest::RunTest(const FString& Parameters)
{
	FFreeStart Start;
	if (!TestTrue(TEXT("an idle taxiway tool"), Begin(Start, TEXT("Taxiway")))) { return false; }

	TestTrue(TEXT("with the angular rows switched on, so their absence below is a measurement"),
		Start.Tunables.GuideSources.bParallel && Start.Tunables.GuideSources.bWorld);

	const FToolContext Context = Start.At(OffTheLine);
	if (!TestTrue(TEXT("a guide holds before the first click"), Context.Guide.bActive))
	{
		return false;
	}

	for (const SnapGuide::FCandidate& Winner : Context.Guide.Winners)
	{
		TestEqual(
			*FString::Printf(TEXT("'%s' is judged by distance, not by a direction that does not exist"),
				*Winner.Description),
			static_cast<int32>(Winner.Fit), static_cast<int32>(SnapGuide::EFit::Perpendicular));
	}

	// AND IT PUT THE POINT ON THE ROAD'S LINE. The cursor was 120 uu north of it; the guided
	// point is on it. Without this the test would pass on a guide that was merely reported.
	TestTrue(TEXT("and the guided point is on the line the road lies along"),
		FMath::IsNearlyEqual(Context.GuidedCursor().Y, 0.0, 1.0));
	TestFalse(TEXT("which is not where the raw cursor was"),
		Context.GuidedCursor().Equals(Context.Cursor, 1.0));

	return true;
}

/**
 * EVERY OPTED-IN TOOL DRAWS ITS FREE-START GUIDE, AND OBEYS IT ON THE CLICK.
 *
 * THE TRAP THIS EXISTS FOR, paid for twice on 2026-09-20 and once in front of the user: a tool
 * that describes an anchor and stops has a guide computed and thrown away. Nothing in
 * FBuildSession draws one - the emission is the TOOL'S, in BuildPreview - so the anchor's own
 * test can pass on a feature that shows nothing. Four tools, four previews, four clicks.
 *
 * ONE TEST OVER FOUR TOOLS rather than four tests, because the thing being asserted is the
 * same sentence four times and a fifth opted-in tool should have to be added to exactly one
 * list. The tool id names which failed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFreeStartToolsDrawTheirGuideTest,
	"Airside.Tool.FreeStartToolsDrawTheirGuide",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFreeStartToolsDrawTheirGuideTest::RunTest(const FString& Parameters)
{
	// THE FOUR CLASSES, not the five registry entries: Taxiway and Road are one FRoadDrawTool
	// under two entries, so drawing is the same code twice. Both are still walked, because the
	// entry is what the player picks and an entry wired to the wrong constructor would show
	// here and nowhere else.
	for (const TCHAR* Id : { TEXT("Taxiway"), TEXT("Road"), TEXT("Apron"), TEXT("Stand"), TEXT("Runway") })
	{
		FFreeStart Start;
		if (!TestTrue(*FString::Printf(TEXT("an idle '%s' tool"), Id), Begin(Start, Id)))
		{
			return false;
		}

		const FToolContext Context = Start.At(OffTheLine);
		if (!TestTrue(*FString::Printf(TEXT("'%s' resolves a guide before its first click"), Id),
			Context.Guide.bActive))
		{
			continue;
		}

		FFreeStartSink Sink;
		Start.Tool->BuildPreview(Context, Sink);

		const TArray<FFreeStartSink::FRecordedLine> Drawn = Sink.Of(EPreviewStyle::Guide);
		if (!TestTrue(*FString::Printf(TEXT("'%s' DRAWS the dashed line it was given"), Id),
			Drawn.Num() > 0))
		{
			continue;
		}

		// FROM THE POINT THE CLICK WOULD TAKE, so the line touches the ghost rather than
		// floating beside it - and so a tool that drew from the RAW cursor while clicking the
		// guided one is caught here. The two differ by 120 uu in this fixture on purpose.
		for (const FFreeStartSink::FRecordedLine& Line : Drawn)
		{
			TestTrue(
				*FString::Printf(TEXT("'%s' draws it from the point the click would take"), Id),
				Line.From.Equals(Context.GuidedCursor(), 1.0));
		}

		// AND THE LABEL SAYS WHAT IT IS, because a dashed line with no words is a mark whose
		// meaning the player has to guess.
		TestTrue(*FString::Printf(TEXT("'%s' labels it"), Id), Sink.GuideLabels.Num() > 0);
	}

	return true;
}

/**
 * AND THE CLICK LANDS ON THE LINE, which is the other half of consuming a guide.
 *
 * A GUIDE DRAWN AND THEN NOT OBEYED IS A MARK WHOSE MEANING HAS GONE. Each tool commits
 * something different - a node, a corner, an entity, a threshold - so each is read back from
 * where it actually landed rather than through a shared abstraction that would have to be
 * invented for the test.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFreeStartClickLandsOnTheGuideTest,
	"Airside.Tool.FreeStartClickLandsOnTheGuide",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFreeStartClickLandsOnTheGuideTest::RunTest(const FString& Parameters)
{
	// 1. THE ROAD TOOL puts its first node on the line.
	{
		FFreeStart Start;
		if (!TestTrue(TEXT("an idle taxiway tool"), Begin(Start, TEXT("Taxiway")))) { return false; }

		const FToolContext Context = Start.At(OffTheLine);
		const FVector2D Guided = Context.GuidedCursor();
		const int32 Before = Start.TestWorld.Actor->Network->GetNodes().Num();

		Start.Tool->OnClick(Context);

		const TArray<FRoadNode>& Nodes = Start.TestWorld.Actor->Network->GetNodes();
		if (TestEqual(TEXT("the click placed one node"), Nodes.Num(), Before + 1))
		{
			TestTrue(TEXT("a road's first node lands on the guide, not under the cursor"),
				Nodes.Last().Position.Equals(Guided, 1.0));
			TestFalse(TEXT("and those two are not the same point in this fixture"),
				Guided.Equals(Context.Cursor, 1.0));
		}
	}

	// 2. THE APRON TOOL puts its first corner on it.
	{
		FFreeStart Start;
		if (!TestTrue(TEXT("an idle apron tool"), Begin(Start, TEXT("Apron")))) { return false; }

		const FToolContext Context = Start.At(OffTheLine);
		const FVector2D Guided = Context.GuidedCursor();

		Start.Tool->OnClick(Context);

		const FOutlineDrawTool* Outline = static_cast<const FOutlineDrawTool*>(Start.Tool);
		const TArrayView<const FVector2D> Corners = Outline->GetCorners();
		if (TestEqual(TEXT("the click placed one corner"), Corners.Num(), 1))
		{
			TestTrue(TEXT("an outline's first corner lands on the guide"),
				Corners[0].Equals(Guided, 1.0));
		}
	}

	// 3. THE STAND TOOL places its pose on it.
	{
		FFreeStart Start;
		if (!TestTrue(TEXT("an idle stand tool"), Begin(Start, TEXT("Stand")))) { return false; }

		const FToolContext Context = Start.At(OffTheLine);
		const FVector2D Guided = Context.GuidedCursor();

		Start.Tool->OnClick(Context);

		// HONOURED, NOT ASSUMED: with no stand definition in the content set PlaceEntity
		// legitimately places nothing, and asserting on an empty array would report the
		// content rather than the tool.
		const TArray<FEntityInstance>& Entities = Start.TestWorld.Actor->Network->GetEntities();
		if (Entities.Num() == 0)
		{
			AddInfo(TEXT("No stand definition resolves; the stand's click not checked"));
		}
		else
		{
			TestTrue(TEXT("a stand's pose lands on the guide"),
				Entities.Last().Position.Equals(Guided, 1.0));
		}
	}

	// 4. THE RUNWAY TOOL takes its first threshold from it. Read back through the tool's own
	// preview rather than the graph: one click lays no strip, so there is nothing in the
	// network yet - the threshold lives in the tool until the second click.
	{
		FFreeStart Start;
		if (!TestTrue(TEXT("an idle runway tool"), Begin(Start, TEXT("Runway")))) { return false; }

		const FToolContext First = Start.At(OffTheLine);
		const FVector2D Guided = First.GuidedCursor();
		Start.Tool->OnClick(First);

		TestFalse(TEXT("a threshold is down"), Start.Tool->IsIdle());

		// WITH THE GUIDE SUSPENDED for the second frame, so the line drawn back to the
		// threshold is the tool's own pending geometry and not another guide's.
		FFreeStartSink Sink;
		const FToolContext Second = Start.Session.MakeContext(
			Start.TestWorld.Actor, FVector2D(9000.0, 9000.0), Start.Tunables, false, false, true);
		Start.Tool->BuildPreview(Second, Sink);

		const bool bFromThreshold = Sink.Lines.ContainsByPredicate(
			[&Guided](const FFreeStartSink::FRecordedLine& L)
			{
				return L.From.Equals(Guided, 1.0) || L.To.Equals(Guided, 1.0);
			});
		TestTrue(TEXT("a runway's first threshold sits where the guide put it"), bFromThreshold);
	}

	return true;
}

/**
 * A TOOL THAT DID NOT OPT IN IS UNCHANGED BY ANY OF THIS.
 *
 * The other half of the ruling, and the half a permissive base would quietly undo. The fuel
 * depot's first click MUST land on a service road - it is already snap-constrained, and its
 * own anchor comment warns that a second rule about where that anchor may go would be a
 * second opinion. Measured through MakeContext, so it is the DRIVER's answer and not the
 * virtual's own report.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FToolsThatDidNotOptInGetNoFreeStartTest,
	"Airside.Tool.ToolsThatDidNotOptInGetNoFreeStart",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FToolsThatDidNotOptInGetNoFreeStartTest::RunTest(const FString& Parameters)
{
	for (const TCHAR* Id : { TEXT("FuelDepot"), TEXT("Guideline"), TEXT("HoldingPosition"), TEXT("Select") })
	{
		FFreeStart Start;
		if (!TestTrue(*FString::Printf(TEXT("an idle '%s' tool"), Id), Begin(Start, Id)))
		{
			return false;
		}

		// THE SAME CURSOR the opted-in tools got a guide at, so a "no guide" here is about the
		// tool and not about there being nothing to line up with.
		const FToolContext Context = Start.At(OffTheLine);
		TestFalse(*FString::Printf(TEXT("'%s' resolves no guide before its first click"), Id),
			Context.Guide.bActive);
		TestTrue(*FString::Printf(TEXT("so '%s' clicks exactly where the cursor is"), Id),
			Context.GuidedCursor().Equals(Context.Cursor, 1.0e-6));
	}

	return true;
}

#endif
