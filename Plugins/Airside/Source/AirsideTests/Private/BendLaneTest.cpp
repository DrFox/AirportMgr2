#include "CoreMinimal.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadMeshBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Content/AirsideContent.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/Vehicle.h"
#include "Profiles/RoadDesignVehicles.h"
#include "Profiles/RoadProfile.h"
#include "Solve/GuidelineGeom.h"
#include "Testing/BendProbe.h"

#if WITH_DEV_AUTOMATION_TESTS

// BEND LANES (user ruling 2026-09-25). At a two-arm bend the pavement is filleted on both sides,
// and the lane turn paths are measured here against it: their radius, whether they are
// concentric with the pavement, and how far the tier's design vehicle leaves the tarmac.

namespace BendLane
{
	TArray<URoadProfile*> Tiers()
	{
		TArray<URoadProfile*> Out;
		const UAirsideContent* Content = UAirsideSettings::GetContent();
		if (Content == nullptr || Content->ServiceRoadProfiles.Num() != 3)
		{
			return Out;
		}
		for (const TSoftObjectPtr<URoadProfile>& Tier : Content->ServiceRoadProfiles)
		{
			Out.Add(Tier.LoadSynchronous());
		}
		return Out;
	}

	const TCHAR* const Names[3] = { TEXT("Narrow"), TEXT("Standard"), TEXT("Wide") };

	/**
	 * THE BEFORE FIGURES, measured 2026-09-25 on this fixture (Census) while each lane turn was one
	 * quadratic cut to cut: MinRadius of the inner lane's turn and the outer lane's, per tier. The
	 * ruling forbids a tighter turn, so these are floors, not expectations.
	 */
	constexpr double BeforeInner[3] = { 726.0, 743.0, 853.0 };
	constexpr double BeforeOuter[3] = { 938.0, 991.0, 1171.0 };

	/** The lane turn whose line starts nearer the inner fillet's centre, and the other. */
	void SplitLanes(const TArray<BendProbe::FTurnChain>& Turns, const FVector2D& Centre,
		const BendProbe::FTurnChain*& OutInner, const BendProbe::FTurnChain*& OutOuter)
	{
		OutInner = OutOuter = nullptr;
		if (Turns.Num() != 2) { return; }
		const bool bFirstInner = FVector2D::Distance(Turns[0].Path[0], Centre) < FVector2D::Distance(Turns[1].Path[0], Centre);
		OutInner = &Turns[bFirstInner ? 0 : 1];
		OutOuter = &Turns[bFirstInner ? 1 : 0];
	}

	/** The samples of a chain's CURVED pieces only: the arc, without any straight lead a widened bend adds. */
	TArray<FVector2D> ArcSamples(const URoadNetwork& Net, const BendProbe::FTurnChain& Chain)
	{
		TArray<FVector2D> Out;
		for (const FGuidelineEdgeId Id : Chain.Pieces)
		{
			const FGuidelineEdge* Piece = Net.GetGuidelineEdge(Id);
			if (Piece == nullptr || Piece->MinRadius <= 0.0) { continue; }
			GuidelineGeom::Sample(Net.GetGuidelineNode(Piece->A)->Position, Piece->Control, Net.GetGuidelineNode(Piece->B)->Position, Out);
		}
		return Out;
	}

	/** The tightest CURVED piece of a chain; a straight piece's 0 means "no curve", not "no radius". */
	double Tightest(const URoadNetwork& Net, const BendProbe::FTurnChain& Chain)
	{
		double Out = TNumericLimits<double>::Max();
		for (const FGuidelineEdgeId Id : Chain.Pieces)
		{
			const FGuidelineEdge* Piece = Net.GetGuidelineEdge(Id);
			if (Piece != nullptr && Piece->MinRadius > 0.0) { Out = FMath::Min(Out, Piece->MinRadius); }
		}
		return Out;
	}
	/** Where the fillet alone would cut the arms: its tangent point's distance from the node, along the arm. */
	double FilletCutOf(const URoadNetwork& Net, FRoadNodeId Corner, const RoadGeom::FFillet& Inner)
	{
		const FVector2D Node = Net.GetNode(Corner)->Position;
		const FVector2D Along = (Inner.TangentA - Inner.Corner).GetSafeNormal();
		return FVector2D::DotProduct(Inner.TangentA - Node, Along);
	}

	/** The mean distance of a chain's arc samples from Centre - the arc's radius when it is one. */
	double MeanRadius(const BendProbe::FTurnChain& Chain, const FVector2D& Centre)
	{
		double Sum = 0.0;
		for (const FVector2D& P : Chain.Path) { Sum += FVector2D::Distance(P, Centre); }
		return Chain.Path.Num() > 0 ? Sum / Chain.Path.Num() : 0.0;
	}

	/** A plain right angle, (0,0) -> (8000,0) -> (8000,8000), derived the production way. */
	struct FBend
	{
		URoadNetwork* Net = nullptr;
		FRoadSolveResult Solved;
		FRoadNodeId Corner;
	};

