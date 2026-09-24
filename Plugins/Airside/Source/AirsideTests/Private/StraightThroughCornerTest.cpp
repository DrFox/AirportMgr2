#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Profiles/RoadProfile.h"
#include "Solve/RoadGeom.h"
#include "Tool/RoadPlacement.h"
#include "Tool/RoadSnap.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A ROAD MAY BE EXTENDED DEAD STRAIGHT. Reported from PIE, samples/issue1.png: a ghost
 * refused with "too short to hold the corner" while continuing a road along its own line -
 * where there is no corner at all.
 *
 * THE CAUSE IS THAT sin IS SYMMETRIC. RoadGeom::CornerReachAtZeroRadius guarded on
 * `sin(Theta) < 1e-9` to catch the HAIRPIN - two arms doubling back, where the corner's reach
 * genuinely diverges - and that guard also catches Theta = pi, which is the exact opposite: a
 * straight-through node, no corner, zero reach. One test per layer, because the arithmetic and
 * the refusal are in different modules and either could regress alone.
 *
 * THE GUIDES DID NOT CAUSE IT, THEY EXPOSED IT. At 179.9 degrees the reach computes to about
 * a thousandth of a half-width and passes; only exactly 180 refuses. Landing there took luck
 * until "along this road" started snapping the click onto the line every time.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCornerReachIsZeroStraightThroughTest,
	"Airside.Solve.CornerReachIsZeroStraightThrough",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FCornerReachIsZeroStraightThroughTest::RunTest(const FString& Parameters)
{
	const double Half = 200.0;

	// STRAIGHT THROUGH. Both tangents point AWAY from the node they meet at, so a road
	// continued along its own line puts them at pi to each other - see RoadPlacement's own
	// comment: "small means a hairpin, 180 means straight through".
	double AlongA = -1.0;
	double AlongB = -1.0;
	if (TestTrue(TEXT("a straight-through node has a corner reach at all"),
		RoadGeom::CornerReachAtZeroRadius(Half, Half, UE_DOUBLE_PI, AlongA, AlongB)))
	{
		TestEqual(TEXT("and it consumes nothing of the arm ahead"), AlongA, 0.0);
		TestEqual(TEXT("nor of the arm behind"), AlongB, 0.0);
	}

	// UNEQUAL WIDTHS, STILL STRAIGHT. The two inner edges are parallel lines a width-step
	// apart and never meet, so there is no intersection to solve - and still no corner to
	// hold. A wide road meeting a narrow one head-on just steps down at the node.
	double WideA = -1.0;
	double NarrowB = -1.0;
	if (TestTrue(TEXT("a width step straight through is not a corner either"),
		RoadGeom::CornerReachAtZeroRadius(400.0, 150.0, UE_DOUBLE_PI, WideA, NarrowB)))
	{
		TestEqual(TEXT("the wide arm gives up nothing"), WideA, 0.0);
		TestEqual(TEXT("and neither does the narrow one"), NarrowB, 0.0);
	}

	// THE HAIRPIN STILL REFUSES, which is the half of the old guard that was right. Without
	// this leg the fix could be "always return true" and the test would not notice.
	double HairpinA = 0.0;
	double HairpinB = 0.0;
	TestFalse(TEXT("but a hairpin doubling back has no finite reach"),
		RoadGeom::CornerReachAtZeroRadius(Half, Half, 0.0, HairpinA, HairpinB));

	// AND AN ORDINARY CORNER IS UNTOUCHED. A right angle between equal widths reaches exactly
	// one half-width along each arm - w / tan(45 degrees). The control that says this change
	// moved the two degenerate ends and nothing in between.
	double SquareA = 0.0;
	double SquareB = 0.0;
	if (TestTrue(TEXT("a right-angled corner still solves"),
		RoadGeom::CornerReachAtZeroRadius(Half, Half, UE_DOUBLE_PI / 2.0, SquareA, SquareB)))
	{
		TestTrue(TEXT("reaching one half-width along each arm"),
			FMath::IsNearlyEqual(SquareA, Half, 1.0e-6) && FMath::IsNearlyEqual(SquareB, Half, 1.0e-6));
	}

	return true;
}

