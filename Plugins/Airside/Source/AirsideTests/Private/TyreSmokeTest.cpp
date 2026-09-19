#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "Present/RoadNetworkActor.h"
#include "Present/TyreSmoke.h"

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

#endif
