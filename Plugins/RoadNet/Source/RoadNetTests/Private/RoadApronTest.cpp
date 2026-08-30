#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Model/RoadApron.h"
#include "Model/RoadNetwork.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadApronTest,
	"RoadNet.Model.Apron",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadApronTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

	// A square of concrete. An apron is a surface with no cross-section: no profile, no
	// arms, no fillets, and nothing for the junction solver to trim.
	FApronSurface Slab;
	Slab.Outline = {
		FVector2D(0.0, 0.0),
		FVector2D(1234567.89, 0.0),
		FVector2D(1234567.89, 8000.0),
		FVector2D(0.0, 8000.0) };
	Slab.SurfaceMaterialSlot = TEXT("Concrete");

	const FApronId Id = Net->AddApron(MoveTemp(Slab));
	TestTrue(TEXT("a new apron handle is set"), Id.IsSet());

	{
		const FApronSurface* Stored = Net->GetApron(Id);
		if (TestNotNull(TEXT("the apron resolves"), Stored))
		{
			TestEqual(TEXT("its outline survived the move"), Stored->Outline.Num(), 4);
			TestEqual(TEXT("its material slot survived"),
				Stored->SurfaceMaterialSlot, FName(TEXT("Concrete")));

			// Positions are double-precision throughout - an apron the size of a real
			// airport is tens of thousands of units across and must not round.
			//
			// The coordinate is chosen so the claim can FAIL. 10000.0 is exactly
			// representable as a float and would survive a narrowing round-trip
			// unchanged; 1234567.89 narrows to 1234567.875, which this catches.
			TestEqual(TEXT("a corner is stored exactly"), Stored->Outline[2].X, 1234567.89);
		}
	}

	// Generation checking, the whole point of the handle: a recycled slot must NOT resolve
	// through the old handle, or an edit silently lands on whatever took the slot over.
	{
		TestTrue(TEXT("the apron removes"), Net->RemoveApron(Id));
		TestNull(TEXT("a removed apron no longer resolves"), Net->GetApron(Id));

		FApronSurface Second;
		Second.Outline = { FVector2D(0.0, 0.0), FVector2D(1.0, 0.0), FVector2D(0.0, 1.0) };
		const FApronId Recycled = Net->AddApron(MoveTemp(Second));

		TestEqual(TEXT("the slot was reused"), Recycled.Index, Id.Index);
		TestNotEqual(TEXT("but the generation moved on"), Recycled.Generation, Id.Generation);
		TestNull(TEXT("the stale handle does not resolve"), Net->GetApron(Id));
		TestNotNull(TEXT("the fresh handle does"), Net->GetApron(Recycled));
	}

	// The three claims RoadApron.h's header makes, pinned together: an apron carries no
	// profile, never enters the junction solve, and generates no guidelines.
	//
	// Asserting only that AddApron adds no segment and no node would be equally true of an
	// AddApron that did nothing whatsoever, so this runs the two passes that WOULD touch a
	// surface - the junction solver and the guideline derivation - over a network holding
	// nothing but an apron, and confirms the apron is still there and neither pass
	// produced anything.
	{
		URoadNetwork* ApronOnly = NewObject<URoadNetwork>(GetTransientPackage());

		FApronSurface Ramp;
		Ramp.Outline = {
			FVector2D(0.0, 0.0),
			FVector2D(30000.0, 0.0),
			FVector2D(30000.0, 20000.0),
			FVector2D(0.0, 20000.0) };
		Ramp.SurfaceMaterialSlot = TEXT("Asphalt");
		TestTrue(TEXT("an apron-only network holds its apron"),
			ApronOnly->AddApron(MoveTemp(Ramp)).IsSet());

		// An apron is a SEPARATE collection from segments, or the junction solver would
		// try to trim it and there is nothing there to trim.
		TestEqual(TEXT("adding an apron adds no segment"), ApronOnly->GetSegments().Num(), 0);
		TestEqual(TEXT("and no node"), ApronOnly->GetNodes().Num(), 0);

		const FRoadSolveResult ApronSolved = FRoadNetworkSolver::SolveAll(*ApronOnly);
		FRoadGuidelineBuilder::Build(*ApronOnly, ApronSolved);

		TestEqual(TEXT("the apron survives both passes"), ApronOnly->GetAprons().Num(), 1);
		TestEqual(TEXT("the junction solver found nothing to solve"),
			ApronSolved.NodeResults.Num(), 0);
		TestEqual(TEXT("and it failed nothing either"), ApronSolved.FailedNodes, 0);
		TestEqual(TEXT("the derivation produced no guideline edge"),
			ApronOnly->GetGuidelineEdges().Num(), 0);
		TestEqual(TEXT("and no guideline node"),
			ApronOnly->GetGuidelineNodes().Num(), 0);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
