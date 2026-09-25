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
				LeastIn = FMath::Min(LeastIn, Turn.ClearIn[I]);
				LeastOut = FMath::Min(LeastOut, Turn.ClearOut[I]);
			}
			const BendProbe::FOverrun RigOff = BendProbe::Overrun(Turn, Rig, Pavement);
			const BendProbe::FOverrun BowserOff = BendProbe::Overrun(Turn, Bowser, Pavement);
			const BendProbe::FOverrun UtilityOff = BendProbe::Overrun(Turn, Utility, Pavement);
			UE_LOG(LogTemp, Display, TEXT("BendCensus %s %s turn: %d piece(s), MinRadius %.0f; about inner centre %.0f..%.0f, about outer centre %.0f..%.0f; least clear in %.0f out %.0f; from (%.0f, %.0f) to (%.0f, %.0f)"),
				Names[Tier], InMin < Inner.Radius + 0.5 * Profiles[Tier]->GetTotalWidth() ? TEXT("inner-lane") : TEXT("outer-lane"), Turn.Pieces.Num(), Turn.MinRadius, InMin, InMax, OutMin, OutMax,
				LeastIn, LeastOut, Turn.Path[0].X, Turn.Path[0].Y, Turn.Path.Last().X, Turn.Path.Last().Y);
			UE_LOG(LogTemp, Display, TEXT("BendCensus %s %s turn: off the tarmac - rig inner %.0f outer %.0f (traced %d) at (%.0f, %.0f); bowser inner %.0f outer %.0f; utility inner %.0f outer %.0f"),
				Names[Tier], InMin < Inner.Radius + 0.5 * Profiles[Tier]->GetTotalWidth() ? TEXT("inner-lane") : TEXT("outer-lane"), RigOff.Inner, RigOff.Outer, RigOff.bTraced ? 1 : 0,
				RigOff.InnerAt.X, RigOff.InnerAt.Y, BowserOff.Inner, BowserOff.Outer, UtilityOff.Inner, UtilityOff.Outer);
			// WHERE the rig leaves the inside: angle about the inner fillet's centre (the fillet runs
			// -90 to 0 here) and how far past each arm's cut line.
			{
				double MinAngle = 1e9, MaxAngle = -1e9, PastCutX = -1e9, PastCutY = -1e9;
				for (int32 I = 0; I < RigOff.InnerPoints.Num(); ++I)
				{
					const FVector2D D = RigOff.InnerPoints[I] - Inner.Centre;
					const double Deg = FMath::RadiansToDegrees(FMath::Atan2(D.Y, D.X));
					MinAngle = FMath::Min(MinAngle, Deg);
					MaxAngle = FMath::Max(MaxAngle, Deg);
					PastCutX = FMath::Max(PastCutX, (8000.0 - Junction.Arms[0].CutDistance) - RigOff.InnerPoints[I].X);
					PastCutY = FMath::Max(PastCutY, RigOff.InnerPoints[I].Y - Junction.Arms[1].CutDistance);
				}
				UE_LOG(LogTemp, Display, TEXT("BendCensus %s: rig inner overrun at %d point(s), %.0f..%.0f deg about the inner centre; furthest past the west cut %.0f, past the north cut %.0f"),
					Names[Tier], RigOff.InnerPoints.Num(), MinAngle, MaxAngle, PastCutX, PastCutY);
				for (int32 Deg = -90; Deg <= 0; Deg += 10)
				{
					double Deepest = 0.0;
					for (int32 I = 0; I < RigOff.InnerPoints.Num(); ++I)
					{
						const FVector2D D = RigOff.InnerPoints[I] - Inner.Centre;
						const double A = FMath::RadiansToDegrees(FMath::Atan2(D.Y, D.X));
						if (A >= Deg - 5 && A < Deg + 5) { Deepest = FMath::Max(Deepest, Inner.Radius - D.Size()); }
					}
					UE_LOG(LogTemp, Display, TEXT("BendCensus %s: rig inside the fillet circle at %d deg: %.0f"), Names[Tier], Deg, Deepest);
				}
			}
		}
	}
	return true;
}

namespace BendLane
{
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBendLaneMixedTest, "Airside.Build.BendLanes.MixedWidthsKeepTheQuadratic",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBendLaneMixedTest::RunTest(const FString& Parameters)
{
	// WHERE NO CIRCLE FITS, NOTHING CHANGES: a Narrow arm meeting a Wide one puts each lane at a
	// different distance from the inner edge (210 and 285 uu), so no one circle about the fillet's
	// centre touches both. The turns stay the one quadratic they were.
	using namespace BendLane;
	const TArray<URoadProfile*> Profiles = Tiers();
	if (!TestTrue(TEXT("the content set has its three service-road tiers, and they load"),
		Profiles.Num() == 3 && !Profiles.Contains(nullptr))) { return false; }
	const FBend Bend = Build(Profiles[0], Profiles[2]);
	const TArray<BendProbe::FTurnChain> Turns = BendProbe::TurnsAt(*Bend.Net, Bend.Corner);
	TestEqual(TEXT("two lane turns at the mixed bend"), Turns.Num(), 2);
	for (const BendProbe::FTurnChain& Turn : Turns)
	{
		TestEqual(TEXT("each is the one quadratic it was"), Turn.Pieces.Num(), 1);
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

#endif
