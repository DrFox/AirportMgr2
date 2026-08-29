#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
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
		FVector2D(10000.0, 0.0),
		FVector2D(10000.0, 8000.0),
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
			TestEqual(TEXT("a corner is stored exactly"), Stored->Outline[2].X, 10000.0);
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

	// Aprons are a SEPARATE collection from segments. An apron must never appear in the
	// segment list, or the junction solver would try to trim it and there is nothing there
	// to trim - no arms, no profile, no cut vertices.
	TestEqual(TEXT("adding an apron adds no segment"), Net->GetSegments().Num(), 0);
	TestEqual(TEXT("and no node"), Net->GetNodes().Num(), 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
