#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "Present/RoadNetworkActor.h"
#include "Present/TyreSmoke.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE PUFF POOL, which is the part of the effect that can go wrong silently.
 *
 * The model half is tested world-free (Airside.Model.TouchdownEdge). What this covers is the
 * pool itself: that a puff lives and then stops, that the pool is a ceiling rather than a
 * leak, and that running out steals the oldest instead of dropping the newest. None of those
 * show up as an error - they show up as smoke that never fades, or a component count that
 * climbs all session, or a landing with no smoke at a busy moment.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTyreSmokePoolTest,
	"Airside.Present.TyreSmokePool",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTyreSmokePoolTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world to register components in"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor spawned"), Actor)) { return false; }

	UTyreSmoke* Smoke = NewObject<UTyreSmoke>(Actor);
	Smoke->PoolSize = 3;
	Smoke->PuffSeconds = 1.0;
	Smoke->Initialise(Actor, UMaterial::GetDefaultMaterial(MD_Surface));

	TestEqual(TEXT("a fresh pool has nothing alive"), Smoke->LivePuffCountForTest(), 0);

	// A PUFF LIVES AND THEN STOPS. The second half is the half that matters: a puff that
	// never retires is a sphere left sitting on the runway for the rest of the session.
	Smoke->Puff(FVector(0.0, 0.0, 0.0), 2842.0);
	TestEqual(TEXT("one puff is alive"), Smoke->LivePuffCountForTest(), 1);

	Smoke->Advance(0.5);
	TestEqual(TEXT("and still alive halfway through its life"), Smoke->LivePuffCountForTest(), 1);

	Smoke->Advance(0.6);
	TestEqual(TEXT("and gone once its life is up"), Smoke->LivePuffCountForTest(), 0);

	// THE POOL IS A CEILING. Ten puffs into a pool of three is three, not ten: the worst
	// case has to be knowable, because this runs for as long as the airport does.
	for (int32 Index = 0; Index < 10; ++Index)
	{
		Smoke->Puff(FVector(Index * 100.0, 0.0, 0.0), 2842.0);
	}
	TestEqual(TEXT("ten puffs into a pool of three leaves three"), Smoke->LivePuffCountForTest(), 3);

	// AND IT STEALS THE OLDEST. A refused puff is a landing with no smoke, which reads as the
	// feature being broken; a stolen one is a puff that ended early, which nobody can see.
	// Proven by the oldest being the one that vanishes: age the pool to just short of death,
	// add one, and the count must hold rather than fall as the stale ones expire together.
	Smoke->Advance(0.9);
	TestEqual(TEXT("three still alive just short of death"), Smoke->LivePuffCountForTest(), 3);
	Smoke->Puff(FVector(9999.0, 0.0, 0.0), 2842.0);
	Smoke->Advance(0.2);
	TestEqual(TEXT("the three old ones expired and the fresh one survives them"),
		Smoke->LivePuffCountForTest(), 1);

	// NO MATERIAL MEANS NO PUFFS, and it must not crash on the way to deciding that.
	UTyreSmoke* Silent = NewObject<UTyreSmoke>(Actor);
	Silent->Initialise(Actor, nullptr);
	Silent->Puff(FVector::ZeroVector, 2842.0);
	Silent->Advance(0.1);
	TestEqual(TEXT("with no material nothing is drawn"), Silent->LivePuffCountForTest(), 0);
	return true;
}

/**
 * ONE LIST NOW (issue #192 item 2). FPuff used to be a plain struct beside two
 * index-parallel UPROPERTY arrays (PuffMeshes/PuffInstances) that existed only to root what
 * the struct's own TObjectPtrs could not - DynamicMeshSink.h calls that exact shape "the
 * defect this codebase has already paid for once". FTyreSmokePuff is a USTRUCT with
 * UPROPERTY pointers now, so Puffs alone should root them.
 *
 * PROVEN BY REFLECTION, NOT BY A COLLECTION (#463). This test used to spawn past the pool, let every puff
 * expire, call CollectGarbage and read each Mesh back non-null - but a puff's mesh is a component the ACTOR
 * owns (the actor's own component list keeps it alive), so the collection left it standing whether or not
 * FTyreSmokePuff::Mesh was a UPROPERTY: it passed with the UPROPERTY deleted, at 0.23 s a run. What can
 * actually regress is the declaration, so that is what is asserted: both pointers on the puff struct and the
 * pool array on the component are reflected, which is what makes the collector see them.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTyreSmokePoolStaysRootedTest,
	"Airside.Present.TyreSmokePoolStaysRooted",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTyreSmokePoolStaysRootedTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world to register components in"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor spawned"), Actor)) { return false; }

	UTyreSmoke* Smoke = NewObject<UTyreSmoke>(Actor);

	Smoke->PoolSize = 4;
	Smoke->PuffSeconds = 1.0;
	Smoke->Initialise(Actor, UMaterial::GetDefaultMaterial(MD_Surface));

	// SPAWN PAST THE POOL, THEN LET EVERY PUFF EXPIRE - the pool has cycled through every
	// slot at least once by the time this collection runs.
	for (int32 Index = 0; Index < 9; ++Index)
	{
		Smoke->Puff(FVector(Index * 100.0, 0.0, 0.0), 2842.0);
	}
	Smoke->Advance(2.0);
	if (!TestEqual(TEXT("everything has expired"), Smoke->LivePuffCountForTest(), 0))
	{
		return false;
	}

	TestEqual(TEXT("the pool is exactly PoolSize long however many puffs were asked for"),
		Smoke->PoolCountForTest(), Smoke->PoolSize);
	TestTrue(TEXT("and every pooled puff owns its mesh component"), Smoke->EveryPuffHasAMeshForTest());

	// THE DECLARATIONS THE COLLECTOR READS. Each is a UPROPERTY, or the property is absent from the
	// reflection data and the lookup below is null.
	const UScriptStruct* PuffStruct = FTyreSmokePuff::StaticStruct();
	TestNotNull(TEXT("FTyreSmokePuff::Mesh is a reflected object pointer"),
		CastField<FObjectPropertyBase>(PuffStruct->FindPropertyByName(TEXT("Mesh"))));
	TestNotNull(TEXT("FTyreSmokePuff::Instance is a reflected object pointer"),
		CastField<FObjectPropertyBase>(PuffStruct->FindPropertyByName(TEXT("Instance"))));
	const FArrayProperty* Pool = CastField<FArrayProperty>(UTyreSmoke::StaticClass()->FindPropertyByName(TEXT("Puffs")));
	if (TestNotNull(TEXT("UTyreSmoke::Puffs is a reflected array"), Pool))
	{
		const FStructProperty* Inner = CastField<FStructProperty>(Pool->Inner);
		TestTrue(TEXT("of FTyreSmokePuff, so the pointers inside it are walked"),
			Inner != nullptr && Inner->Struct == PuffStruct);
	}

	return true;
}

#endif
