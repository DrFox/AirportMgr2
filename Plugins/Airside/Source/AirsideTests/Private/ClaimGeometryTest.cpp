#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/GroundTraffic.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The two maps between route distance and edge distance that every claim is built on.
 *
 * WORTH A TEST OF THEIR OWN because neither is readable off any behavioural fixture. A
 * mirror applied the wrong way round yields From > To, and a half-open interval that way
 * round conflicts with NOTHING - so the agent is granted line another agent is standing on,
 * and the only symptom is two bodies in one place, several ticks later, on a reversed step.
 * Every other traffic test asserts on where an agent stopped; this one asserts on the
 * arithmetic that decided it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClaimGeometryTest,
	"Airside.Model.Traffic.ClaimGeometry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FClaimGeometryTest::RunTest(const FString& Parameters)
{
	using FGeom = UGroundTraffic::FClaimGeometry;

	// One step: route 1000..1400, over an edge 400 long. The window covers 1100..1300.
	const double StepBegin = 1000.0;
	const double Length = 400.0;
	const double Lo = 1100.0;
	const double Hi = 1300.0;

	// FORWARD: edge distance is simply route distance less the step's start.
	const FGeom::FEdgeInterval Forward = FGeom::EdgeInterval(Lo, Hi, StepBegin, Length, false);
	TestEqual(TEXT("forward interval starts 100 uu into the edge"), Forward.From, 100.0);
	TestEqual(TEXT("forward interval ends 300 uu into the edge"), Forward.To, 300.0);

	// REVERSED: the agent walks the edge from B, so the interval mirrors through the edge
	// length - AND THE ENDS SWAP. From < To is the property that matters: a half-open
	// interval the other way round overlaps nothing, so a wrong mirror does not refuse, it
	// silently grants.
	const FGeom::FEdgeInterval Reversed = FGeom::EdgeInterval(Lo, Hi, StepBegin, Length, true);
	TestTrue(TEXT("a reversed step's interval still has From < To"), Reversed.From < Reversed.To);
	TestEqual(TEXT("reversed interval mirrors Hi"), Reversed.From, 100.0);
	TestEqual(TEXT("reversed interval mirrors Lo"), Reversed.To, 300.0);

	// The mirror is only visible when the interval is NOT centred on the edge, so pin an
	// off-centre one too: route 1000..1100 forward is edge 0..100, reversed is edge 300..400.
	const FGeom::FEdgeInterval Near = FGeom::EdgeInterval(1000.0, 1100.0, StepBegin, Length, false);
	TestEqual(TEXT("forward: the near end of the step is the near end of the edge"), Near.From, 0.0);
	TestEqual(TEXT("forward: 100 uu of it"), Near.To, 100.0);

	const FGeom::FEdgeInterval NearReversed = FGeom::EdgeInterval(1000.0, 1100.0, StepBegin, Length, true);
	TestTrue(TEXT("off-centre reversed interval still has From < To"), NearReversed.From < NearReversed.To);
	TestEqual(TEXT("reversed: the near end of the step is the FAR end of the edge"), NearReversed.From, 300.0);
	TestEqual(TEXT("reversed: and it runs to the edge's end"), NearReversed.To, 400.0);

	// A blocker holding edge 50..100 of the same edge. FORWARD, the first thing in the
	// agent's way is the blocker's From, at route 1050.
	TestEqual(TEXT("forward blocker's nearest boundary ahead"),
		FGeom::BoundaryAhead(StepBegin, Length, false, 50.0, 100.0), 1050.0);

	// REVERSED, the agent meets the blocker's To first, mirrored: StepBegin + (L - To).
	TestEqual(TEXT("reversed blocker's nearest boundary ahead is StepBegin + (L - To)"),
		FGeom::BoundaryAhead(StepBegin, Length, true, 50.0, 100.0), StepBegin + (Length - 100.0));
	TestEqual(TEXT("which is route 1300 on this step"),
		FGeom::BoundaryAhead(StepBegin, Length, true, 50.0, 100.0), 1300.0);

	// A blocker at the very start of the edge bars the way at the step's start going
	// forward, and at its END going backwards - the two ends of the same asymmetry.
	TestEqual(TEXT("forward, a blocker at the edge's A end bars the step's start"),
		FGeom::BoundaryAhead(StepBegin, Length, false, 0.0, 20.0), 1000.0);
	TestEqual(TEXT("reversed, that same blocker is at the far end of the step"),
		FGeom::BoundaryAhead(StepBegin, Length, true, 0.0, 20.0), 1380.0);

	// THE TWO AGREE ON ONE THING, which is what makes them one pair rather than two
	// functions: the boundary of a claim built by EdgeInterval, read back by BoundaryAhead,
	// is where that claim's owner actually is. On a reversed step the interval's To mirrors
	// back to Lo, the nearest route distance the blocker occupies.
	TestEqual(TEXT("BoundaryAhead inverts EdgeInterval on a reversed step"),
		FGeom::BoundaryAhead(StepBegin, Length, true, Reversed.From, Reversed.To), Lo);
	TestEqual(TEXT("and on a forward one"),
		FGeom::BoundaryAhead(StepBegin, Length, false, Forward.From, Forward.To), Lo);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
