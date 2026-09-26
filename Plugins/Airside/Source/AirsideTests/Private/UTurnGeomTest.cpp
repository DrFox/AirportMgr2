#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Content/AirsideSettings.h"
#include "Model/Vehicle.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/UTurnGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUTurnBalloonTest, "Airside.Solve.UTurnBalloon",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUTurnBalloonTest::RunTest(const FString& Parameters)
{
	// The two lane ends of a Narrow road at a dead end facing +Y, and a 699 uu lock radius -
	// the 8.5 m bowser's when the balloon was prototyped (it is 510 at 6.2 m). The figure is the
	// input the prototype was run with, not a claim about today's truck.
	const FVector2D In(-150.0, 0.0);
	const FVector2D Out(150.0, 0.0);
	const FVector2D Axis(0.0, 1.0);
	const double Needed = 699.0;

	double Radius = 0.0;
	const TArray<UTurnGeom::FPiece> Pieces = UTurnGeom::Balloon(In, Out, Axis, Needed, &Radius);
	// RESHAPED 2026-09-25: the lane change out (two), the half circle in CirclePieces, the lane
	// change back (two) - was six, with the circle in two quarters that each delivered only
	// 0.707 of its radius.
	if (!TestEqual(TEXT("lane change out, half circle in CirclePieces, lane change back"), Pieces.Num(), 4 + UTurnGeom::CirclePieces))
	{
		return false;
	}
	TestTrue(TEXT("it ends exactly on the leaving lane, so the handle joins"), Pieces.Last().End == Out);

	FVector2D Prev = In;
	for (int32 Index = 0; Index < Pieces.Num(); ++Index)
	{
		const UTurnGeom::FPiece& Piece = Pieces[Index];
		TestTrue(FString::Printf(TEXT("piece %d is followable by the vehicle it was sized for"), Index),
			GuidelineGeom::TightestRadius(Prev, Piece.Control, Piece.End) >= Needed);
		if (Index + 1 < Pieces.Num())
		{
			const FVector2D Arriving = (Piece.End - Piece.Control).GetSafeNormal();
			const FVector2D Leaving = (Pieces[Index + 1].Control - Piece.End).GetSafeNormal();
			TestTrue(FString::Printf(TEXT("no heading kink at joint %d"), Index),
				FVector2D::DotProduct(Arriving, Leaving) > 0.9999);
		}
		Prev = Piece.End;
	}

	TestTrue(TEXT("it leaves the arriving lane straight on, along the axis"),
		FVector2D::DotProduct((Pieces[0].Control - In).GetSafeNormal(), Axis) > 0.9999);
	TestTrue(TEXT("and joins the leaving lane heading back down the road"),
		FVector2D::DotProduct((Out - Pieces.Last().Control).GetSafeNormal(), -Axis) > 0.9999);

	// THE FOOTPRINT is still the prototype's: the Python prototype of 2026-09-23 settled on
	// R = 1091 at 699 needed, and FootprintFor keeps that search to size the box.
	const UTurnGeom::FFootprint Box = UTurnGeom::FootprintFor(150.0, Needed);
	TestTrue(FString::Printf(TEXT("the ruled box is the prototype's R = 1091 (%.0f)"), Box.HalfWidth),
		Box.HalfWidth > 1000.0 && Box.HalfWidth < 1200.0);
	TestTrue(FString::Printf(TEXT("and the reshaped circle (%.0f) fits inside its width"), Radius), Radius <= Box.HalfWidth);

	TestEqual(TEXT("coincident ends give no balloon"), UTurnGeom::Balloon(In, In, Axis, Needed).Num(), 0);
	TestEqual(TEXT("a zero axis gives no balloon"), UTurnGeom::Balloon(In, Out, FVector2D::ZeroVector, Needed).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUTurnBalloonFootprintTest, "Airside.Solve.UTurnBalloonFootprint",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUTurnBalloonFootprintTest::RunTest(const FString& Parameters)
{
	// SAME FOOTPRINT, GENTLER CURVES (user ruling 2026-09-25): a dead end is the size of the
	// bowser's balloon on every tier, and inside that box the curves are as gentle as it allows.
	// Measured on GuidelineGeom's samples - the line that is driven - at the RESOLVED bowser's lock
	// (what the builder sizes every dead end for) and the three service tiers' half lane spacings
	// (3, 3.5 and 4.5 m lanes). THE FOOTPRINT FOLLOWS THE DESIGN VEHICLE (controller ruling
	// 2026-09-25, on #279 moving the bowser onto rigidCab1: "size ground geometry for the largest
	// vehicle admitted"): the box is derived from that lock here, never typed - the literal
	// 1818 x 727 pinned the old mesh's 510.1 uu lock and went red on 502 (1789.5 x 715.8).
	const double Bowser = UAirsideSettings::ResolveLargestServiceVehicle().TightestFollowableRadius();
	const double RigLock = UAirsideSettings::ResolveRigVehicle().Chassis.TightestFollowableRadius();
	const FVector2D Axis(0.0, 1.0);
	struct FTier { const TCHAR* Name; double Half; };
	for (const FTier Tier : { FTier{ TEXT("Narrow"), 150.0 }, FTier{ TEXT("Standard"), 175.0 }, FTier{ TEXT("Wide"), 225.0 } })
	{
		const FVector2D In(-Tier.Half, 0.0);
		const FVector2D Out(Tier.Half, 0.0);
		const UTurnGeom::FFootprint Box = UTurnGeom::FootprintFor(Tier.Half, Bowser);
		double Radius = 0.0;
		const TArray<UTurnGeom::FPiece> Pieces = UTurnGeom::Balloon(In, Out, Axis, Bowser, &Radius);
		const UTurnGeom::FMeasured Drawn = UTurnGeom::Measure(Pieces, In, Out, Axis);
		AddInfo(FString::Printf(TEXT("%s: box %.1f past the ends x %.1f either side; balloon %.1f x %.1f, circle R %.1f, tightest %.1f uu"),
			Tier.Name, Box.Reach, Box.HalfWidth, Drawn.Reach, Drawn.HalfWidth, Radius, Drawn.Tightest));

		// THE BOX IS THE OLD CONSTRUCTION'S, run on the bowser's lock: R grown 3% a step from the
		// lock, which binds before any tier's spacing does - so the same box on every tier, as wide
		// as R and HeightFactor + 1 of it long, and never narrower than the lock it was grown from.
		AddInfo(FString::Printf(TEXT("%s: bowser lock %.1f, rig lock %.1f"), Tier.Name, Bowser, RigLock));
		TestTrue(FString::Printf(TEXT("%s: the box is at least the bowser's lock wide (%.1f vs %.1f)"), Tier.Name, Box.HalfWidth, Bowser),
			Box.HalfWidth >= Bowser);
		TestTrue(FString::Printf(TEXT("%s: and grown from it, not from the tier (%.1f within 1.5x of %.1f)"), Tier.Name, Box.HalfWidth, Bowser),
			Box.HalfWidth <= 1.5 * Bowser);
		TestEqual(FString::Printf(TEXT("%s: the reach is (HeightFactor + 1) x the half-width"), Tier.Name),
			Box.Reach, (UTurnGeom::HeightFactor + 1.0) * Box.HalfWidth, 0.01);
		TestEqual(FString::Printf(TEXT("%s: the same box as the Narrow tier's - the lock binds, not the spacing"), Tier.Name),
			Box.HalfWidth, UTurnGeom::FootprintFor(150.0, Bowser).HalfWidth, 0.01);
		TestTrue(FString::Printf(TEXT("%s: the reshaped balloon's tightest piece clears the bowser's own lock (%.1f vs %.1f)"),
			Tier.Name, Drawn.Tightest, Bowser), Drawn.Tightest > Bowser);
		// NO LARGER ON EITHER AXIS: a quadratic bulges past its circle by a hair at mid-piece,
		// which is why the circle's extremes are piece ENDS (CirclePieces is even).
		TestTrue(FString::Printf(TEXT("%s: the balloon reaches no further than the box (%.1f vs %.1f)"), Tier.Name, Drawn.Reach, Box.Reach),
			Drawn.Reach <= Box.Reach + 0.5);
		TestTrue(FString::Printf(TEXT("%s: nor wider (%.1f vs %.1f)"), Tier.Name, Drawn.HalfWidth, Box.HalfWidth),
			Drawn.HalfWidth <= Box.HalfWidth + 0.5);
		// GENTLER: the old shape's tightest piece was 514 uu on every tier; the reshaped one
		// clears even the rig's lock, so the rig is refused at a dead end on its TRAILER, not its
		// lock (Airside.Build.DesignVehicle.WideDeadEndRefusesRig).
		TestTrue(FString::Printf(TEXT("%s: the tightest piece (%.1f) clears the rig's %.1f lock"), Tier.Name, Drawn.Tightest, RigLock),
			Drawn.Tightest > RigLock);
	}
	return true;
}

#endif
