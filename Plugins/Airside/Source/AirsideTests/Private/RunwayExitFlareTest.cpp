#include "CoreMinimal.h"
#include "Build/ExitGeometry.h"
#include "Build/RoadNetworkSolver.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"
#include "Solve/JunctionSolver.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE FLARE FOLLOWS THE ARC. At a runway junction the corner an exit arc sweeps through
 * gets a fillet a taxiway's half width inside the arc, so the pavement is where the wheels
 * go (samples/runway2.png, 2026-09-07); the other corner, the hairpin's, gets a small kerb.
 * Measured on the solver's own fillets - the centre-to-tangent distance is the radius it
 * actually paved with - on a 30 degree rapid exit long enough for nothing to shrink.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayExitFlareTest,
	"Airside.Solve.RunwayExitFlare",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayExitFlareTest::RunTest(const FString& Parameters)
{
	constexpr double ExitLength = 6000.0;
	constexpr double TaxiwayWidth = 2300.0;
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = URoadProfile::MakeTransient(1800.0, 1500.0, 180.0);
	Runway->bContinuousThroughJunctions = true;
	Runway->ExitLength = ExitLength;
	URoadProfile* Taxiway = URoadProfile::MakeTransient(TaxiwayWidth, 1500.0, 230.0);

	// A runway W -> E and a long taxiway leaving X at 30 degrees below east: a rapid exit
	// for a WESTBOUND landing (the aircraft arrives from E and turns 30 degrees onto it).
	const FRoadNodeId W = Net->AddNode(FVector2D(-80000.0, 0.0));
	const FRoadNodeId X = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId E = Net->AddNode(FVector2D(80000.0, 0.0));
	const FVector2D Exit = FVector2D(FMath::Cos(-PI / 6.0), FMath::Sin(-PI / 6.0));
	const FRoadNodeId T = Net->AddNode(Exit * 60000.0);
	Net->AddStraightSegment(W, X, Runway);
	Net->AddStraightSegment(X, E, Runway);
	const FRoadSegmentId XT = Net->AddStraightSegment(X, T, Taxiway);

	// 1. THE GEOMETRY, from the one place it is decided.
	const double Expected30 = ExitGeometry::FlareRadius(ExitLength, PI - PI / 6.0, TaxiwayWidth * 0.5);
	const double Acute = ExitGeometry::FlareRadius(ExitLength, PI / 6.0, TaxiwayWidth * 0.5);
	TestTrue(FString::Printf(TEXT("a 30 degree exit with 60 m tangents flares to a %.0f uu fillet (arc 224 m less a half width)"), Expected30),
		FMath::Abs(Expected30 - (ExitLength * FMath::Tan((PI - PI / 6.0) * 0.5) - TaxiwayWidth * 0.5)) < 1.0 && Expected30 > 20000.0);
	TestTrue(FString::Printf(TEXT("and the acute corner asks for %.0f, less than the 1500 default"), Acute), Acute < 1500.0);

	// 2. THE SOLVER PAVED IT. The junction's corners, CCW from arm to arm: exactly one of
	//    them is between the taxiway and the runway half the aircraft arrives from - the
	//    obtuse one - and its fillet radius is the flare.
	FRoadNodeCuts Cuts;
	if (!TestTrue(TEXT("X solves"), FRoadNetworkSolver::SolveNodeCuts(*Net, X.Index, 8, Cuts) && Cuts.Result.bValid)) { return false; }
	const int32 Arms = Cuts.Input.Arms.Num();
	TestEqual(TEXT("three arms at X"), Arms, 3);
	int32 Flared = 0, Kerbed = 0;
	double FlareRadius = 0.0, KerbRadius = -1.0;
	for (int32 Index = 0; Index < Arms; ++Index)
	{
		const FJunctionArm& A = Cuts.Input.Arms[Index];
		const FJunctionArm& B = Cuts.Input.Arms[(Index + 1) % Arms];
		if (A.bContinuous == B.bContinuous) { continue; }   // the runway's own two halves
		const RoadGeom::FFillet& Corner = Cuts.Result.Corners[Index];
		const double Radius = FVector2D::Distance(Corner.Centre, Corner.TangentA);
		if (Radius > 1500.0 + 1.0) { ++Flared; FlareRadius = Radius; }
		else { ++Kerbed; KerbRadius = Radius; }
	}
	TestEqual(TEXT("one corner flared - the obtuse one the exit arc sweeps through"), Flared, 1);
	TestEqual(FString::Printf(TEXT("and one is a kerb at or below the default - the acute corner the hairpin lives in (%.0f uu)"), KerbRadius), Kerbed, 1);
	// The acute corner also follows its own (hairpin) arc where that asks for more than the
	// kerb minimum: 6000 x tan(15 deg) less a half width is 458 here, above the 300 floor.
	TestTrue(FString::Printf(TEXT("the kerb is the acute arc's own radius or the 300 floor, whichever is larger (%.0f of %.0f)"), KerbRadius, FMath::Max(Acute, ExitGeometry::AcuteCornerRadius)),
		FMath::Abs(KerbRadius - FMath::Max(Acute, ExitGeometry::AcuteCornerRadius)) < 1.0);
	TestTrue(FString::Printf(TEXT("the flare's paved radius is the arc's less a half width (%.0f of %.0f), nothing shrunk on a long taxiway"), FlareRadius, Expected30),
		FMath::Abs(FlareRadius - Expected30) < 1.0);

	// 3. THE RUNWAY IS STILL UNCUT: a flare belongs to the taxiway side, and the strip's
	//    edges run through as they always did.
	for (int32 Index = 0; Index < Arms; ++Index)
	{
		if (Cuts.Input.Arms[Index].bContinuous)
		{
			TestTrue(TEXT("the runway arm keeps CutDistance 0"), Cuts.Result.Arms[Index].CutDistance == 0.0);
		}
	}
	return true;
}

#endif
