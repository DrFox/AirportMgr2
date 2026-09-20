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

/**
 * THE TIEBREAK IS A PAIR, RELATION FIRST. Two candidates equally close must not be separated
 * by the order the network happened to be walked in - that changes with an unrelated edit.
 * A stand's pose losing to the nearest road is the ONE ranking this split changes, and
 * FAlignedGuideSource's own header already argued for it: "the weakest of the four network
 * sources", which its old rank of third-of-eight contradicted.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideGridBreaksTiesByRelationThenReferenceTest,
	"Airside.Solve.GuideGridBreaksTiesByRelationThenReference",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideGridBreaksTiesByRelationThenReferenceTest::RunTest(const FString& Parameters)
{
	SnapGuide::FCandidate Road;
	Road.Direction = FVector2D(1.0, 0.0);
	Road.Through = FVector2D::ZeroVector;
	Road.Fit = SnapGuide::EFit::Angular;
	Road.Relation = SnapGuide::ERelation::Parallel;
	Road.Reference = SnapGuide::EReference::Road;

	// THE SAME DIRECTION, so the two are exactly tied on error and only the rank can separate
	// them. Listed AFTER the stand, so a first-wins bug shows up as the stand winning.
	SnapGuide::FCandidate Stand = Road;
	Stand.Reference = SnapGuide::EReference::Stand;

	const SnapGuide::FCandidate Candidates[] = { Stand, Road };
	const SnapGuide::FResult Result = SnapGuide::Arbitrate(
		Candidates, FVector2D::ZeroVector, FVector2D(1000.0, 0.0), SnapGuide::FResult());

	if (!TestTrue(TEXT("a guide holds"), Result.bActive)) { return false; }
	TestEqual(TEXT("the nearest road beats a stand's pose on a tie"),
		static_cast<int32>(Result.Winners[0].Reference),
		static_cast<int32>(SnapGuide::EReference::Road));

	// AND RELATION OUTRANKS REFERENCE. Extending x ThisGesture is a worse reference rank than
	// Parallel x Road, and must still win - otherwise the pair is being compared the wrong way
	// round, which a reference-only test could not tell apart.
	SnapGuide::FCandidate Extending = Road;
	Extending.Relation = SnapGuide::ERelation::Extending;
	Extending.Reference = SnapGuide::EReference::ThisGesture;

	const SnapGuide::FCandidate Both[] = { Road, Extending };
	const SnapGuide::FResult Second = SnapGuide::Arbitrate(
		Both, FVector2D::ZeroVector, FVector2D(1000.0, 0.0), SnapGuide::FResult());

	if (!TestTrue(TEXT("a guide holds"), Second.bActive)) { return false; }
	TestEqual(TEXT("what you are extending beats the road you are beside"),
		static_cast<int32>(Second.Winners[0].Relation),
		static_cast<int32>(SnapGuide::ERelation::Extending));

	return true;
}

#endif
