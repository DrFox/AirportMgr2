#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Tool/SnapGuideChain.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * An EMPTY network, because none of the three sources installed so far reads one -
	 * Extending and PointAlign are handed their geometry by the tool, and World is absolute.
	 * A network with roads in it would suggest these sources consult it, which is exactly
	 * what stage 2 changes.
	 */
	URoadNetwork* EmptyNetwork()
	{
		return NewObject<URoadNetwork>(GetTransientPackage());
	}

	/** A plot's back corner: swinging around the far end of an east-west frontage. */
	FGuideAnchor Frontage()
	{
		FGuideAnchor Anchor;
		Anchor.Origin = FVector2D(3000.0, 0.0);
		Anchor.Reference = FVector2D(1.0, 0.0);
		Anchor.ReferenceAt = FVector2D::ZeroVector;
		Anchor.ReferenceName = TEXT("the frontage");
		return Anchor;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideChainProposesTheFrontageAndItsPerpendicularTest,
	"Airside.Tool.GuideChainProposesTheFrontageAndItsPerpendicular",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideChainProposesTheFrontageAndItsPerpendicularTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Network = EmptyNetwork();
	if (!TestNotNull(TEXT("a network"), Network)) { return false; }

	const FSnapGuideChain Chain;
	// THE NUMBER MOVES WITH EVERY SOURCE ADDED, and that is exactly why it is asserted: a
	// source written, declared and never installed in the constructor would be invisible
	// otherwise - its candidates simply never appear, and every other test of the chain still
	// passes. Stage 2 takes this from 3 to 7, one at a time.
	TestEqual(TEXT("the chain installs every source it declares"), Chain.NumSources(), 5);

	const FGuideAnchor Anchor = Frontage();

	// A CORNER DRAGGED ALMOST SQUARE: 2000 uu out and 50 uu along, which is about 1.4
	// degrees off the perpendicular - inside the 7-degree tolerance.
	const FVector2D Cursor = Anchor.Origin + FVector2D(50.0, 2000.0);

	const SnapGuide::FResult Result = Chain.Resolve(
		*Network, Anchor, Cursor, SnapGuide::FResult());

	if (!TestTrue(TEXT("a corner dragged near square is offered a guide"), Result.bActive))
	{
		return false;
	}

	TestEqual(TEXT("the perpendicular of the tool's own reference is what wins"),
		static_cast<int32>(Result.Winners[0].Source), static_cast<int32>(SnapGuide::ESource::Extending));
	TestEqual(TEXT("and it is described by the name the TOOL gave its reference"),
		Result.Winners[0].Description, FString(TEXT("square to the frontage")));

	// THE DASHED LINE POINTS AT THE EDGE, not along the guide - design section 6. This is the
	// field the overlay draws to, so an anchor whose ReferenceAt did not travel would draw a
	// line to the world origin and look like a bug in the gesture.
	TestTrue(TEXT("the reference point the tool supplied travels to the winner"),
		Result.Winners[0].ReferenceAt.Equals(Anchor.ReferenceAt, 1.0e-6));

	// EXACTLY SQUARE, not nearly: the constrained point is what the corner becomes.
	TestTrue(TEXT("the constrained point is exactly square to the frontage"),
		FMath::IsNearlyEqual(Result.Point.X, Anchor.Origin.X, 1.0e-6));

	// AND THE PARALLEL IS PROPOSED TOO. A corner dragged back along the frontage gets the
	// other of Extending's two candidates - without this leg the source could be proposing
	// one direction and the test above would not notice.
	const SnapGuide::FResult AlongIt = Chain.Resolve(
		*Network, Anchor, Anchor.Origin + FVector2D(2000.0, 30.0), SnapGuide::FResult());
	TestEqual(TEXT("dragging along the frontage gets the frontage's own direction"),
		AlongIt.Winners[0].Description, FString(TEXT("along the frontage")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideChainPrefersTheFrontageOverTheWorldGridTest,
	"Airside.Tool.GuideChainPrefersTheFrontageOverTheWorldGrid",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideChainPrefersTheFrontageOverTheWorldGridTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Network = EmptyNetwork();
	if (!TestNotNull(TEXT("a network"), Network)) { return false; }

	const FSnapGuideChain Chain;

	// A FRONTAGE ON A WORLD AXIS - every plot off an east-west service road. Extending
	// proposes 0 and 90; World proposes 0, 45, 90, 135. The 90s tie exactly.
	FGuideAnchor Anchor = Frontage();
	const FVector2D Cursor = Anchor.Origin + FVector2D(60.0, 2000.0);

	const SnapGuide::FResult Result = Chain.Resolve(
		*Network, Anchor, Cursor, SnapGuide::FResult());
	TestEqual(TEXT("a tie between the frontage and a world axis goes to the frontage"),
		static_cast<int32>(Result.Winners[0].Source), static_cast<int32>(SnapGuide::ESource::Extending));

	// CONTROL LEG: World was a live competitor, not an absent one. With no reference the
	// Extending source proposes nothing and the same cursor gets the world axis instead - so
	// the assertion above is measuring the tiebreak rather than an empty list.
	Anchor.Reference = FVector2D::ZeroVector;
	const SnapGuide::FResult WorldOnly = Chain.Resolve(
		*Network, Anchor, Cursor, SnapGuide::FResult());
	TestTrue(TEXT("with no reference the world grid still answers"), WorldOnly.bActive);
	TestEqual(TEXT("and it is the world axis that does"),
		static_cast<int32>(WorldOnly.Winners[0].Source), static_cast<int32>(SnapGuide::ESource::World));
	TestEqual(TEXT("named as an angle, since the grid has no thing to point at"),
		WorldOnly.Winners[0].Description, FString(TEXT("90 degrees")));
	TestTrue(TEXT("and its line points back at the corner it swings around"),
		WorldOnly.Winners[0].ReferenceAt.Equals(Anchor.Origin, 1.0e-6));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideChainOffersNothingBetweenCandidatesTest,
	"Airside.Tool.GuideChainOffersNothingBetweenCandidates",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideChainOffersNothingBetweenCandidatesTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Network = EmptyNetwork();
	if (!TestNotNull(TEXT("a network"), Network)) { return false; }

	const FSnapGuideChain Chain;

	FGuideAnchor Anchor = Frontage();
	Anchor.Reference = FVector2D::ZeroVector;   // world axes only, 45 degrees apart

	// 22 degrees off +X: 22 from one axis, 23 from the next, both past the 7-degree
	// tolerance. THE GUIDE MUST BE OFF MOST OF THE TIME, or it is a constraint the player
	// never asked for rather than an aid.
	const double Radians = FMath::DegreesToRadians(22.0);
	const FVector2D Cursor = Anchor.Origin
		+ FVector2D(FMath::Cos(Radians), FMath::Sin(Radians)) * 2000.0;

	const SnapGuide::FResult Result = Chain.Resolve(
		*Network, Anchor, Cursor, SnapGuide::FResult());
	TestFalse(TEXT("a cursor between two world axes is offered neither"), Result.bActive);

	return true;
}

#endif