/**
 * AND THE GHOST SAYS SO: the same case through RoadPlacement::Validate, which is what the
 * player actually met.
 *
 * A SEPARATE TEST FROM THE ARITHMETIC ABOVE because the refusal needs NewRoadHalfWidth set,
 * and that is the gate the whole corner-fit rule sits behind - every caller from before the
 * rule passes 0 and never reaches it. A test that left it 0 would pass on the broken build.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadExtendsStraightAheadTest,
	"Airside.Tool.RoadExtendsStraightAhead",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadExtendsStraightAheadTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Network = NewObject<URoadNetwork>(GetTransientPackage());
	if (!TestNotNull(TEXT("a network"), Network)) { return false; }

	URoadProfile* Profile = URoadProfile::MakeTransient(400.0, 100.0, 40.0);
	if (!TestNotNull(TEXT("a profile"), Profile)) { return false; }

	// A road running EAST from West to Hub. Continuing east from Hub is the reported gesture.
	const FRoadNodeId West = Network->AddNode(FVector2D(-4000.0, 0.0));
	const FRoadNodeId Hub = Network->AddNode(FVector2D(0.0, 0.0));
	if (!TestTrue(TEXT("the road behind exists"),
		Network->AddStraightSegment(West, Hub, Profile).IsSet()))
	{
		return false;
	}

	FRoadPlacementLimits Limits;
	Limits.MinSegmentLength = 250.0;
	Limits.MinTurnDegrees = 25.0;

	// THE GATE. Without a half-width the corner-fit rule is skipped entirely and this test
	// would pass against the bug - which is exactly why the bug survived to reach PIE.
	Limits.NewRoadHalfWidth = Profile->GetMaxHalfWidth();
	if (!TestTrue(TEXT("the new road has a half-width, so the corner rule runs"),
		Limits.NewRoadHalfWidth > 0.0))
	{
		return false;
	}

	FRoadSnapResult Ahead;
	Ahead.Kind = ERoadSnapKind::Free;
	Ahead.Position = FVector2D(4000.0, 0.0);

	TestEqual(TEXT("a road may be continued along its own line"),
		static_cast<int32>(RoadPlacement::Validate(*Network, Hub, Ahead, Limits)),
		static_cast<int32>(ERoadPlacement::Valid));

	// A HAIR OFF STRAIGHT WAS ALWAYS FINE, and saying so pins what the bug actually was: not
	// "straight roads are refused" but "EXACTLY straight is", which is why it took a guide
	// that lands the click precisely on the line to make it a common sight.
	FRoadSnapResult NearlyAhead;
	NearlyAhead.Kind = ERoadSnapKind::Free;
	NearlyAhead.Position = FVector2D(4000.0, 7.0);
	TestEqual(TEXT("as it always could a fraction off it"),
		static_cast<int32>(RoadPlacement::Validate(*Network, Hub, NearlyAhead, Limits)),
		static_cast<int32>(ERoadPlacement::Valid));

	// AND DOUBLING BACK IS STILL REFUSED. Drawing west from Hub, back along the road that is
	// already there, is the hairpin the guard was written for. TooSharp catches it before the
	// corner rule does - which is fine, and worth pinning: the point is that it is refused,
	// and that this fix did not open it up.
	FRoadSnapResult Backwards;
	Backwards.Kind = ERoadSnapKind::Free;
	Backwards.Position = FVector2D(-4000.0, 1.0);
	TestNotEqual(TEXT("but doubling back along it is still refused"),
		static_cast<int32>(RoadPlacement::Validate(*Network, Hub, Backwards, Limits)),
		static_cast<int32>(ERoadPlacement::Valid));

	return true;
}

/**
 * THE VALIDATOR AND THE SOLVER MEAN THE SAME THING BY "STRAIGHT THROUGH". Reported from PIE,
 * samples/t-junctions.png: an L corner of service road, a new road dragged out of the corner
 * along the left arm's line to make a T, refused "too short to hold the corner" - while the
 * same road drawn INTO the corner built.
 *
 * TWO DEFINITIONS, THREE ORDERS OF MAGNITUDE APART. RoadGeom::SolveFillet - what actually draws
 * the junction - calls a corner straight through within 1e-6 rad of pi. CornerReachAtZeroRadius
 * - what RoadPlacement refuses with - used `sin(Theta) < 1e-9`, i.e. within 1e-9. Between the
 * two, the solver draws a node with no corner and the validator computes one. With EQUAL widths
 * that computed corner is a hair (w * tan(delta / 2)) and nobody noticed; with UNEQUAL widths
 * the two inner edges are nearly parallel lines a width-step apart and meet (dHalf / delta) away
 * - ~680 km at delta = 1.5e-8 (1020 uu / 1.5e-8, measured) - so the corner "needs" more road than exists.
 *
 * AND acos PUT EVERY UNLUCKY GUIDED CLICK IN THAT GAP. The angle fed in was acos(dot), and acos
 * has a square-root singularity at -1: a dot ONE ulp off -1 comes back 1.5e-8 rad off pi. A
 * snap guide lands the click on the line as exactly as a normalise-project-normalise round trip
 * allows, and a large minority of positions come out of it one ulp off rather than exact.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStraightThroughIsOneDefinitionTest,
	"Airside.Solve.StraightThroughIsOneDefinition",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStraightThroughIsOneDefinitionTest::RunTest(const FString& Parameters)
{
	// A taxiway-width new road against a service-width arm: the pair PIE met, because
	// Limits.NewRoadHalfWidth comes from the actor's own (taxiway) profile.
	const double Wide = 1380.0;
	const double Narrow = 360.0;

	// delta = how far off pi. 1.49e-8 is acos(-1 + 1 ulp); 2e-6 and 1e-3 are real bends,
	// where the two must still agree (both say "a corner").
	const double Deltas[] = { 0.0, 1.0e-12, 1.0e-9, 1.49e-8, 1.0e-7, 9.0e-7, 2.0e-6, 1.0e-3 };
	for (const double Delta : Deltas)
	{
		const double Theta = UE_DOUBLE_PI - Delta;

		FRay2D A;
		A.Dir = FVector2D(1.0, 0.0);
		FRay2D B;
		B.Dir = FVector2D(FMath::Cos(Theta), FMath::Sin(Theta));
		const RoadGeom::FFillet Fillet = RoadGeom::SolveFillet(A, B, 0.0);

		double AlongA = -1.0;
		double AlongB = -1.0;
		const bool bSolved = RoadGeom::CornerReachAtZeroRadius(Wide, Narrow, Theta, AlongA, AlongB);
		const bool bValidatorSaysStraight = bSolved && AlongA == 0.0 && AlongB == 0.0;

		TestEqual(FString::Printf(
			TEXT("at %.3g rad off pi the validator calls it straight through exactly when the solver does ")
			TEXT("(solver %d, validator reach %.0f / %.0f)"),
			Delta, Fillet.bStraightThrough ? 1 : 0, AlongA, AlongB),
			bValidatorSaysStraight, Fillet.bStraightThrough);
	}

	// THE ANGLE ITSELF MUST NOT MANUFACTURE THE GAP. Two directions a normalise leaves one ulp
	// from exactly opposite: acos(dot) reads them 1.5e-8 off pi, AngleBetween to the ulp.
	const FVector2D Ahead = FVector2D(0.97437006478523525, 0.22495105434386500).GetSafeNormal();
	const FVector2D Back(-Ahead.X, -Ahead.Y + 1.0e-16);
	const double Theta = RoadGeom::AngleBetween(Ahead, Back);
	TestTrue(FString::Printf(TEXT("opposite directions measure as straight through (%.3g off pi)"), UE_DOUBLE_PI - Theta),
		RoadGeom::IsStraightThrough(Theta));
	TestTrue(TEXT("and AngleBetween resolves them far finer than the tolerance, which acos could not"),
		FMath::Abs(UE_DOUBLE_PI - Theta) < 1.0e-12);
	TestTrue(TEXT("while a right angle still reads pi/2, unit or not - the control that it is not a constant"),
		FMath::IsNearlyEqual(RoadGeom::AngleBetween(FVector2D(1.0, 0.0), FVector2D(0.0, 3.0)), UE_DOUBLE_HALF_PI, 1.0e-12));

	return true;
}

namespace TJunctionFit
{
	/** An L of service road: West -> Corner, then a right-angle turn Corner -> Up. */
	struct FL
	{
		URoadNetwork* Network = nullptr;
		FRoadNodeId West;
		FRoadNodeId Corner;
		FRoadNodeId Up;
		FVector2D CornerAt = FVector2D::ZeroVector;
		FVector2D Along = FVector2D::ZeroVector;   // unit, West -> Corner: the line a T continues
	};

	/**
	 * Tilted by BearingDeg and set off the origin, because an axis-aligned L at (0,0) makes
	 * every dot product exactly -1 and the bug cannot show - which is how it reached PIE.
	 */
	FL MakeL(URoadProfile* Service, double BearingDeg)
	{
		FL L;
		L.Network = NewObject<URoadNetwork>(GetTransientPackage());
		const double Bearing = FMath::DegreesToRadians(BearingDeg);
		const FVector2D Dir(FMath::Cos(Bearing), FMath::Sin(Bearing));
		L.CornerAt = FVector2D(12345.6, -7890.1);
		const FVector2D WestAt = L.CornerAt - Dir * 6000.0;
		const FVector2D UpAt = L.CornerAt + RoadGeom::PerpCCW(Dir) * 6000.0;
		L.West = L.Network->AddNode(WestAt);
		L.Corner = L.Network->AddNode(L.CornerAt);
		L.Up = L.Network->AddNode(UpAt);
		L.Network->AddStraightSegment(L.West, L.Corner, Service);
		L.Network->AddStraightSegment(L.Corner, L.Up, Service);
		// What FParallelGuideSource hands the arbiter: the segment's own (B - A) normalised.
		L.Along = (L.CornerAt - WestAt).GetSafeNormal();
		return L;
	}

	/**
	 * Where the "parallel to the service road" guide puts a click: the cursor projected onto
	 * the line through the corner - GuideArbiter's Through + Unit * Dot(Cursor - Through, Unit).
	 * The cursor is deliberately off the line, as a hand is.
	 */
	FVector2D GuidedPoint(const FL& L, double Distance)
	{
		const FVector2D Cursor = L.CornerAt + L.Along * Distance + RoadGeom::PerpCCW(L.Along) * 37.0;
		return L.CornerAt + L.Along * FVector2D::DotProduct(Cursor - L.CornerAt, L.Along);
	}

	/** Dragged OUT of the corner, ending on a free point. */
	ERoadPlacement Outward(const FL& L, const FVector2D& Far, const FRoadPlacementLimits& Limits)
	{
		FRoadSnapResult Snap;
		Snap.Kind = ERoadSnapKind::Free;
		Snap.Position = Far;
		return RoadPlacement::Validate(*L.Network, L.Corner, Snap, Limits);
	}

	/** From the same free point (its node already placed, as the first click does) INTO the corner. */
	ERoadPlacement Inward(const FL& L, const FVector2D& Far, const FRoadPlacementLimits& Limits)
	{
		const FRoadNodeId Start = L.Network->AddNode(Far);
		FRoadSnapResult Snap;
		Snap.Kind = ERoadSnapKind::Node;
		Snap.Node = L.Corner;
		Snap.Position = L.CornerAt;
		const ERoadPlacement Result = RoadPlacement::Validate(*L.Network, Start, Snap, Limits);
		L.Network->RemoveNode(Start);
		return Result;
	}

	/**
	 * Whether the angle the OLD validator computed here was not bitwise pi - acos of a dot one
	 * ulp off -1. Written out rather than calling production code, because it is the measurement
	 * that says this sweep reaches the case that bit, whatever the fix does to Validate.
	 */
	bool AcosSeesOffPi(const FL& L, const FVector2D& Far)
	{
		const FVector2D Outgoing = (Far - L.CornerAt) / FVector2D::Distance(L.CornerAt, Far);
		const FRoadNode* Node = L.Network->GetNode(L.Corner);
		double Best = 1.0;
		for (const FRoadSegmentId& Incident : Node->Incident)
		{
			Best = FMath::Min(Best, FVector2D::DotProduct(Outgoing, L.Network->GetOutgoingTangent(Incident, L.Corner)));
		}
		return FMath::Sin(FMath::Acos(FMath::Clamp(Best, -1.0, 1.0))) >= 1.0e-9;
	}

	const TCHAR* Name(ERoadPlacement Result)
	{
		return Result == ERoadPlacement::Valid ? TEXT("valid") : RoadPlacement::Describe(Result);
	}
}

