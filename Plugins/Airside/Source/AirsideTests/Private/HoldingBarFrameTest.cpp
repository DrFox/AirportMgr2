#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Build/HoldingPositionMarkingBuilder.h"
#include "Misc/AutomationTest.h"
#include "Model/HoldingBarFrame.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"
#include "Tool/GuidelineOverlay.h"
#include "Tool/RoadBuildTool.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the UNITY build - see HoldingPointToolTest.cpp's FM2HoldSink for why a
	// bare name here would collide with another test file's own.
	struct FHBFrameSink : IToolPreviewSink
	{
		struct FCross { FVector2D At; FVector2D Along; };
		TArray<FCross> Crosses;

		virtual void Marker(const FVector2D&, EPreviewStyle) override {}
		virtual void Line(const FVector2D&, const FVector2D&, EPreviewStyle) override {}
		virtual void CrossMark(const FVector2D& At, const FVector2D& Along, EPreviewStyle) override
		{
			Crosses.Add({ At, Along });
		}
		virtual void Label(const FVector2D&, const FString&, EPreviewStyle) override {}

		/** By POSITION, not by call order - two flagged nodes' bars must not be told apart by
		 *  which one the overlay happened to draw first. */
		const FVector2D* AlongAt(const FVector2D& Position) const
		{
			for (const FCross& Cross : Crosses)
			{
				if (Cross.At.Equals(Position, 1.0))
				{
					return &Cross.Along;
				}
			}
			return nullptr;
		}
	};

	/**
	 * Measures a painted bar's own Toward/Across/HalfWidth off the buffer it produced, the
	 * same projection Airside.Build.HoldingPositionMarking uses for its one hardcoded
	 * (due-north) case - generalised to whatever Frame says, so it can be checked against
	 * ANY node's geometry, not just one this test already knows the answer for.
	 */
	void MeasureBar(FAutomationTestBase& Test, const FRoadMeshBuffers& Buffers,
		const FVector2D& NodeAt, const FHoldingBarFrame& Frame, double ExpectedDepth, const TCHAR* Label)
	{
		double MaxAlong = -1.0e9, MinAlong = 1.0e9, MaxAcross = 0.0;
		for (const FVector3d& P : Buffers.Positions)
		{
			const FVector2D Offset(P.X - NodeAt.X, P.Y - NodeAt.Y);
			const double Along = FVector2D::DotProduct(Offset, Frame.Toward);
			MaxAlong = FMath::Max(MaxAlong, Along);
			MinAlong = FMath::Min(MinAlong, Along);
			MaxAcross = FMath::Max(MaxAcross, FMath::Abs(FVector2D::DotProduct(Offset, Frame.Across)));
		}
		Test.TestTrue(FString::Printf(TEXT("%s: pattern starts at the node (%.3f)"), Label, MinAlong),
			FMath::Abs(MinAlong) < 1.0e-6);
		Test.TestTrue(FString::Printf(TEXT("%s: pattern reaches the expected depth (%.3f of %.3f)"), Label, MaxAlong, ExpectedDepth),
			FMath::Abs(MaxAlong - ExpectedDepth) < 1.0e-6);
		Test.TestTrue(FString::Printf(TEXT("%s: bar spans exactly the frame's half width (%.3f of %.3f)"), Label, MaxAcross, Frame.HalfWidth),
			FMath::Abs(MaxAcross - Frame.HalfWidth) < 1.0e-6);
	}
}