	FBend Build(URoadProfile* Profile, URoadProfile* NorthProfile = nullptr)
	{
		FBend Out;
		Out.Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadNodeId West = Out.Net->AddNode(FVector2D(0.0, 0.0));
		Out.Corner = Out.Net->AddNode(FVector2D(8000.0, 0.0));
		const FRoadNodeId North = Out.Net->AddNode(FVector2D(8000.0, 8000.0));
		Out.Net->AddStraightSegment(West, Out.Corner, Profile);
		Out.Net->AddStraightSegment(Out.Corner, North, NorthProfile != nullptr ? NorthProfile : Profile);
		const FRoadDesignVehicles Designs = UAirsideSettings::ResolveRoadDesignVehicles();
		Out.Solved = FRoadNetworkSolver::SolveAll(*Out.Net, 12, &Designs);
		FRoadGuidelineBuilder::Build(*Out.Net, Out.Solved, Designs);
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBendLaneCensusTest, "Airside.Build.BendLanes.Census",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBendLaneCensusTest::RunTest(const FString& Parameters)
{
	using namespace BendLane;
	const TArray<URoadProfile*> Profiles = Tiers();
	if (!TestTrue(TEXT("the content set has its three service-road tiers, and they load"),
		Profiles.Num() == 3 && !Profiles.Contains(nullptr))) { return false; }
	const FVehicle Rig = UAirsideSettings::ResolveRigVehicle();
	const FVehicle Bowser = UAirsideSettings::ResolveDefaultVehicle();
	const FVehicle Utility = UAirsideSettings::ResolveUtilityTowVehicle();
	for (int32 Tier = 0; Tier < 3; ++Tier)
	{
		const FBend Bend = Build(Profiles[Tier]);
		RoadGeom::FFillet Inner, Outer;
		if (!TestTrue(TEXT("the bend has an inner and an outer fillet"), BendProbe::Fillets(Bend.Solved, Bend.Corner, Inner, Outer))) { continue; }
		const FJunctionResult& Junction = Bend.Solved.NodeResults[Bend.Corner.Index];
		const BendProbe::FPavement Pavement = BendProbe::PavementAll(*Bend.Net, Bend.Solved);
		UE_LOG(LogTemp, Display, TEXT("BendCensus %s: width %.0f, design lock rig %.0f bowser %.0f; inner fillet R %.0f centre (%.1f, %.1f); outer fillet R %.0f centre (%.1f, %.1f); cut %.1f / %.1f"),
			Names[Tier], Profiles[Tier]->GetTotalWidth(), Rig.Chassis.TightestFollowableRadius(), Bowser.Chassis.TightestFollowableRadius(),
			Inner.Radius, Inner.Centre.X, Inner.Centre.Y, Outer.Radius, Outer.Centre.X, Outer.Centre.Y,
			Junction.Arms[0].CutDistance, Junction.Arms[1].CutDistance);
		for (const FProfileGuideline& Lane : Profiles[Tier]->Guidelines)
		{
			UE_LOG(LogTemp, Display, TEXT("BendCensus %s: lane offset %.0f width %.0f"), Names[Tier], Lane.CentreOffset, Lane.Width);
		}
		const TArray<BendProbe::FTurnChain> Turns = BendProbe::TurnsAt(*Bend.Net, Bend.Corner);
		TestEqual(TEXT("two lane turns at a two-lane bend"), Turns.Num(), 2);
		for (const BendProbe::FTurnChain& Turn : Turns)
		{
			double InMin, InMax, OutMin, OutMax;
			BendProbe::RadiusAbout(Turn, Inner.Centre, InMin, InMax);
			BendProbe::RadiusAbout(Turn, Outer.Centre, OutMin, OutMax);
			double LeastIn = TNumericLimits<double>::Max(), LeastOut = TNumericLimits<double>::Max();
			for (int32 I = 0; I < Turn.Path.Num(); ++I)
			{
				if (Turn.ClearIn[I] < 0.0) { continue; }   // a straight lead carries none
				LeastIn = FMath::Min(LeastIn, Turn.ClearIn[I]);
				LeastOut = FMath::Min(LeastOut, Turn.ClearOut[I]);
			}
			const BendProbe::FOverrun RigOff = BendProbe::Overrun(Turn, Rig, Pavement);
			const BendProbe::FOverrun BowserOff = BendProbe::Overrun(Turn, Bowser, Pavement);
			const BendProbe::FOverrun UtilityOff = BendProbe::Overrun(Turn, Utility, Pavement);
			UE_LOG(LogTemp, Display, TEXT("BendCensus %s %s turn: %d piece(s), MinRadius %.0f; about inner centre %.0f..%.0f, about outer centre %.0f..%.0f; least clear in %.0f out %.0f; from (%.0f, %.0f) to (%.0f, %.0f)"),
				Names[Tier], InMin < Inner.Radius + 0.5 * Profiles[Tier]->GetTotalWidth() ? TEXT("inner-lane") : TEXT("outer-lane"), Turn.Pieces.Num(), Tightest(*Bend.Net, Turn), InMin, InMax, OutMin, OutMax,
				LeastIn, LeastOut, Turn.Path[0].X, Turn.Path[0].Y, Turn.Path.Last().X, Turn.Path.Last().Y);
			UE_LOG(LogTemp, Display, TEXT("BendCensus %s %s turn: off the tarmac - rig inner %.0f outer %.0f (traced %d) at (%.0f, %.0f); bowser inner %.0f outer %.0f; utility inner %.0f outer %.0f"),
				Names[Tier], InMin < Inner.Radius + 0.5 * Profiles[Tier]->GetTotalWidth() ? TEXT("inner-lane") : TEXT("outer-lane"), RigOff.Inner, RigOff.Outer, RigOff.bTraced ? 1 : 0,
				RigOff.InnerAt.X, RigOff.InnerAt.Y, BowserOff.Inner, BowserOff.Outer, UtilityOff.Inner, UtilityOff.Outer);
		}
	}
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBendLaneConcentricTest, "Airside.Build.BendLanes.ConcentricWithPavement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBendLaneConcentricTest::RunTest(const FString& Parameters)
{
	// THE LANES FOLLOW THE PAVEMENT (ruling 2026-09-25): each lane's turn is an arc about the
	// inner fillet's centre at that lane's own distance from the inner edge, so the two lanes'
	// radii differ by exactly their spacing and each keeps the distance off the edge it keeps on
	// the straights. And it joins the straights TANGENTLY: a kink at a lane end is a sharp vertex
	// FSpeedProfile crawls.
	using namespace BendLane;
	const TArray<URoadProfile*> Profiles = Tiers();
	if (!TestTrue(TEXT("the content set has its three service-road tiers, and they load"),
		Profiles.Num() == 3 && !Profiles.Contains(nullptr))) { return false; }
	for (int32 Tier = 0; Tier < 3; ++Tier)
	{
		const FBend Bend = Build(Profiles[Tier]);
		RoadGeom::FFillet Inner, Outer;
		if (!TestTrue(TEXT("the bend has an inner and an outer fillet"), BendProbe::Fillets(Bend.Solved, Bend.Corner, Inner, Outer))) { continue; }
		const TArray<BendProbe::FTurnChain> Turns = BendProbe::TurnsAt(*Bend.Net, Bend.Corner);
		const BendProbe::FTurnChain* InLane = nullptr;
		const BendProbe::FTurnChain* OutLane = nullptr;
		SplitLanes(Turns, Inner.Centre, InLane, OutLane);
		if (!TestTrue(FString::Printf(TEXT("%s: two lane turns"), Names[Tier]), InLane != nullptr && OutLane != nullptr)) { continue; }
		const double Half = 0.5 * Profiles[Tier]->GetTotalWidth();
		const double Offset = FMath::Abs(Profiles[Tier]->Guidelines[0].CentreOffset);
		double Radius[2] = { 0.0, 0.0 };
		for (int32 Which = 0; Which < 2; ++Which)
		{
			const BendProbe::FTurnChain& Lane = Which == 0 ? *InLane : *OutLane;
			const TCHAR* Name = Which == 0 ? TEXT("inner") : TEXT("outer");
			const TArray<FVector2D> Arc = ArcSamples(*Bend.Net, Lane);
			TestTrue(FString::Printf(TEXT("%s %s lane: laid as an arc in pieces (%d piece(s))"), Names[Tier], Name, Lane.Pieces.Num()),
				Lane.Pieces.Num() >= 2 && Arc.Num() > 0);
			double Min = TNumericLimits<double>::Max(), Max = 0.0, Sum = 0.0;
			for (const FVector2D& P : Arc)
			{
				const double D = FVector2D::Distance(P, Inner.Centre);
				Min = FMath::Min(Min, D);
				Max = FMath::Max(Max, D);
				Sum += D;
			}
			Radius[Which] = Arc.Num() > 0 ? Sum / Arc.Num() : 0.0;
			// 1 uu: a quadratic across 22.5 degrees of a 10-17 m arc strays 0.2-0.3 uu off the circle.
			TestTrue(FString::Printf(TEXT("%s %s lane: every arc sample the same distance from the inner fillet's centre (%.2f..%.2f)"),
				Names[Tier], Name, Min, Max), Arc.Num() > 0 && Max - Min <= 1.0);
			const double Expected = Inner.Radius + (Which == 0 ? Half - Offset : Half + Offset);
			TestTrue(FString::Printf(TEXT("%s %s lane: its radius is the fillet's plus the lane's distance from the inner edge (%.1f vs %.1f)"),
				Names[Tier], Name, Radius[Which], Expected), FMath::Abs(Radius[Which] - Expected) <= 1.0);

			// Tangent to the lanes at both ends and to itself at every joint.
			FVector2D LaneIn = FVector2D::ZeroVector, LaneOut = FVector2D::ZeroVector;
			if (!TestTrue(TEXT("both lane ends have their lane"), BendProbe::LaneAt(*Bend.Net, Lane.From, LaneIn)
				&& BendProbe::LaneAt(*Bend.Net, Lane.To, LaneOut))) { continue; }
			FVector2D Prev = (Lane.Path[0] - LaneIn).GetSafeNormal();
			double WorstKink = 0.0;
			for (const FGuidelineEdgeId Id : Lane.Pieces)
			{
				const FGuidelineEdge* Piece = Bend.Net->GetGuidelineEdge(Id);
				const FVector2D A = Bend.Net->GetGuidelineNode(Piece->A)->Position;
				const FVector2D B = Bend.Net->GetGuidelineNode(Piece->B)->Position;
				WorstKink = FMath::Max(WorstKink, RoadGeom::AngleBetween(Prev, GuidelineGeom::Tangent(A, Piece->Control, B, 0.0)));
				Prev = GuidelineGeom::Tangent(A, Piece->Control, B, 1.0);
			}
			WorstKink = FMath::Max(WorstKink, RoadGeom::AngleBetween(Prev, (LaneOut - Lane.Path.Last()).GetSafeNormal()));
			TestTrue(FString::Printf(TEXT("%s %s lane: tangent to both lanes and at every joint (worst %.5f rad)"), Names[Tier], Name, WorstKink),
				WorstKink < 1e-3);
		}
		// Concentric with each other as with the edge: the radii differ by the lanes' spacing.
		TestTrue(FString::Printf(TEXT("%s: the lanes' radii differ by the lane spacing (%.1f vs %.1f)"), Names[Tier],
			Radius[1] - Radius[0], 2.0 * Offset), FMath::Abs(Radius[1] - Radius[0] - 2.0 * Offset) <= 1.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBendLaneNeverTighterTest, "Airside.Build.BendLanes.NeverTighter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBendLaneNeverTighterTest::RunTest(const FString& Parameters)
{
	// NEVER TIGHTER THAN BEFORE (the ruling's own condition): the tightest piece of each lane's arc
	// delivers at least what the one quadratic did, per tier - and clears the tier's design
	// vehicle's lock.
	using namespace BendLane;
	const TArray<URoadProfile*> Profiles = Tiers();
	if (!TestTrue(TEXT("the content set has its three service-road tiers, and they load"),
		Profiles.Num() == 3 && !Profiles.Contains(nullptr))) { return false; }
	const FRoadDesignVehicles Designs = UAirsideSettings::ResolveRoadDesignVehicles();
	for (int32 Tier = 0; Tier < 3; ++Tier)
	{
		const FBend Bend = Build(Profiles[Tier]);
		RoadGeom::FFillet Inner, Outer;
		if (!TestTrue(TEXT("the bend has an inner and an outer fillet"), BendProbe::Fillets(Bend.Solved, Bend.Corner, Inner, Outer))) { continue; }
		const TArray<BendProbe::FTurnChain> Turns = BendProbe::TurnsAt(*Bend.Net, Bend.Corner);
		const BendProbe::FTurnChain* InLane = nullptr;
		const BendProbe::FTurnChain* OutLane = nullptr;
		SplitLanes(Turns, Inner.Centre, InLane, OutLane);
		if (!TestTrue(FString::Printf(TEXT("%s: two lane turns"), Names[Tier]), InLane != nullptr && OutLane != nullptr)) { continue; }
		const double In = Tightest(*Bend.Net, *InLane);
		const double Out = Tightest(*Bend.Net, *OutLane);
		TestTrue(FString::Printf(TEXT("%s: the inner lane turns no tighter than before (%.0f vs %.0f)"), Names[Tier], In, BeforeInner[Tier]),
			In >= BeforeInner[Tier]);
		TestTrue(FString::Printf(TEXT("%s: the outer lane turns no tighter than before (%.0f vs %.0f)"), Names[Tier], Out, BeforeOuter[Tier]),
			Out >= BeforeOuter[Tier]);
		const double Lock = Designs.For(Profiles[Tier]).TightestFollowableRadius();
		TestTrue(FString::Printf(TEXT("%s: and clears the design vehicle's lock (%.0f vs %.0f)"), Names[Tier], In, Lock), In >= Lock);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBendLaneOnPavementTest, "Airside.Build.BendLanes.StaysOnPavement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBendLaneOnPavementTest::RunTest(const FString& Parameters)
{
	// THE LANE'S OWN WIDTH IS ON THE TARMAC at every sample of its turn: the builder's clearances
	// (MeasureTurn, marched to the pavement polygons) are at least half the lane either side, less
	// the march's 10 uu step. Straight pieces carry no clearances (the lane width gates them) and
	// are skipped.
	using namespace BendLane;
	const TArray<URoadProfile*> Profiles = Tiers();
	if (!TestTrue(TEXT("the content set has its three service-road tiers, and they load"),
		Profiles.Num() == 3 && !Profiles.Contains(nullptr))) { return false; }
	for (int32 Tier = 0; Tier < 3; ++Tier)
	{
		const FBend Bend = Build(Profiles[Tier]);
		int32 Checked = 0;
		for (const BendProbe::FTurnChain& Turn : BendProbe::TurnsAt(*Bend.Net, Bend.Corner))
		{
			double Least = TNumericLimits<double>::Max();
			for (int32 I = 0; I < Turn.Path.Num(); ++I)
			{
				if (Turn.ClearIn[I] < 0.0 || Turn.ClearOut[I] < 0.0) { continue; }
				Least = FMath::Min(Least, FMath::Min(Turn.ClearIn[I], Turn.ClearOut[I]));
				++Checked;
			}
			TestTrue(FString::Printf(TEXT("%s: the lane's half-width is on the tarmac either side at every sample (least %.0f, half lane %.0f)"),
				Names[Tier], Least, 0.5 * Turn.Width), Least >= 0.5 * Turn.Width - 10.0);
		}
		TestTrue(FString::Printf(TEXT("%s: samples were measured - not vacuous (%d)"), Names[Tier], Checked), Checked > 32);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBendLaneWeldTest, "Airside.Build.BendLanes.WeldExact",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBendLaneWeldTest::RunTest(const FString& Parameters)
{
	// THE SURFACE STILL WELDS BITWISE at the bend (CLAUDE.md, invariant one), measured as the width
	// taper's test does: each cut vertex is ONE mesh vertex, and every mesh edge along a cut line is
	// used by exactly two triangles - the ribbon's and the junction's.
	using namespace BendLane;
	const TArray<URoadProfile*> Profiles = Tiers();
	if (!TestTrue(TEXT("the content set has its three service-road tiers, and they load"),
		Profiles.Num() == 3 && !Profiles.Contains(nullptr))) { return false; }
	for (int32 Tier = 0; Tier < 3; ++Tier)
	{
		const FBend Bend = Build(Profiles[Tier]);
		// THE WIDE FIXTURE IS WIDENED, or the weld measured below is the plain fillet's again.
		if (Tier == UAirsideSettings::WideServiceTier)
		{
			RoadGeom::FFillet Inner, Outer;
			const FJunctionResult* Junction = Bend.Solved.NodeResults.Find(Bend.Corner.Index);
			const double FilletCut = BendProbe::Fillets(Bend.Solved, Bend.Corner, Inner, Outer) ? FilletCutOf(*Bend.Net, Bend.Corner, Inner) : 0.0;
			TestTrue(FString::Printf(TEXT("Wide: the fixture is widened - an arm cut past the fillet (%.0f / %.0f vs %.0f)"),
				Junction ? Junction->Arms[0].CutDistance : 0.0, Junction ? Junction->Arms[1].CutDistance : 0.0, FilletCut),
				Junction != nullptr && FilletCut > 0.0 && FMath::Max(Junction->Arms[0].CutDistance, Junction->Arms[1].CutDistance) > FilletCut + 100.0);
		}
		FRoadMeshBuilder Builder(10.0);
		Builder.Build(*Bend.Net, Bend.Solved, 3);
		const FRoadMeshBuffers& Buffers = Builder.GetBuffers();
		const FRoadNode* Corner = Bend.Net->GetNode(Bend.Corner);
		TArray<TPair<FVector2D, FVector2D>> Lines;
		for (const FRoadSegmentId& Id : Corner->Incident)
		{
			const FRoadSegment* Seg = Bend.Net->GetSegment(Id);
			const bool bAtA = Seg->A == Bend.Corner;
			Lines.Add({ bAtA ? Seg->LeftCutA : Seg->LeftCutB, bAtA ? Seg->RightCutA : Seg->RightCutB });
		}
		for (const TPair<FVector2D, FVector2D>& Line : Lines)
		{
			for (const FVector2D& Cut : { Line.Key, Line.Value })
			{
				int32 Matches = 0;
				for (const FVector3d& P : Buffers.Positions) { Matches += (P.X == Cut.X && P.Y == Cut.Y) ? 1 : 0; }
				TestEqual(FString::Printf(TEXT("%s: cut vertex (%.1f, %.1f) is one welded mesh vertex"), Names[Tier], Cut.X, Cut.Y), Matches, 1);
			}
		}
		TMap<TPair<int32, int32>, int32> EdgeUse;
		for (int32 Slot = 0; Slot + 2 < Buffers.Indices.Num(); Slot += 3)
		{
			for (int32 K = 0; K < 3; ++K)
			{
				const int32 A = Buffers.Indices[Slot + K];
				const int32 B = Buffers.Indices[Slot + (K + 1) % 3];
				EdgeUse.FindOrAdd(TPair<int32, int32>(FMath::Min(A, B), FMath::Max(A, B)))++;
			}
		}
		auto OnLine = [](const FVector3d& P, const FVector2D& A, const FVector2D& B)
		{
			const FVector2D Q(P.X, P.Y);
			const FVector2D AB = B - A;
			const double T = FVector2D::DotProduct(Q - A, AB) / AB.SizeSquared();
			return T >= -1e-9 && T <= 1.0 + 1e-9 && FMath::Abs(FVector2D::CrossProduct(AB, Q - A)) / AB.Size() < 1e-3;
		};
		int32 CutEdges = 0;
		for (const TPair<FVector2D, FVector2D>& Line : Lines)
		{
			for (const TPair<TPair<int32, int32>, int32>& Use : EdgeUse)
			{
				if (OnLine(Buffers.Positions[Use.Key.Key], Line.Key, Line.Value) && OnLine(Buffers.Positions[Use.Key.Value], Line.Key, Line.Value))
				{
					++CutEdges;
					TestEqual(FString::Printf(TEXT("%s: cut-line edge (%.1f, %.1f)-(%.1f, %.1f) is shared by the ribbon and the junction"), Names[Tier],
						Buffers.Positions[Use.Key.Key].X, Buffers.Positions[Use.Key.Key].Y,
						Buffers.Positions[Use.Key.Value].X, Buffers.Positions[Use.Key.Value].Y), Use.Value, 2);
				}
			}
		}
		TestTrue(FString::Printf(TEXT("%s: edges along both cut lines were measured - not vacuous (%d)"), Names[Tier], CutEdges), CutEdges >= 2);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBendLaneSmoothTest, "Airside.Build.BendLanes.EveryTierIsSmooth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBendLaneSmoothTest::RunTest(const FString& Parameters)
{
	// ONE SMOOTH SHAPE PER TIER, AND ACROSS A WIDTH STEP (user, PIE on 33d6f49b: "three
	// variants"). Every tier's right angle, and each mixed pair both ways round, judged as the rig
	// course's bends are (BendProbe::MeasureSmoothness): both edges and every lane tangent-
	// continuous, no edge tighter than half its inner arc, lanes laid by the one length rule.
	// A WIDTH STEP'S LANES ARE A CURVE NOW, not the one quadratic cut to cut that 33d6f49b kept
	// ("only one node around the corner") - they ramp onto the wide arm's circle on the bend's own
	// ramp clock (GuidelineGeom::RampedBendLane), so each has more than two pieces.
	using namespace BendLane;
	const TArray<URoadProfile*> Profiles = Tiers();
	if (!TestTrue(TEXT("the content set has its three service-road tiers, and they load"),
		Profiles.Num() == 3 && !Profiles.Contains(nullptr))) { return false; }
	const int32 Cases[][2] = { { 0, 0 }, { 1, 1 }, { 2, 2 }, { 0, 2 }, { 2, 0 }, { 1, 2 } };
	for (const int32 (&Case)[2] : Cases)
	{
		const FString Name = FString::Printf(TEXT("%s -> %s"), Names[Case[0]], Names[Case[1]]);
		const FBend Bend = Build(Profiles[Case[0]], Profiles[Case[1]]);
		const FJunctionResult* Junction = Bend.Solved.NodeResults.Find(Bend.Corner.Index);
		const FBendOuter* Note = Bend.Solved.BendOuters.FindByPredicate(
			[&Bend](const FBendOuter& Candidate) { return Candidate.NodeIndex == Bend.Corner.Index; });
		if (!TestTrue(FString::Printf(TEXT("%s: the bend solved and was laid as the smooth shape"), *Name),
			Junction != nullptr && Note != nullptr && Note->bApplied)) { continue; }
		const BendProbe::FSmoothness S = BendProbe::MeasureSmoothness(*Bend.Net, *Junction, Bend.Corner);
		AddInfo(FString::Printf(TEXT("%s: inner kink %.2f, outer %.2f, tightest %.0f of arc %.0f, ramps %.0f / %.0f, lanes: %s"),
			*Name, S.InnerKink, S.OuterKink, S.Tightest, Note->InnerArcRadius, Note->Ramp[0], Note->Ramp[1], *S.LaneText));
		TestTrue(FString::Printf(TEXT("%s: the inner edge is smooth (kink %.2f deg)"), *Name, S.InnerKink), S.InnerKink <= BendProbe::KinkThreshold);
		TestTrue(FString::Printf(TEXT("%s: the outer edge is smooth (kink %.2f deg)"), *Name, S.OuterKink), S.OuterKink <= BendProbe::KinkThreshold);
		TestTrue(FString::Printf(TEXT("%s: no edge bends tighter than half its inner arc (%.0f of %.0f)"), *Name, S.Tightest, Note->InnerArcRadius),
			S.Tightest >= BendProbe::TightestFraction * Note->InnerArcRadius - 1.0);
		TestTrue(FString::Printf(TEXT("%s: every lane is smooth (kink %.2f deg)"), *Name, S.LaneKink), S.LaneKink <= BendProbe::KinkThreshold);
		TestTrue(FString::Printf(TEXT("%s: two lanes, laid by one length rule (%.0f - %.0f uu)"), *Name, S.ShortestStep, S.LongestStep),
			S.Lanes == 2 && S.LongestStep <= GuidelineGeom::BendPieceLength + 1.0 && S.ShortestStep >= 0.5 * GuidelineGeom::BendPieceLength);
		for (const BendProbe::FTurnChain& Turn : BendProbe::TurnsAt(*Bend.Net, Bend.Corner))
		{
			TestTrue(FString::Printf(TEXT("%s: the lane turn is a curve in pieces, not one quadratic (%d)"), *Name, Turn.Pieces.Num()),
				Turn.Pieces.Num() > 2);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBendLaneTaxiwayTest, "Airside.Build.BendLanes.TaxiwayBendUnchanged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBendLaneTaxiwayTest::RunTest(const FString& Parameters)
{
	// A TAXIWAY'S CORNER IS AUTHORED FOR AIRCRAFT (PreferredFilletRadius) and its turn is the
	// centreline quadratic controlled on the node, bitwise (TwoWay.TaxiwayControlUnchanged): the
	// bend-lane rule is a service road's and leaves it alone.
	using namespace BendLane;
	const FBend Bend = Build(URoadProfile::MakeTransient(2300.0, 1500.0));
	const TArray<BendProbe::FTurnChain> Turns = BendProbe::TurnsAt(*Bend.Net, Bend.Corner);
	TestTrue(FString::Printf(TEXT("the taxiway bend has its turns (%d)"), Turns.Num()), Turns.Num() > 0);
	for (const BendProbe::FTurnChain& Turn : Turns)
	{
		TestEqual(TEXT("each taxiway turn is one quadratic"), Turn.Pieces.Num(), 1);
		const FGuidelineEdge* Edge = Bend.Net->GetGuidelineEdge(Turn.Pieces[0]);
		TestTrue(TEXT("controlled on the node, exactly"), Edge != nullptr && Edge->Control == Bend.Net->GetNode(Bend.Corner)->Position);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBendLaneWideRigTest, "Airside.Build.BendLanes.WideBendCarriesTheRig",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBendLaneWideRigTest::RunTest(const FString& Parameters)
{
	// THE WIDE TIER'S DESIGN VEHICLE STAYS ON ITS TARMAC (ruling 2026-09-25, step 2): on concentric
	// lanes alone the rig's trailer still left the inner edge by 247 uu (measured), so the solver
	// widens the inside to its traced sweep plus a margin. Driven round both lane turns here by the
	// router's own pursuit, against the pavement the mesh paves, the rig leaves it nowhere. The
	// router's clearances come from the widened pavement - its inner clearance on the arc exceeds
	// what the unwidened edge gave - and the widened corner is still paved by the fan (the shoulder
	// band ring and all), not the ear-clip fallback that has no band.
	using namespace BendLane;
	const TArray<URoadProfile*> Profiles = Tiers();
	if (!TestTrue(TEXT("the content set has its three service-road tiers, and they load"),
		Profiles.Num() == 3 && !Profiles.Contains(nullptr))) { return false; }
	const FVehicle Rig = UAirsideSettings::ResolveRigVehicle();
	const int32 Wide = UAirsideSettings::WideServiceTier;
	const FBend Bend = Build(Profiles[Wide]);
	RoadGeom::FFillet Inner, Outer;
	if (!TestTrue(TEXT("the bend has an inner and an outer fillet"), BendProbe::Fillets(Bend.Solved, Bend.Corner, Inner, Outer))) { return false; }
	const FJunctionResult& Junction = Bend.Solved.NodeResults[Bend.Corner.Index];
	const BendProbe::FPavement Pavement = BendProbe::PavementAll(*Bend.Net, Bend.Solved);
	const double Half = 0.5 * Profiles[Wide]->GetTotalWidth();
	const double Offset = FMath::Abs(Profiles[Wide]->Guidelines[0].CentreOffset);

	// Widened at all: an arm is cut back past the fillet's tangent point to hold it.
	const double FilletCut = FilletCutOf(*Bend.Net, Bend.Corner, Inner);
	TestTrue(FString::Printf(TEXT("Wide: an arm is cut back past the fillet to hold the widening (cuts %.0f / %.0f, fillet's %.0f)"),
		Junction.Arms[0].CutDistance, Junction.Arms[1].CutDistance, FilletCut),
		FMath::Max(Junction.Arms[0].CutDistance, Junction.Arms[1].CutDistance) > FilletCut + 100.0);
	TestTrue(TEXT("Wide: the widened corner is paved by the fan - same bands as every junction"), Junction.Triangles.Num() > 0);

	const TArray<BendProbe::FTurnChain> Chains = BendProbe::TurnsAt(*Bend.Net, Bend.Corner);
	TestEqual(TEXT("Wide: both lane turns are there to drive"), Chains.Num(), 2);
	for (const BendProbe::FTurnChain& Turn : Chains)
	{
		const BendProbe::FOverrun Off = BendProbe::Overrun(Turn, Rig, Pavement);
		TestTrue(TEXT("Wide: the rig's drive was traced to the end"), Off.bTraced);
		TestEqual(FString::Printf(TEXT("Wide: the rig stays on the tarmac inside the bend (off by %.0f uu at (%.0f, %.0f))"),
			Off.Inner, Off.InnerAt.X, Off.InnerAt.Y), Off.Inner, 0.0);
		TestEqual(TEXT("Wide: and outside it"), Off.Outer, 0.0);
	}

	// THE ROUTER READS THE WIDENED PAVEMENT: the inner lane's clearance inward, somewhere on its
	// arc, is well past the 285 uu the unwidened edge gave it (the lane's distance off that edge).
	const BendProbe::FTurnChain* InLane = nullptr;
	const BendProbe::FTurnChain* OutLane = nullptr;
	SplitLanes(Chains, Inner.Centre, InLane, OutLane);
	if (!TestTrue(TEXT("Wide: two lane turns"), InLane != nullptr)) { return false; }
	double MostIn = 0.0;
	for (const double Clear : InLane->ClearIn) { MostIn = FMath::Max(MostIn, Clear); }
	TestTrue(FString::Printf(TEXT("Wide: the inner lane's clearances are the widened pavement's (%.0f inward at best, vs %.0f to the unwidened edge)"),
		MostIn, Half - Offset), MostIn >= Half - Offset + 100.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBendLaneWideningOnlyTest, "Airside.Build.BendLanes.WideningOnlyWhereNeeded",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBendLaneWideningOnlyTest::RunTest(const FString& Parameters)
{
	// ONLY BY WHAT IS LEFT (ruling 2026-09-25): Narrow and Standard widen for THEIR design vehicle,
	// the bowser, which stays on their tarmac - so nothing moves there, the cuts are the fillet's
	// own. And on Wide the rig comes as close to the widened edge as the margin allows and no
	// further off: the widening is its sweep plus BendWidening::Margin (25 uu), not a generous one.
	using namespace BendLane;
	const TArray<URoadProfile*> Profiles = Tiers();
	if (!TestTrue(TEXT("the content set has its three service-road tiers, and they load"),
		Profiles.Num() == 3 && !Profiles.Contains(nullptr))) { return false; }
	const FVehicle Bowser = UAirsideSettings::ResolveDefaultVehicle();
	const FVehicle Rig = UAirsideSettings::ResolveRigVehicle();
	for (int32 Tier = 0; Tier < 3; ++Tier)
	{
		const FBend Bend = Build(Profiles[Tier]);
		RoadGeom::FFillet Inner, Outer;
		if (!TestTrue(TEXT("the bend has an inner and an outer fillet"), BendProbe::Fillets(Bend.Solved, Bend.Corner, Inner, Outer))) { continue; }
		const FJunctionResult& Junction = Bend.Solved.NodeResults[Bend.Corner.Index];
		const BendProbe::FPavement Pavement = BendProbe::PavementAll(*Bend.Net, Bend.Solved);
		const TArray<BendProbe::FTurnChain> Chains = BendProbe::TurnsAt(*Bend.Net, Bend.Corner);
		for (const BendProbe::FTurnChain& Turn : Chains)
		{
			TestEqual(FString::Printf(TEXT("%s: the bowser stays on the tarmac"), Names[Tier]), BendProbe::Overrun(Turn, Bowser, Pavement).Inner, 0.0);
		}
		const double FilletCut = FilletCutOf(*Bend.Net, Bend.Corner, Inner);
		if (Tier != UAirsideSettings::WideServiceTier)
		{
			TestTrue(FString::Printf(TEXT("%s: not widened - the cuts are the fillet's own (%.1f / %.1f vs %.1f)"), Names[Tier],
				Junction.Arms[0].CutDistance, Junction.Arms[1].CutDistance, FilletCut),
				FMath::Abs(Junction.Arms[0].CutDistance - FilletCut) < 1e-3 && FMath::Abs(Junction.Arms[1].CutDistance - FilletCut) < 1e-3);
			continue;
		}
		// Wide: how close the rig's body comes to the junction's rim over both turns.
		TArray<FVector2D> Points;
		for (const BendProbe::FTurnChain& Turn : Chains)
		{
			Points.Append(BendProbe::BodyPoints(Turn, Rig));
		}
		const double Clearance = BendProbe::RimClearance(Junction, Points);
		UE_LOG(LogTemp, Display, TEXT("BendLanes: Wide rig's closest approach to the widened rim %.1f uu; cuts %.0f / %.0f (fillet's %.0f)"),
			Clearance, Junction.Arms[0].CutDistance, Junction.Arms[1].CutDistance, FilletCut);
		// 25 uu margin, less a bin of lean (25 x 0.25) where the envelope's bins interpolate, plus
		// 15 uu for the bins' own 25 uu resolution along the edge: a tight fit, not a generous one.
		TestTrue(FString::Printf(TEXT("Wide: the rig passes within the margin of the widened edge, not far inside it (closest %.1f uu)"), Clearance),
			Clearance > 0.0 && Clearance <= 25.0 + 15.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBendLaneSnapCostTest, "Airside.Build.BendLanes.SnapAndDragDoNotTrace",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBendLaneSnapCostTest::RunTest(const FString& Parameters)
{
	// THE WIDENING IS TRACED ON A TOPOLOGY REBUILD AND NOWHERE ELSE (review of 75d3cbc0): the
	// snap's claims, cut and reach queries run per cursor move, and the ghost's two-node solve per
	// drag frame. They read what the rebuild traced - so a cursor over a widened bend still sees
	// its widened pavement - and never drive a vehicle themselves.
	using namespace BendLane;
	const TArray<URoadProfile*> Profiles = Tiers();
	if (!TestTrue(TEXT("the content set has its three service-road tiers, and they load"),
		Profiles.Num() == 3 && !Profiles.Contains(nullptr))) { return false; }
	for (int32 Tier = 0; Tier < 3; ++Tier)
	{
		const FBend Bend = Build(Profiles[Tier]);
		const FRoadSegmentId Arm = Bend.Net->GetNode(Bend.Corner)->Incident[0];
		FRoadNetworkSolver::ResetWideningTraceCountForTest();
		constexpr int32 Moves = 50;
		const double Began = FPlatformTime::Seconds();
		for (int32 Move = 0; Move < Moves; ++Move)
		{
			// As RoadSnap asks it: self-resolving, no vehicles passed.
			FRoadNetworkSolver::NodeClaims(*Bend.Net, Bend.Corner, FVector2D(7000.0 + Move * 10.0, 900.0));
			FRoadNetworkSolver::ArmCutDistance(*Bend.Net, Arm, Bend.Corner);
			FRoadNetworkSolver::NodeReach(*Bend.Net, Bend.Corner);
		}
		const double Ms = (FPlatformTime::Seconds() - Began) * 1000.0 / Moves;
		UE_LOG(LogTemp, Display, TEXT("BendLanes: %s snap query cost %.3f ms per cursor move (claims + cut + reach)"), Names[Tier], Ms);
		TestEqual(FString::Printf(TEXT("%s: no widening trace on %d cursor moves (%.3f ms each)"), Names[Tier], Moves, Ms),
			FRoadNetworkSolver::WideningTraceCountForTest, 0);

		// The ghost's per-drag-frame solve of the bend node, and a Geometry rebuild's.
		FRoadSolveResult Ghost;
		FRoadNetworkSolver::SolveNodeInto(*Bend.Net, Bend.Corner.Index, 12, Ghost);
		FRoadNetworkSolver::SolveAll(*Bend.Net, 12, nullptr, EWideningTrace::ReadCached);
		TestEqual(FString::Printf(TEXT("%s: no widening trace on a drag frame's solves"), Names[Tier]),
			FRoadNetworkSolver::WideningTraceCountForTest, 0);
		// And they see the widened pavement the rebuild laid: the same cuts.
		const FJunctionResult* Laid = Bend.Solved.NodeResults.Find(Bend.Corner.Index);
		const FJunctionResult* Seen = Ghost.NodeResults.Find(Bend.Corner.Index);
		TestTrue(FString::Printf(TEXT("%s: the drag frame's solve reads the widened cuts the rebuild traced"), Names[Tier]),
			Laid != nullptr && Seen != nullptr && Laid->Arms[0].CutDistance == Seen->Arms[0].CutDistance
			&& Laid->Arms[1].CutDistance == Seen->Arms[1].CutDistance);
	}
	return true;
}

namespace BendLane
{
	/** LogRoadSolve warnings naming a capped widening. Unbuffered (CanBeUsedOnMultipleThreads), or the log thread delivers late. */
	struct FCappedSpy : public FOutputDevice
	{
		TArray<FString> Lines;
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Verbosity == ELogVerbosity::Warning && Category == FName(TEXT("LogRoadSolve")) && FString(V).Contains(TEXT("inside widening is capped")))
			{
				Lines.Add(FString(V));
			}
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBendLaneCappedWarnsTest, "Airside.Build.BendLanes.CappedWideningWarns",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBendLaneCappedWarnsTest::RunTest(const FString& Parameters)
{
	// A CAPPED WIDENING SAYS SO, ONCE PER REBUILD (review of 75d3cbc0): a Wide bend whose north arm
	// is a 25 m stub cannot be cut back far enough to hold the rig's widening, and the rig then
	// still leaves the tarmac there - which the course's Wide corners did with nothing logged. A
	// tracing solve (a Topology rebuild) names the bend, the overrun and the length to draw; a
	// reading one (a drag frame) stays quiet.
	using namespace BendLane;
	const TArray<URoadProfile*> Profiles = Tiers();
	if (!TestTrue(TEXT("the content set has its three service-road tiers, and they load"),
		Profiles.Num() == 3 && !Profiles.Contains(nullptr))) { return false; }
	URoadProfile* Wide = Profiles[UAirsideSettings::WideServiceTier];
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadNodeId West = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId Corner = Net->AddNode(FVector2D(8000.0, 0.0));
	Net->AddStraightSegment(West, Corner, Wide);
	Net->AddStraightSegment(Corner, Net->AddNode(FVector2D(8000.0, 2500.0)), Wide);
	const FRoadDesignVehicles Designs = UAirsideSettings::ResolveRoadDesignVehicles();

	FCappedSpy Spy;
	GLog->AddOutputDevice(&Spy);
	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net, 12, &Designs, EWideningTrace::Trace);
	const int32 OnRebuild = Spy.Lines.Num();
	FRoadNetworkSolver::SolveAll(*Net, 12, &Designs, EWideningTrace::ReadCached);
	GLog->RemoveOutputDevice(&Spy);

	TestEqual(TEXT("the capped bend is warned once on the rebuild that traced it"), OnRebuild, 1);
	TestEqual(TEXT("and not again on a drag frame's solve"), Spy.Lines.Num(), 1);
	TestEqual(TEXT("the solve reports the one capped bend"), Solved.CappedWidenings.Num(), 1);
	if (Spy.Lines.Num() > 0)
	{
		AddInfo(Spy.Lines[0]);
		TestTrue(TEXT("the warning names the bend and the length to draw"),
			Spy.Lines[0].Contains(TEXT("Bend at (8000,0)")) && Spy.Lines[0].Contains(TEXT("at least")));
	}
	if (Solved.CappedWidenings.Num() == 1)
	{
		const FCappedWidening& Capped = Solved.CappedWidenings[0];
		TestTrue(FString::Printf(TEXT("the length it names is longer than the stub (%.0f vs %.0f uu)"), Capped.LengthNeeded, Capped.Length),
			Capped.LengthNeeded > Capped.Length && FMath::IsNearlyEqual(Capped.Length, 2500.0, 1.0));

		// AND IT IS TRUE: the rig driven round the built bend still leaves the tarmac, by about what it says.
		FRoadGuidelineBuilder::Build(*Net, Solved, Designs);
		const BendProbe::FPavement Pavement = BendProbe::PavementAll(*Net, Solved);
		double Worst = 0.0;
		for (const BendProbe::FTurnChain& Turn : BendProbe::TurnsAt(*Net, Corner))
		{
			Worst = FMath::Max(Worst, BendProbe::Overrun(Turn, UAirsideSettings::ResolveRigVehicle(), Pavement).Inner);
		}
		TestTrue(FString::Printf(TEXT("the rig does still leave the tarmac there (%.0f uu; warned %.0f)"), Worst, Capped.Overrun),
			Worst > 10.0 && Capped.Overrun > 10.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBendLaneCappedOncePerGeometryTest, "Airside.Build.BendLanes.CappedWideningWarnsOncePerGeometry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBendLaneCappedOncePerGeometryTest::RunTest(const FString& Parameters)
{
	// ONCE UNTIL THE BEND CHANGES (re-review of 8de90a45): the rig course is laid once and rebuilt
	// on every Topology edit, and its capped Wide bends said so on every one. The same bend over
	// three tracing rebuilds warns once; shorten its stub and it warns again, with the new figures.
	using namespace BendLane;
	const TArray<URoadProfile*> Profiles = Tiers();
	if (!TestTrue(TEXT("the content set has its three service-road tiers, and they load"),
		Profiles.Num() == 3 && !Profiles.Contains(nullptr))) { return false; }
	URoadProfile* Wide = Profiles[UAirsideSettings::WideServiceTier];
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadNodeId West = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId Corner = Net->AddNode(FVector2D(8000.0, 0.0));
	Net->AddStraightSegment(West, Corner, Wide);
	const FRoadNodeId Stub = Net->AddNode(FVector2D(8000.0, 2500.0));
	Net->AddStraightSegment(Corner, Stub, Wide);
	const FRoadDesignVehicles Designs = UAirsideSettings::ResolveRoadDesignVehicles();

	FCappedSpy Spy;
	GLog->AddOutputDevice(&Spy);
	for (int32 Rebuild = 0; Rebuild < 3; ++Rebuild)
	{
		FRoadNetworkSolver::SolveAll(*Net, 12, &Designs, EWideningTrace::Trace);
	}
	const int32 Unchanged = Spy.Lines.Num();
	Net->SetNodePosition(Stub, FVector2D(8000.0, 2000.0));
	FRoadNetworkSolver::SolveAll(*Net, 12, &Designs, EWideningTrace::Trace);
	FRoadNetworkSolver::SolveAll(*Net, 12, &Designs, EWideningTrace::Trace);
	GLog->RemoveOutputDevice(&Spy);

	TestEqual(TEXT("the same capped bend over three rebuilds is warned once"), Unchanged, 1);
	TestEqual(TEXT("its stub shortened, it is warned again - once"), Spy.Lines.Num(), 2);
	if (Spy.Lines.Num() == 2)
	{
		TestTrue(TEXT("with the new figures"), Spy.Lines[1] != Spy.Lines[0]);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBendLaneOuterEdgeTest, "Airside.Build.BendLanes.OuterEdgeConcentric",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBendLaneOuterEdgeTest::RunTest(const FString& Parameters)
{
	// THE OUTSIDE FOLLOWS THE LANES (user ruling 2026-09-25). The lanes turn about the inner
	// fillet's centre; the outer edge was the tier's fillet about its OWN centre, leaving a band
	// outside the outer lane nobody drives. Now the outer rim is the arc about the lanes' centre at
	// the inner fillet's radius plus the road width - and tangent to both arms' outer edges, which
	// the ribbons run along.
	using namespace BendLane;
	const TArray<URoadProfile*> Profiles = Tiers();
	if (!TestTrue(TEXT("the content set has its three service-road tiers, and they load"),
		Profiles.Num() == 3 && !Profiles.Contains(nullptr))) { return false; }
	for (int32 Tier = 0; Tier < 3; ++Tier)
	{
		const FBend Bend = Build(Profiles[Tier]);
		RoadGeom::FFillet Inner, Outer;
		const FJunctionResult* Junction = Bend.Solved.NodeResults.Find(Bend.Corner.Index);
		if (!TestTrue(FString::Printf(TEXT("%s: the bend solved, with its two fillets"), Names[Tier]),
			Junction != nullptr && BendProbe::Fillets(Bend.Solved, Bend.Corner, Inner, Outer))) { continue; }
		const double Width = Profiles[Tier]->GetTotalWidth();
		const double Expected = Inner.Radius + Width;
		const FVector2D& C = Inner.Centre;

		// The rim's points between the inner arc's two radial lines, on the OUTER side of the road.
		const double From = FMath::Atan2(Inner.TangentA.Y - C.Y, Inner.TangentA.X - C.X);
		const double Span = FMath::UnwindRadians(FMath::Atan2(Inner.TangentB.Y - C.Y, Inner.TangentB.X - C.X) - From);
		int32 OnArc = 0;
		double Worst = 0.0;
		for (int32 Slot = 0; Slot + 1 < Junction->Boundary.Num(); ++Slot)
		{
			const FVector2D P = Junction->Boundary[Slot];
			const double Distance = FVector2D::Distance(P, C);
			const double Along = FMath::UnwindRadians(FMath::Atan2(P.Y - C.Y, P.X - C.X) - From) / Span;
			if (Distance < Inner.Radius + Width * 0.5 || Along < -1.0e-6 || Along > 1.0 + 1.0e-6)
			{
				continue;
			}
			++OnArc;
			Worst = FMath::Max(Worst, FMath::Abs(Distance - Expected));
		}
		AddInfo(FString::Printf(TEXT("%s: outer edge was a %.0f uu fillet about its own centre; now %.1f uu about the lanes' centre (inner %.0f + width %.0f), %d rim points"),
			Names[Tier], Outer.Radius, Expected, Inner.Radius, Width, OnArc));
		TestTrue(FString::Printf(TEXT("%s: the outer rim is an arc round the bend (%d points)"), Names[Tier], OnArc), OnArc >= 8);
		TestTrue(FString::Printf(TEXT("%s: every point of it is the inner radius plus the road width from the lanes' centre (worst %.3f uu off)"),
			Names[Tier], Worst), Worst <= 0.01);

		// TANGENT: the arc's ends lie on the arms' outer edges, where the radius meets them square.
		// West arm along +X at y = -Width/2 (the bend turns left, north); north arm up x = 8000 + Width/2.
		const FVector2D WestFoot(C.X, -Width * 0.5);
		const FVector2D NorthFoot(8000.0 + Width * 0.5, C.Y);
		for (const FVector2D& Foot : { WestFoot, NorthFoot })
		{
			bool bFound = false;
			for (const FVector2D& P : Junction->Boundary) { bFound |= P.Equals(Foot, 0.01); }
			TestTrue(FString::Printf(TEXT("%s: the arc meets an arm's outer edge at the foot of its centre (%.1f, %.1f) - tangent there"),
				Names[Tier], Foot.X, Foot.Y), bFound);
			TestTrue(FString::Printf(TEXT("%s: and the foot is the arc's radius from the centre"), Names[Tier]),
				FMath::IsNearlyEqual(FVector2D::Distance(Foot, C), Expected, 0.01));
		}
	}
	return true;
}

#endif
