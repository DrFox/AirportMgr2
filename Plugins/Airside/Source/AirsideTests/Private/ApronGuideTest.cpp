#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadApron.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Solve/GuideArbiter.h"
#include "Tool/BuildSession.h"
#include "Tool/RoadBuildTool.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/SnapGuideChain.h"
#include "Tool/SnapGuideLabel.h"
#include "Tool/SnapGuideSettings.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * A square apron with its south-west corner at the origin, 8000 uu on a side.
	 *
	 * COUNTER-CLOCKWISE, as FApronSurface::Outline declares itself. A clockwise outline would
	 * reverse every edge direction and so flip which side the flush candidates land on - the
	 * test would still pass, measuring a mirror image of the rule.
	 */
	bool LayApron(ARoadNetworkActor* Actor)
	{
		IRoadEditTarget* Target = Actor;

		// A NODE FIRST, to bring the network into being - URoadEditFacade creates it lazily
		// inside PlaceNode, and AddApron does not. Same trap TestGuide::LayRunway records.
		Target->PlaceNode(FVector2D(-100000.0, -100000.0));

		const TArray<FVector2D> Outline = {
			FVector2D(0.0, 0.0), FVector2D(8000.0, 0.0),
			FVector2D(8000.0, 8000.0), FVector2D(0.0, 8000.0) };
		return Target->AddApron(Outline) != INDEX_NONE;
	}

	/** Only the relation named, only the Apron column. Nothing else can answer. */
	FSnapGuideSettings OnlyApron(SnapGuide::ERelation Relation)
	{
		FSnapGuideSettings Settings;
		Settings.bExtending = false;
		Settings.bLevelWith = Relation == SnapGuide::ERelation::LevelWith;
		Settings.bParallel = Relation == SnapGuide::ERelation::Parallel;
		Settings.bCollinear = Relation == SnapGuide::ERelation::Collinear;
		Settings.bAngledFrom = Relation == SnapGuide::ERelation::AngledFrom;
		Settings.bMatchingGap = false;
		Settings.bTaxiway = false;
		Settings.bServiceRoad = false;
		Settings.bRunway = false;
		Settings.bApron = true;
		Settings.bStand = false;
		Settings.bWorld = false;
		return Settings;
	}

	/** An anchor dragging a ROAD centreline, with asymmetric half-widths to catch a mirror. */
	FGuideAnchor RoadAnchorAt(const FVector2D& Origin)
	{
		FGuideAnchor Anchor;
		Anchor.Origin = Origin;
		Anchor.Reference = FVector2D(1.0, 0.0);
		Anchor.ReferenceAt = Origin - FVector2D(2000.0, 0.0);
		Anchor.ReferenceName = TEXT("this road");
		Anchor.Point = EDragPoint::Centreline;

		// DELIBERATELY UNEQUAL. A mirrored pair would be wrong on every off-centre cross-section
		// and right on every symmetric one, so a symmetric fixture cannot tell the two apart.
		Anchor.HalfWidthLeft = 900.0;
		Anchor.HalfWidthRight = 500.0;
		return Anchor;
	}
}