/**
 * A T MAY BE MADE FROM EITHER END. The PIE report above, at the RoadPlacement level: the real
 * service-road profile the Road tool lays, the (taxiway) NewRoadHalfWidth MakeTunables passes,
 * and the click where the guide actually puts it rather than on a tidy axis.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTJunctionFitsBothWaysTest,
	"Airside.Tool.TJunctionFitsBothWays",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTJunctionFitsBothWaysTest::RunTest(const FString& Parameters)
{
	using namespace TJunctionFit;

	URoadProfile* Service = URoadProfile::MakeServiceRoadTransient();
	// What ARoadNetworkActor::ResolveProfile builds when no Profile is authored, and so what
	// MakeTunables writes into NewRoadHalfWidth today even while a SERVICE road is drawn.
	URoadProfile* Taxiway = URoadProfile::MakeTransient(URoadProfile::StandardTaxiwayWidth,
		URoadProfile::StandardTaxiwayFilletRadius, URoadProfile::StandardTaxiwayWidth * 0.1);
	if (!TestNotNull(TEXT("a service road profile"), Service) || !TestNotNull(TEXT("a taxiway profile"), Taxiway))
	{
		return false;
	}

	FRoadPlacementLimits Limits;
	Limits.MinSegmentLength = 100.0;
	Limits.MinTurnDegrees = 25.0;
	Limits.NewRoadHalfWidth = Taxiway->GetMaxHalfWidth();
	if (!TestTrue(TEXT("the new road is judged wider than the arms it meets, as in PIE - the case the gap bites"),
		Limits.NewRoadHalfWidth > Service->GetMaxHalfWidth()))
	{
		return false;
	}

	// 1. EXACTLY ON THE LINE, as the guide lands it, across enough bearings that some come
	// out of the round trip one ulp off.
	int32 OffPiCases = 0;
	TArray<FString> Refused;
	for (int32 Bearing = 1; Bearing < 90; ++Bearing)
	{
		const FL L = MakeL(Service, Bearing);
		const FVector2D Far = GuidedPoint(L, 4000.0);
		OffPiCases += AcosSeesOffPi(L, Far) ? 1 : 0;
		const ERoadPlacement Out = Outward(L, Far, Limits);
		const ERoadPlacement In = Inward(L, Far, Limits);
		if (Out != ERoadPlacement::Valid || In != ERoadPlacement::Valid)
		{
			Refused.Add(FString::Printf(TEXT("%d deg: out %s, in %s"), Bearing, Name(Out), Name(In)));
		}
	}
	TestTrue(FString::Printf(TEXT("the sweep reaches the one-ulp case the guide produces, or it measures nothing (%d of 89)"), OffPiCases),
		OffPiCases > 0);
	TestTrue(FString::Printf(TEXT("a guided T off an L is valid dragged out of the corner AND into it at every bearing; refused %d: %s"),
		Refused.Num(), *FString::Join(Refused, TEXT("; "))), Refused.Num() == 0);

	// 2. A TENTH OF A DEGREE OFF THE LINE, as a free hand leaves it. Judged at the service road's
	// own width, which is the road actually laid: the arms fit, so both ways are valid.
	{
		FRoadPlacementLimits Own = Limits;
		Own.NewRoadHalfWidth = Service->GetMaxHalfWidth();
		const FL L = MakeL(Service, 13.0);
		const FVector2D Far = L.CornerAt + RoadGeom::Rotate(L.Along, FMath::DegreesToRadians(0.1)) * 4000.0;
		const ERoadPlacement Out = Outward(L, Far, Own);
		const ERoadPlacement In = Inward(L, Far, Own);
		TestEqual(TEXT("0.1 degrees off the line, drawing out of the corner and into it agree"),
			static_cast<int32>(Out), static_cast<int32>(In));
		TestEqual(TEXT("and both are valid, since a same-width road a hair off straight has no corner to speak of"),
			static_cast<int32>(Out), static_cast<int32>(ERoadPlacement::Valid));

		// At the taxiway width a tenth of a degree is a genuine width-step corner the solver
		// would fail too, so no verdict is asserted - only that direction cannot change it.
		TestEqual(TEXT("and at a mismatched width the two directions still agree, whatever they say"),
			static_cast<int32>(Outward(L, Far, Limits)), static_cast<int32>(Inward(L, Far, Limits)));
	}

	// 3. GENUINELY TOO SHORT STILL REFUSES, both ways. The right-angle arm reaches its own
	// half-width along the new road; a road shorter than that cannot hold the corner. Without
	// this leg "always valid" would pass sections 1 and 2.
	{
		FRoadPlacementLimits Own = Limits;
		Own.NewRoadHalfWidth = Service->GetMaxHalfWidth();
		const FL L = MakeL(Service, 13.0);
		const double Short = Service->GetMaxHalfWidth() * 0.8;
		if (!TestTrue(TEXT("the short road is still longer than the plain minimum, so only the corner rule can refuse it"),
			Short > Own.MinSegmentLength))
		{
			return false;
		}
		const FVector2D Far = GuidedPoint(L, Short);
		TestEqual(TEXT("a road too short for the corner is refused dragged out of it"),
			static_cast<int32>(Outward(L, Far, Own)), static_cast<int32>(ERoadPlacement::TooShortForCorner));
		TestEqual(TEXT("and refused drawn into it"),
			static_cast<int32>(Inward(L, Far, Own)), static_cast<int32>(ERoadPlacement::TooShortForCorner));
	}

	return true;
}

#endif