/**
 * Issue #307: the overlay drew a holding bar's cross-mark along the guideline EDGE's chord to
 * whatever node sat on the other end, while the paint laid the same bar along the ROAD
 * SEGMENT's own tangent at the node's Origin - two evaluators a comment on the builder
 * claimed were "exactly" the same. Both must now agree because both call HoldingBarAt and
 * nothing else - measured here at the level of GuidelineOverlay::Draw and
 * FHoldingPositionMarkingBuilder::Build, not at HoldingBarAt alone, so a regression that
 * re-introduces a second evaluator in either shows up here.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHoldingBarFrameTest,
	"Airside.Model.HoldingBarFrame",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FHoldingBarFrameTest::RunTest(const FString& Parameters)
{
	// 1. THE SHAPE ITSELF: a derived node whose Origin resolves to a CURVED segment. Every
	//    fixture that existed before this issue declares its taxiway with AddStraightSegment,
	//    where the guideline chord and the segment's own tangent are the same ray whatever the
	//    setback - so none of them could have caught #307, and this one is deliberately added
	//    (per the brief: "if it passes on every existing fixture, add one where they differ").
	//    Control sits well off the A-B line, so GetOutgoingTangent (what the paint uses) and
	//    the straight chord to the far guideline node (what the OLD overlay used) point in
	//    genuinely different directions - #307's evidence exactly (RoadGuidelineBuilder.cpp's
	//    SetBack, and "on any curved segment the guideline chord and the road tangent differ").
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* Taxiway = TestProfiles::Taxiway();
		const FRoadNodeId A = Net->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId B = Net->AddNode(FVector2D(0.0, -20000.0));
		const FVector2D Control(10000.0, -10000.0);
		const FRoadSegmentId Seg = Net->AddSegment(A, B, Control, Taxiway);
		if (!TestTrue(TEXT("curved segment created"), Seg.IsSet())) { return false; }

		// Hand-placed rather than derived: this pins the ONE fact under test (tangent vs
		// chord) without depending on FRoadGuidelineBuilder's setback/flare pipeline.
		const FGuidelineNodeId NodeId = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
		FGuidelineEndRef Origin;
		Origin.Segment = Seg;
		Origin.bEndA = true;
		Net->SetGuidelineNodeOrigin(NodeId, Origin);
		TestTrue(TEXT("marked an intermediate holding position"), Net->SetIntermediateHoldingPosition(NodeId, true));

		// The taxiway's own incident guideline edge, chorded straight south to where an
		// un-curved derivation would put the far end - the OLD overlay's Along, whichever of
		// its two branches picked it up (DerivedFrom matches Origin.Segment for the first;
		// it is this node's only incident edge for the second).
		const FGuidelineNodeId FarId = Net->AddGuidelineNode(FVector2D(0.0, -20000.0));
		FGuidelineEdge Edge;
		Edge.A = NodeId;
		Edge.B = FarId;
		Edge.Control = (Net->GetGuidelineNode(NodeId)->Position + Net->GetGuidelineNode(FarId)->Position) * 0.5;
		Edge.AllowedTraffic = FTrafficMask::All();
		Edge.DerivedFrom = Seg;
		Net->AddGuidelineEdge(MoveTemp(Edge));

		const FHoldingBarFrame Frame = HoldingBarAt(*Net, NodeId);
		const FVector2D ExpectedToward = FVector2D(-10000.0, 10000.0).GetSafeNormal();
		TestTrue(TEXT("HoldingBarAt: Toward is the segment's own tangent, negated (into the junction)"),
			(Frame.Toward - ExpectedToward).IsNearlyZero(1.0e-9));
		const FVector2D NaiveChord(0.0, -1.0);
		TestTrue(TEXT("HoldingBarAt: NOT the straight chord to the far guideline node"),
			!(Frame.Toward - NaiveChord).IsNearlyZero(0.5));

		// OVERLAY: must draw this node's cross mark along the SAME Toward HoldingBarAt just
		// gave - the seam that goes red if GuidelineOverlay ever grows its own evaluator again.
		FHBFrameSink Sink;
		GuidelineOverlay::Draw(*Net, Sink);
		const FVector2D* Along = Sink.AlongAt(FVector2D(0.0, 0.0));
		if (TestNotNull(TEXT("overlay drew this node's cross mark"), Along))
		{
			TestTrue(TEXT("overlay's Along is bitwise HoldingBarAt's Toward"), *Along == Frame.Toward);
		}

		// PAINT: the bar it lays must be square to, and no wider than, the SAME frame.
		FRoadMeshBuffers Buffers;
		TestEqual(TEXT("one position painted"), FHoldingPositionMarkingBuilder::Build(*Net, 0.0, Buffers), 1);
		const double ExpectedDepth = FHoldingPositionMarkingBuilder::LineWidth;   // one dashed intermediate bar
		MeasureBar(*this, Buffers, Net->GetGuidelineNode(NodeId)->Position, Frame, ExpectedDepth, TEXT("curved case"));
	}

	// 2. THE RUNWAY-CROSSING FIXTURE named in #307's own test line: every flagged node on it -
	//    here, the one runway-holding position its crossing taxiway carries - gets the same
	//    treatment. This fixture's taxiway is STRAIGHT and hand-authored with no Origin, so it
	//    exercises HoldingBarAt's OTHER branch (the Incident fallback), not case 1's curved
	//    one - included because #307's test line names this fixture, not because it can
	//    reproduce the tangent-vs-chord defect (a straight, centred guideline never can - see
	//    case 1's comment). It still pins a real, smaller defect the same comment caused: the
	//    OLD overlay's fallback did not negate its "away" vector the way the OLD paint's did,
	//    so the two disagreed in SIGN even here.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FCrossingFixture Fixture = FCrossingFixture::Build(*Net);
		const FGuidelineNode* HNode = Net->GetGuidelineNode(Fixture.H);
		if (!TestNotNull(TEXT("H exists"), HNode)) { return false; }
		TestTrue(TEXT("H is a flagged runway-holding position"),
			HNode->HoldingPosition == EHoldingPositionKind::Runway);

		const FHoldingBarFrame Frame = HoldingBarAt(*Net, Fixture.H);
		TestTrue(TEXT("HoldingBarAt: H has a frame"), Frame.IsSet());

		FHBFrameSink Sink;
		GuidelineOverlay::Draw(*Net, Sink);
		const FVector2D* Along = Sink.AlongAt(HNode->Position);
		if (TestNotNull(TEXT("overlay drew H's cross mark"), Along))
		{
			TestTrue(TEXT("overlay's Along is bitwise HoldingBarAt's Toward"), *Along == Frame.Toward);
		}

		FRoadMeshBuffers Buffers;
		TestEqual(TEXT("one position painted"), FHoldingPositionMarkingBuilder::Build(*Net, 0.0, Buffers), 1);
		const double ExpectedDepth =
			4.0 * FHoldingPositionMarkingBuilder::LineWidth + 3.0 * FHoldingPositionMarkingBuilder::LineGap;
		MeasureBar(*this, Buffers, HNode->Position, Frame, ExpectedDepth, TEXT("runway-crossing fixture, H"));
	}

	return true;
}

#endif