/**
 * AN APRON EDGE IS A DIRECTION TO POINT ALONG.
 *
 * The Apron column had four declared cells and no source behind any of them until 2026-09-20 -
 * bApron was a switch with nothing under it, the same state bOffset shipped in.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApronEdgeOffersItsDirectionTest,
	"Airside.Tool.ApronEdgeOffersItsDirection",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FApronEdgeOffersItsDirectionTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }
	if (!TestTrue(TEXT("the apron is laid"), LayApron(Actor))) { return false; }

	// SOUTH OF THE APRON'S SOUTH EDGE, which runs along +X. A cursor due east of the origin is
	// 0 degrees off that edge's direction, inside the 7 degree tolerance.
	const FGuideAnchor Anchor = RoadAnchorAt(FVector2D(2000.0, -3000.0));
	const FSnapGuideChain Chain;

	const SnapGuide::FResult Result = Chain.Resolve(*Actor->Network, Anchor,
		FVector2D(9000.0, -3000.0), SnapGuide::FResult(),
		OnlyApron(SnapGuide::ERelation::Parallel));

	if (!TestTrue(TEXT("an apron edge offers its direction"), Result.bActive)) { return false; }
	TestEqual(TEXT("against the Apron column"),
		static_cast<int32>(Result.Winners[0].Reference),
		static_cast<int32>(SnapGuide::EReference::Apron));
	TestEqual(TEXT("as a Parallel guide"),
		static_cast<int32>(Result.Winners[0].Relation),
		static_cast<int32>(SnapGuide::ERelation::Parallel));

	// ANGULAR, so no half-width applies: it constrains which way, never where.
	TestEqual(TEXT("judged by direction, so the drag's width is irrelevant"),
		static_cast<int32>(Result.Winners[0].Fit),
		static_cast<int32>(SnapGuide::EFit::Angular));

	return true;
}

/**
 * A ROAD LINES UP EDGE TO EDGE WITH AN APRON, NOT CENTRE TO EDGE.
 *
 * Raised in review of the grid design: "the centre of your road aligning with the edge of an
 * apron is probably not that useful, you want an edge of your road colinear with the edge of the
 * apron". This is the only cell where a CENTRELINE drag meets an extended BOUNDARY, so it is the
 * only place the displacement applies - design section 6.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCollinearAgainstAnApronIsFlushByHalfWidthTest,
	"Airside.Tool.CollinearAgainstAnApronIsFlushByHalfWidth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FCollinearAgainstAnApronIsFlushByHalfWidthTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }
	if (!TestTrue(TEXT("the apron is laid"), LayApron(Actor))) { return false; }

	const FGuideAnchor Anchor = RoadAnchorAt(FVector2D(2000.0, -3000.0));
	const FSnapGuideChain Chain;

	// THE SOURCE ASKED DIRECTLY, not through Resolve: the arbiter returns at most one winner per
	// fit kind, and the whole claim here is that TWO candidates exist for one edge.
	const FApronLineGuideSource Source;
	TArray<SnapGuide::FCandidate> Candidates;
	Source.Propose(*Actor->Network, Anchor, Anchor.Origin, Candidates);

	// The apron's south edge runs along +X through y = 0. Its two flush lines sit at y = +900
	// and y = -500 - the road's own half-widths, which are NOT equal.
	int32 Left = 0;
	int32 Right = 0;
	for (const SnapGuide::FCandidate& Candidate : Candidates)
	{
		if (!FMath::IsNearlyZero(Candidate.Direction.Y, 1.0e-6)) { continue; }
		if (FMath::IsNearlyEqual(Candidate.Through.Y, 900.0, 1.0)) { ++Left; }
		if (FMath::IsNearlyEqual(Candidate.Through.Y, -500.0, 1.0)) { ++Right; }
	}

	TestEqual(TEXT("the road's left edge can sit flush with the apron"), Left, 1);
	TestEqual(TEXT("and its right edge, at its OWN half-width"), Right, 1);

	// NOT ON THE EDGE ITSELF. A candidate through y = 0 would be the centre-to-edge alignment
	// this test exists to rule out, and it is what the source proposed before the displacement.
	int32 Centred = 0;
	for (const SnapGuide::FCandidate& Candidate : Candidates)
	{
		if (FMath::IsNearlyZero(Candidate.Direction.Y, 1.0e-6)
			&& FMath::IsNearlyZero(Candidate.Through.Y, 1.0))
		{
			++Centred;
		}
	}
	TestEqual(TEXT("and the road's CENTRE is never offered on the apron's edge"), Centred, 0);

	// A BOUNDARY DRAG IS NOT DISPLACED - an apron corner against another apron's edge is
	// boundary against boundary, and one line is then the honest answer rather than two.
	FGuideAnchor Corner = Anchor;
	Corner.Point = EDragPoint::Boundary;
	Corner.HalfWidthLeft = 0.0;
	Corner.HalfWidthRight = 0.0;

	TArray<SnapGuide::FCandidate> Undisplaced;
	Source.Propose(*Actor->Network, Corner, Corner.Origin, Undisplaced);

	int32 OnTheEdge = 0;
	for (const SnapGuide::FCandidate& Candidate : Undisplaced)
	{
		if (FMath::IsNearlyZero(Candidate.Direction.Y, 1.0e-6)
			&& FMath::IsNearlyZero(Candidate.Through.Y, 1.0))
		{
			++OnTheEdge;
		}
	}
	TestEqual(TEXT("a boundary drag gets the edge itself, once"), OnTheEdge, 1);

	return true;
}

/**
 * THE APRON COLUMN OFF SILENCES EVERY RELATION - the 2026-09-20 report, in its new column.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApronColumnOffSilencesEveryRelationTest,
	"Airside.Tool.ApronColumnOffSilencesEveryRelation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FApronColumnOffSilencesEveryRelationTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }
	if (!TestTrue(TEXT("the apron is laid"), LayApron(Actor))) { return false; }

	const FGuideAnchor Anchor = RoadAnchorAt(FVector2D(2000.0, -3000.0));
	const FSnapGuideChain Chain;

	// EVERY RELATION ON, THE APRON COLUMN OFF, and no other column on either - so the apron is
	// the only thing on the field that could answer, and nothing may.
	FSnapGuideSettings Settings;
	Settings.bExtending = false;
	Settings.bLevelWith = true;
	Settings.bParallel = true;
	Settings.bCollinear = true;
	Settings.bAngledFrom = true;
	Settings.bMatchingGap = true;
	Settings.bTaxiway = false;
	Settings.bServiceRoad = false;
	Settings.bRunway = false;
	Settings.bApron = false;
	Settings.bStand = false;
	Settings.bWorld = false;

	const FVector2D Cursor(9000.0, -3000.0);
	const SnapGuide::FResult Off = Chain.Resolve(
		*Actor->Network, Anchor, Cursor, SnapGuide::FResult(), Settings);
	TestFalse(TEXT("with the Apron column off, no relation answers"), Off.bActive);

	// CONTROL LEG: the drag was fine. Switch the column on and the apron answers.
	Settings.bApron = true;
	const SnapGuide::FResult On = Chain.Resolve(
		*Actor->Network, Anchor, Cursor, SnapGuide::FResult(), Settings);
	if (!TestTrue(TEXT("with it on, the apron answers"), On.bActive)) { return false; }
	TestEqual(TEXT("and what answers is the apron"),
		static_cast<int32>(On.Winners[0].Reference),
		static_cast<int32>(SnapGuide::EReference::Apron));

	return true;
}

/**
 * PLOTTED STANDS AND DEPOTS OFFER THEIR EDGES AS THE APRON DOES - 2026-09-24.
 *
 * Measured in PIE that day: the Stand column's only source read an entity's POSE, so a road
 * could be squared to where a stand pointed but never lined up with the stand's own EDGE, and a
 * fuel depot's plot offered nothing at all. Every test below walks BOTH kinds, because the label
 * is the only thing that differs and a plot source that served one would pass half a suite.
 *
 * THROUGH THE ROAD TOOL AND FBuildSession::MakeContext, the seam both drivers share, and never
 * at the anchor alone - FreeStartGuideTest records why an anchor that is right proves nothing.
 */
