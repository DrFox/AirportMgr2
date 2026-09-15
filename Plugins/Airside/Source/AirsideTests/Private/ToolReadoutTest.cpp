#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Tool/ApronDrawTool.h"
#include "Tool/ToolReadout.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FToolReadoutCollectorTest,
	"Airside.Tool.ToolReadoutCollector",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FToolReadoutCollectorTest::RunTest(const FString& Parameters)
{
	FToolReadoutCollector Collector;

	// COMMITTABLE DEFAULTS TO FALSE, and that is the load-bearing default. Every tool but
	// one never calls Committable at all, and none of them should light a Build button by
	// saying nothing - the safe answer has to be the one you get for free.
	TestFalse(TEXT("a fresh readout is not committable"), Collector.Readout.bCommittable);

	Collector.Fact(TEXT("Bays"), TEXT("3"));
	Collector.Fact(TEXT("Rows"), TEXT("2"));
	Collector.Warning(TEXT("No room to grow"));
	Collector.Committable(true);

	TestEqual(TEXT("two facts"), Collector.Readout.Facts.Num(), 2);

	// IN THE ORDER EMITTED. The bar renders them in sequence, so a tool that emits bays
	// before rows means the player reads bays before rows - order is the tool's to decide
	// and the collector's to preserve.
	TestEqual(TEXT("and the first is the one emitted first"),
		Collector.Readout.Facts[0].Key, FString(TEXT("Bays")));
	TestEqual(TEXT("with its value"),
		Collector.Readout.Facts[0].Value, FString(TEXT("3")));
	TestEqual(TEXT("one warning"), Collector.Readout.Warnings.Num(), 1);
	TestTrue(TEXT("and it is committable now"), Collector.Readout.bCommittable);

	// RESET IS TOTAL, because the collector is refilled every frame and a fact left over
	// from last frame describes a gesture the player has already changed. Committable in
	// particular must go back to FALSE: a stale true is a Build button lit over nothing.
	Collector.Reset();
	TestEqual(TEXT("reset clears the facts"), Collector.Readout.Facts.Num(), 0);
	TestEqual(TEXT("and the warnings"), Collector.Readout.Warnings.Num(), 0);
	TestFalse(TEXT("and takes committable back to false"), Collector.Readout.bCommittable);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FToolsAreSilentByDefaultTest,
	"Airside.Tool.ToolsAreSilentByDefault",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FToolsAreSilentByDefaultTest::RunTest(const FString& Parameters)
{
	// THE WHOLE POINT OF DEFAULTING BOTH VIRTUALS. Eight tools and two test doubles
	// implement IBuildTool and none of them has a readout; a tool that said nothing must
	// leave the bar empty and the Build button dark rather than needing an edit to each.
	//
	// FApronDrawTool stands in for all of them - it is the sibling of the tool being
	// replaced, so if defaulting were wrong anywhere it would be wrong here.
	FApronDrawTool Silent;
	FToolReadoutCollector Collector;
	Collector.Committable(true);
	Collector.Reset();

	FToolContext Context;
	Silent.BuildReadout(Context, Collector);

	TestEqual(TEXT("a tool with no readout emits no facts"), Collector.Readout.Facts.Num(), 0);
	TestEqual(TEXT("and no warnings"), Collector.Readout.Warnings.Num(), 0);
	TestFalse(TEXT("and leaves the Build button dark"), Collector.Readout.bCommittable);

	// And OnCommit on a tool that does not implement it must be harmless rather than a
	// crash: the Build button exists whatever tool is selected.
	Silent.OnCommit(Context);
	TestTrue(TEXT("committing a tool that cannot commit is harmless"), true);

	return true;
}

#endif
