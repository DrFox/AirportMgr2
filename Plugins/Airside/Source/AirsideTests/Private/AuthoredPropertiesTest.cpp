#include "CoreMinimal.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadMaterialSet.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAuthoredPropertiesTest,
	"Airside.Present.AuthoredPropertiesUntouched",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAuthoredPropertiesTest::RunTest(const FString& Parameters)
{
	// NOTHING THE ACTOR DOES MAY WRITE A PROPERTY SOMEBODY AUTHORED.
	//
	// This has now cost two separate incidents on the same afternoon, from the same mistake
	// written twice. ApplyContentDefaults filled MaterialSet, SurfaceMaterial, ApronMaterial,
	// GhostMaterial and StandDefinition whenever it found them null; ResolveProfile - copied
	// from it - did the same to Profile.
	//
	// It looks harmless. It is not, because these are EditAnywhere properties on an actor
	// that rebuilds at DESIGN TIME: the write lands on the level and is saved. An airport
	// deliberately left on a single material acquired a material set it never asked for, its
	// roads changed appearance permanently, and clearing the property by hand lasted exactly
	// until the next rebuild. Nothing reported any of it, because from the code's point of
	// view it was filling in a helpful default.
	//
	// The defaults still exist - see ARoadNetworkActor::ResolveMaterialSet and friends - they
	// are just RESOLVED rather than STORED. This test is the difference between that being
	// true today and it staying true.
	ARoadNetworkActor* Actor = NewObject<ARoadNetworkActor>(GetTransientPackage());
	if (!TestNotNull(TEXT("actor constructed"), Actor))
	{
		return false;
	}

	// 1. A FRESH ACTOR AUTHORS NOTHING. If construction fills any of these, everything below
	//    is measuring a state the user never chose.
	{
		TestNull(TEXT("a new actor has no material set"), Actor->MaterialSet.Get());
		TestNull(TEXT("no surface material"), Actor->SurfaceMaterial.Get());
		TestNull(TEXT("no apron material"), Actor->ApronMaterial.Get());
		TestNull(TEXT("no ghost material"), Actor->GhostMaterial.Get());
		TestNull(TEXT("no stand definition"), Actor->StandDefinition.Get());
		TestNull(TEXT("and no profile"), Actor->Profile.Get());
	}

	// 2. AND STILL AUTHORS NOTHING AFTER DOING ITS WORK.
	//
	//    Rebuilding is what used to fill them, so it is what has to be exercised. Placing and
	//    connecting go through ResolveProfile, which is where the second copy lived.
	{
		Actor->RebuildMesh();

		const int32 A = Actor->PlaceNode(FVector2D::ZeroVector);
		const int32 B = Actor->PlaceNode(FVector2D(5000.0, 0.0));
		if (A != INDEX_NONE && B != INDEX_NONE)
		{
			Actor->ConnectNodes(A, B);
		}
		Actor->RebuildMesh();

		TestNull(TEXT("rebuilding does not assign a material set"), Actor->MaterialSet.Get());
		TestNull(TEXT("nor a surface material"), Actor->SurfaceMaterial.Get());
		TestNull(TEXT("nor an apron material"), Actor->ApronMaterial.Get());
		TestNull(TEXT("nor a ghost material"), Actor->GhostMaterial.Get());
		TestNull(TEXT("nor a stand definition"), Actor->StandDefinition.Get());

		// THE ONE THAT ACTUALLY HAPPENED. Drawing a road resolves a profile for the new
		// segment; it must not write that profile onto the actor, which is what silently
		// replaced a 400 uu road with a 2300 uu one and saved it into the level.
		TestNull(TEXT("and drawing a road does not assign a profile"), Actor->Profile.Get());
	}

	// 3. AN AUTHORED VALUE IS NEVER OVERRIDDEN EITHER. The other half of the contract: a
	//    resolver that replaced what somebody chose would be the same bug pointing the other
	//    way, and is how a tuned 400 uu FallbackWidth got outranked by a class default.
	{
		URoadProfile* Mine = URoadProfile::MakeTransient(1234.0, 500.0, 100.0);
		Actor->Profile = Mine;
		Actor->RebuildMesh();

		TestEqual(TEXT("an authored profile survives a rebuild"),
			Actor->Profile.Get(), Mine);
		TestEqual(TEXT("and is what the actor resolves"),
			Actor->ResolveProfileForTest(), static_cast<const URoadProfile*>(Mine));
	}

	return true;
}

#endif