namespace PlotEdgeGuideTest
{
	/** One kind of plot, and the word its label must carry. */
	struct FPlotKind
	{
		EServiceRole Role;
		const TCHAR* Word;
	};

	const FPlotKind Kinds[] = {
		{ EServiceRole::Aircraft, TEXT("the stand's") },
		{ EServiceRole::Fuel, TEXT("the fuel depot's") },
	};

	/**
	 * A 40 m x 60 m plot with its south-west corner at the origin, COUNTER-CLOCKWISE like an
	 * apron's. Its east edge is x = 4000, its south edge y = 0.
	 *
	 * THE POSE IS TURNED 20 DEGREES OFF THE BOX, which no real stand would be: FAlignedGuideSource
	 * proposes the pose's own 0/45/90/135 in the same Parallel x Stand cell, and a pose square to
	 * the box would offer the very direction the edge does - so a Parallel test could pass with no
	 * edge source at all. 20 degrees keeps every pose line outside the 7 degree tolerance.
	 */
	int32 PlacePlot(ARoadNetworkActor* Actor, EServiceRole Role)
	{
		// A NODE FIRST, to bring the network into being - AddApron's trap, recorded in LayApron.
		// Far off, so nothing it offers is in reach of the plot.
		IRoadEditTarget* Target = Actor;
		Target->PlaceNode(FVector2D(-100000.0, -100000.0));
		if (Actor->Network == nullptr) { return INDEX_NONE; }

		FEntityPlacement Placement;
		Placement.Definition = Role == EServiceRole::Fuel
			? UEntityDefinition::MakeFuelDepotTransient()
			: NewObject<UEntityDefinition>(GetTransientPackage());
		Placement.Position = FVector2D(2000.0, 3000.0);
		Placement.Heading = FMath::DegreesToRadians(20.0);
		Placement.PoseRole = Role;
		Placement.Outline = { FVector2D(0.0, 0.0), FVector2D(4000.0, 0.0),
			FVector2D(4000.0, 6000.0), FVector2D(0.0, 6000.0) };

		if (!Actor->Network->PlaceEntity(Placement).IsSet()) { return INDEX_NONE; }
		return Actor->Network->GetEntities().Num() - 1;
	}

