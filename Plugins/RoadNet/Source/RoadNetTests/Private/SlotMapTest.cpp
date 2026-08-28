#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadHandles.h"
#include "Model/RoadSlotMap.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	struct FTestItem
	{
		int32 Generation = 0;
		bool  bAlive = false;
		int32 Payload = 0;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadSlotMapTest,
	"RoadNet.Model.SlotMap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadSlotMapTest::RunTest(const FString& Parameters)
{
	TArray<FTestItem> Items;
	TArray<int32> FreeList;

	FTestItem A; A.Payload = 10;
	FTestItem B; B.Payload = 20;

	const FRoadNodeId HandleA = RoadSlot::Add<FRoadNodeId>(Items, FreeList, MoveTemp(A));
	const FRoadNodeId HandleB = RoadSlot::Add<FRoadNodeId>(Items, FreeList, MoveTemp(B));

	TestTrue(TEXT("A valid after add"), RoadSlot::IsValid<FRoadNodeId>(Items, HandleA));
	TestEqual(TEXT("A payload"), RoadSlot::Get<FRoadNodeId>(Items, HandleA)->Payload, 10);
	TestEqual(TEXT("B payload"), RoadSlot::Get<FRoadNodeId>(Items, HandleB)->Payload, 20);

	TestTrue(TEXT("remove A"), RoadSlot::Remove<FRoadNodeId>(Items, FreeList, HandleA));
	TestFalse(TEXT("A invalid after remove"), RoadSlot::IsValid<FRoadNodeId>(Items, HandleA));
	TestNull(TEXT("A get returns null"), RoadSlot::Get<FRoadNodeId>(Items, HandleA));
	TestTrue(TEXT("B still valid"), RoadSlot::IsValid<FRoadNodeId>(Items, HandleB));

	TestFalse(TEXT("double remove fails"), RoadSlot::Remove<FRoadNodeId>(Items, FreeList, HandleA));

	// Recycling the slot must NOT resurrect the stale handle.
	FTestItem C; C.Payload = 30;
	const FRoadNodeId HandleC = RoadSlot::Add<FRoadNodeId>(Items, FreeList, MoveTemp(C));
	TestEqual(TEXT("C reuses A's slot"), HandleC.Index, HandleA.Index);
	TestNotEqual(TEXT("C has a new generation"), HandleC.Generation, HandleA.Generation);
	TestFalse(TEXT("stale A still invalid"), RoadSlot::IsValid<FRoadNodeId>(Items, HandleA));
	TestEqual(TEXT("C payload"), RoadSlot::Get<FRoadNodeId>(Items, HandleC)->Payload, 30);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
