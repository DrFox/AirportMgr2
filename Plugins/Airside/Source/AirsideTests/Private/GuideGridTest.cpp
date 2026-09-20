#include "Misc/AutomationTest.h"
#include "Solve/GuideArbiter.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE GRID IS A LIST, AND THIS IS ITS FIRST CONSUMER. Sixteen of the thirty pairs are legal;
 * a hole is a statement, not an omission, so the count is asserted rather than the shape.
 * See the 2026-09-20 guide-grid design section 3 for each hole's reason.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideGridDeclaresSixteenCellsTest,
	"Airside.Solve.GuideGridDeclaresSixteenCells",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideGridDeclaresSixteenCellsTest::RunTest(const FString& Parameters)
{
	const int32 Relations = static_cast<int32>(SnapGuide::ERelation::MatchingGap) + 1;
	const int32 References = static_cast<int32>(SnapGuide::EReference::World) + 1;
	TestEqual(TEXT("five relations"), Relations, 5);
	TestEqual(TEXT("six references"), References, 6);

	int32 Legal = 0;
	for (int32 R = 0; R < Relations; ++R)
	{
		for (int32 F = 0; F < References; ++F)
		{
			if (SnapGuide::IsLegalCell(
				static_cast<SnapGuide::ERelation>(R), static_cast<SnapGuide::EReference>(F)))
			{
				++Legal;
			}
		}
	}
	TestEqual(TEXT("sixteen of the thirty pairs are legal"), Legal, 16);

	// THREE NAMED CELLS, not a re-listing of the table: a test that restated the whole grid
	// would be a second copy of it, and the two would drift. These three are the ones whose
	// reasoning the design argues hardest, so they are the ones worth pinning by name.
	TestTrue(TEXT("Extending is about the shape being drawn"),
		SnapGuide::IsLegalCell(SnapGuide::ERelation::Extending, SnapGuide::EReference::ThisGesture));
	TestFalse(TEXT("and Extending means nothing against a road"),
		SnapGuide::IsLegalCell(SnapGuide::ERelation::Extending, SnapGuide::EReference::Road));
	TestFalse(TEXT("a world axis has no position, so nothing can be in line with it"),
		SnapGuide::IsLegalCell(SnapGuide::ERelation::Collinear, SnapGuide::EReference::World));

	return true;
}

#endif