	/**
	 * Only the relation named, and only the Stand column. A service road on the field would
	 * otherwise answer for its own node, and the world axes for everything angular.
	 */
	FSnapGuideSettings OnlyStand(SnapGuide::ERelation Relation)
	{
		FSnapGuideSettings Settings;
		Settings.bExtending = false;
		Settings.bLevelWith = Relation == SnapGuide::ERelation::LevelWith;
		Settings.bParallel = Relation == SnapGuide::ERelation::Parallel;
		Settings.bCollinear = Relation == SnapGuide::ERelation::Collinear;
		Settings.bAngledFrom = Relation == SnapGuide::ERelation::AngledFrom;
		Settings.bMatchingGap = false;
		Settings.bTaxiway = false;
		Settings.bServiceRoad = false;
		Settings.bRunway = false;
		Settings.bApron = false;
		Settings.bStand = true;
		Settings.bWorld = false;
		return Settings;
	}

	/** A service-road session beside one plot of Kind, with only Relation x Stand live. */
	struct FRoadBesidePlot
	{
		FAirsideTestWorld TestWorld;
		FBuildSession Session;
		FBuildSessionTunables Tunables;
		IBuildTool* Tool = nullptr;
		int32 Plot = INDEX_NONE;

		FToolContext At(const FVector2D& Where) const
		{
			return Session.MakeContext(TestWorld.Actor, Where, Tunables, false, false);
		}
	};

	/** BY ID, never a literal index - the next tool added to the registry moves every one. */
	bool Begin(FRoadBesidePlot& Out, EServiceRole Role, SnapGuide::ERelation Relation)
	{
		if (Out.TestWorld.World == nullptr || Out.TestWorld.Actor == nullptr) { return false; }
		Out.Plot = PlacePlot(Out.TestWorld.Actor, Role);
		if (Out.Plot == INDEX_NONE) { return false; }

		int32 Road = INDEX_NONE;
		const TConstArrayView<FToolRegistration> Registry = ToolRegistry();
		for (int32 Index = 0; Index < Registry.Num(); ++Index)
		{
			if (Registry[Index].Id == FName(TEXT("Road"))) { Road = Index; }
		}
		if (Road == INDEX_NONE) { return false; }

		Out.Tunables = Out.TestWorld.Actor->MakeTunables(10000.0);
		Out.Tunables.GuideSources = OnlyStand(Relation);
		Out.Session.SelectTool(Road);
		Out.Tool = Out.Session.GetActiveTool();
		return Out.Tool != nullptr;
	}

	/** The winner of this cell, if one holds. At most two winners, one per fit kind. */
	const SnapGuide::FCandidate* WinnerIn(const FToolContext& Context,
		SnapGuide::ERelation Relation, SnapGuide::EReference Reference)
	{
		return Context.Guide.Winners.FindByPredicate([Relation, Reference](const SnapGuide::FCandidate& C)
			{
				return C.Relation == Relation && C.Reference == Reference;
			});
	}
}

/**
 * A SERVICE ROAD DRAWN ALONG A STAND'S SIDE PUTS ITS KERB ON THE STAND'S EDGE.
 *
 * The first click is south-east of the plot; the second is dragged up beside its east edge,
 * just outside the line the road's flank would sit on. The guide must be the edge's, it must
 * name the kind, and the guided point must be exactly one half-width off the edge - FLUSH, as
 * against an apron, never centre-on-edge.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadLinesUpWithAPlotEdgeTest,
	"Airside.Tool.RoadLinesUpWithAPlotEdge",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadLinesUpWithAPlotEdgeTest::RunTest(const FString& Parameters)
{
	for (const PlotEdgeGuideTest::FPlotKind& Kind : PlotEdgeGuideTest::Kinds)
	{
		PlotEdgeGuideTest::FRoadBesidePlot Road;
		if (!TestTrue(*FString::Printf(TEXT("a road session beside %s plot"), Kind.Word),
			PlotEdgeGuideTest::Begin(Road, Kind.Role, SnapGuide::ERelation::Collinear)))
		{
			return false;
		}

		Road.Tool->OnClick(Road.At(FVector2D(7000.0, -4000.0)));

		// THE WIDTH THE TOOL ITSELF REPORTS, not a figure typed here: the profile decides it.
		FGuideAnchor Anchor;
		if (!TestTrue(TEXT("the road describes its drag"),
			Road.Tool->DescribeGuideAnchor(Road.TestWorld.Actor->Network, Road.TestWorld.Actor, Anchor)))
		{
			return false;
		}
		if (!TestTrue(TEXT("with a real width, or flush means nothing"), Anchor.HalfWidthRight > 0.0))
		{
			return false;
		}

		// OUTSIDE THE EAST EDGE, 60 uu beyond the line the road's flank would sit on.
		const FVector2D Cursor(4000.0 + Anchor.HalfWidthRight + 60.0, -1500.0);
		const FToolContext Context = Road.At(Cursor);
		const SnapGuide::FCandidate* Winner = PlotEdgeGuideTest::WinnerIn(Context,
			SnapGuide::ERelation::Collinear, SnapGuide::EReference::Stand);
		if (!TestNotNull(*FString::Printf(TEXT("%s edge puts the road in line with it"), Kind.Word), Winner))
		{
			continue;
		}

		TestTrue(*FString::Printf(TEXT("labelled '%s' as %s edge"), *Winner->Description, Kind.Word),
			Winner->Description.Contains(FString::Printf(TEXT("%s edge"), Kind.Word)));

		// ONE HALF-WIDTH OFF THE EDGE, either side's figure - which side PerpCCW calls left is
		// the apron test's business, not this one's.
		const double OffEdge = FMath::Abs(Context.GuidedCursor().X - 4000.0);
		TestTrue(*FString::Printf(TEXT("the road's EDGE is on %s edge (%.1f uu off it), not its centre"),
				Kind.Word, OffEdge),
			FMath::IsNearlyEqual(OffEdge, Anchor.HalfWidthRight, 1.0)
				|| FMath::IsNearlyEqual(OffEdge, Anchor.HalfWidthLeft, 1.0));
	}
	return true;
}

/**
 * A PLOT'S EDGE IS A DIRECTION TO DRAW ALONG - the Parallel member of the family.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadRunsParallelToAPlotEdgeTest,
	"Airside.Tool.RoadRunsParallelToAPlotEdge",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadRunsParallelToAPlotEdgeTest::RunTest(const FString& Parameters)
{
	for (const PlotEdgeGuideTest::FPlotKind& Kind : PlotEdgeGuideTest::Kinds)
	{
		PlotEdgeGuideTest::FRoadBesidePlot Road;
		if (!TestTrue(*FString::Printf(TEXT("a road session beside %s plot"), Kind.Word),
			PlotEdgeGuideTest::Begin(Road, Kind.Role, SnapGuide::ERelation::Parallel)))
		{
			return false;
		}

		// FROM SOUTH-EAST OF THE PLOT, dragged north: about 0.4 degrees off the east edge's
		// direction, and 20 or more off every line the turned pose offers.
		Road.Tool->OnClick(Road.At(FVector2D(7000.0, -2000.0)));
		const FToolContext Context = Road.At(FVector2D(7040.0, 4000.0));

		const SnapGuide::FCandidate* Winner = PlotEdgeGuideTest::WinnerIn(Context,
			SnapGuide::ERelation::Parallel, SnapGuide::EReference::Stand);
		if (!TestNotNull(*FString::Printf(TEXT("%s edge offers its direction"), Kind.Word), Winner))
		{
			continue;
		}
		// "parallel to" OR "square to": a rectangle's east edge and its south edge's square are
		// the same line, and which of the two equal candidates the arbiter keeps is not this
		// test's claim. That the edge answers, under its kind's name, is.
		TestTrue(*FString::Printf(TEXT("'%s' names %s edge"), *Winner->Description, Kind.Word),
			Winner->Description == FString::Printf(TEXT("parallel to %s edge"), Kind.Word)
				|| Winner->Description == FString::Printf(TEXT("square to %s edge"), Kind.Word));
		TestTrue(TEXT("and the guided point runs due north, along the edge"),
			FMath::IsNearlyEqual(Context.GuidedCursor().X, 7000.0, 1.0));
	}
	return true;
}

/**
 * BEFORE THE FIRST CLICK, A ROAD CAN START LEVEL WITH A PLOT'S CORNER.
 *
 * The free start has no direction, so FApronCornerGuideSource used to propose nothing for it -
 * for an apron as for a plot. Since 2026-09-24 the corner's own edge is the axis. The cursor is
 * east of the plot, 80 uu north of the line its south edge lies on.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFreeStartIsLevelWithAPlotCornerTest,
	"Airside.Tool.FreeStartIsLevelWithAPlotCorner",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFreeStartIsLevelWithAPlotCornerTest::RunTest(const FString& Parameters)
{
	for (const PlotEdgeGuideTest::FPlotKind& Kind : PlotEdgeGuideTest::Kinds)
	{
		PlotEdgeGuideTest::FRoadBesidePlot Road;
		if (!TestTrue(*FString::Printf(TEXT("a road session beside %s plot"), Kind.Word),
			PlotEdgeGuideTest::Begin(Road, Kind.Role, SnapGuide::ERelation::LevelWith)))
		{
			return false;
		}
		if (!TestTrue(TEXT("the road tool is idle - this is a free start"), Road.Tool->IsIdle()))
		{
			return false;
		}

		const FToolContext Context = Road.At(FVector2D(7000.0, 80.0));
		const SnapGuide::FCandidate* Winner = PlotEdgeGuideTest::WinnerIn(Context,
			SnapGuide::ERelation::LevelWith, SnapGuide::EReference::Stand);
		if (!TestNotNull(*FString::Printf(TEXT("%s corner guides the first click"), Kind.Word), Winner))
		{
			continue;
		}
		TestEqual(*FString::Printf(TEXT("named as %s corner, exactly"), Kind.Word),
			Winner->Description, FString::Printf(TEXT("level with %s corner"), Kind.Word));
		TestTrue(TEXT("and the first click lands level with it"),
			FMath::IsNearlyEqual(Context.GuidedCursor().Y, 0.0, 1.0));
	}
	return true;
}

/**
 * THE STAND BUTTON OWNS EVERY PLOT LINE, AND THE APRON BUTTON KEEPS ITS OWN.
 *
 * An apron and a stand side by side, every relation on. With Stand off, not one candidate may
 * be tagged Stand - and the apron's must still be there, labelled as the apron's, so the two
 * instances of each source are seen not to have leaked into each other's column. The control
 * leg switches Stand on and requires the plot's edges to appear, which is what makes the first
 * half a measurement.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandColumnOffSilencesPlotEdgesTest,
	"Airside.Tool.StandColumnOffSilencesPlotEdges",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandColumnOffSilencesPlotEdgesTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }
	if (!TestTrue(TEXT("a stand is placed"),
		PlotEdgeGuideTest::PlacePlot(Actor, EServiceRole::Aircraft) != INDEX_NONE))
	{
		return false;
	}
	IRoadEditTarget* Target = Actor;
	if (!TestTrue(TEXT("and an apron east of it"), Target->AddApron({
		FVector2D(9000.0, 0.0), FVector2D(15000.0, 0.0),
		FVector2D(15000.0, 6000.0), FVector2D(9000.0, 6000.0) }) != INDEX_NONE))
	{
		return false;
	}

	// BETWEEN THE TWO, with a direction of its own so every relation - LevelWith's gesture axes
	// included - has something to say.
	FGuideAnchor Anchor = RoadAnchorAt(FVector2D(6500.0, -2000.0));
	const FVector2D Cursor(6500.0, 3000.0);

	FSnapGuideSettings Settings;
	Settings.bExtending = false;
	Settings.bLevelWith = true;
	Settings.bParallel = true;
	Settings.bCollinear = true;
	Settings.bAngledFrom = true;
	Settings.bMatchingGap = true;
	Settings.bTaxiway = false;
	Settings.bServiceRoad = false;
	Settings.bRunway = false;
	Settings.bApron = true;
	Settings.bStand = false;
	Settings.bWorld = false;

	const FSnapGuideChain Chain;
	TArray<SnapGuide::FCandidate> Off;
	Chain.ProposeAll(*Actor->Network, Anchor, Cursor, Settings, Off);

	int32 StandTagged = 0;
	int32 Apron = 0;
	for (const SnapGuide::FCandidate& Candidate : Off)
	{
		if (Candidate.Reference == SnapGuide::EReference::Stand) { ++StandTagged; }
		if (Candidate.Reference != SnapGuide::EReference::Apron) { continue; }
		++Apron;
		const FString Label = SnapGuide::Describe(*Actor->Network, Anchor, Candidate.Label);
		TestTrue(*FString::Printf(TEXT("an apron line still names the apron ('%s')"), *Label),
			Label.Contains(TEXT("apron")));
	}
	TestEqual(TEXT("with the Stand column off, no plot line is offered"), StandTagged, 0);
	TestTrue(TEXT("while the apron's lines still are"), Apron > 0);

	// CONTROL LEG: the same field and drag, Stand on - the plot's EDGES answer, not only its pose.
	Settings.bStand = true;
	TArray<SnapGuide::FCandidate> On;
	Chain.ProposeAll(*Actor->Network, Anchor, Cursor, Settings, On);
	const int32 PlotEdges = On.FilterByPredicate([&Actor, &Anchor](const SnapGuide::FCandidate& C)
		{
			return C.Reference == SnapGuide::EReference::Stand
				&& SnapGuide::Describe(*Actor->Network, Anchor, C.Label).Contains(TEXT("the stand's"));
		}).Num();
	TestTrue(TEXT("with it on, the stand's edges and corners are offered"), PlotEdges > 0);

	return true;
}

#endif
