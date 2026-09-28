#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Profiles/RoadProfile.h"
#include "Tool/RoadDrawTool.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/RoadEditTarget.h"
#include "Build/BuildCost.h"
#include "Model/BuildPurse.h"
#include "Present/RoadEditFacade.h"
#include "Present/RoadEditHistory.h"
#include "Solve/IcaoCode.h"
#include "Model/TaxiwayRestriction.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the UNITY build - these test files share one translation unit.

	/**
	 * A target that records the width index the tool hands it, and nothing else.
	 *
	 * THE SAME SHAPE AS FFakeRunwayTarget and for the same stated reason: the tool must be
	 * testable without UAirsideContent, because the whole point of the seam (#78) is that a
	 * tool does not know the content set. Grep this file for UAirsideContent and find
	 * nothing - if that ever stops being true, the tool has grown a content dependency.
	 * Every virtual this does not override is FNullEditTarget's inert default (#189),
	 * including GetRunwayProfileCount/ResolveRunwayProfile - this fake is the TAXIWAY tool's -
	 * and ResolveProfileFor, since a fake resolves nothing composite (the real rule lives on
	 * ARoadNetworkActor, pinned by Airside.Present.ProfileResolutionIsOneRule).
	 */
	struct FFakeWidthTarget : FNullEditTarget
	{
		TArray<URoadProfile*> TaxiwayProfiles;

		/** What the tool asked for, last time it asked. INDEX_NONE means "the default". */
		mutable int32 LastConnectWidth = -2;
		mutable int32 LastGhostWidth = -2;
		int32 Connects = 0;

		virtual int32 PlaceNode(FVector2D) override { return Connects; }
		virtual bool ConnectNodes(int32, int32, ERoadKind, int32 WidthIndex, EPavement) override
		{
			LastConnectWidth = WidthIndex;
			++Connects;
			return true;
		}
		using IRoadEditTarget::ConnectNodes;
		virtual void UpdateGhost(int32, const FRoadSnapResult&, bool, ERoadKind, int32 WidthIndex) override
		{
			LastGhostWidth = WidthIndex;
		}
		using IRoadEditTarget::UpdateGhost;

		virtual int32 GetWidthCount(ERoadKind Kind) const override { return Kind == ERoadKind::Taxiway ? TaxiwayProfiles.Num() : 0; }
		virtual URoadProfile* ResolveWidthProfile(ERoadKind Kind, int32 Index) const override
		{
			if (Kind != ERoadKind::Taxiway || TaxiwayProfiles.Num() == 0)
			{
				return nullptr;
			}
			// Clamped, mirroring ARoadNetworkActor's own contract - a fake that did not
			// clamp would let a test pass against behaviour a real target refuses.
			return TaxiwayProfiles[FMath::Clamp(Index, 0, TaxiwayProfiles.Num() - 1)];
		}
	};

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTaxiwayWidthTest,
	"Airside.Tool.TaxiwayWidth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiwayWidthTest::RunTest(const FString& Parameters)
{
	// A REAL WORLD for the drawing, because the click path runs through the facade and the
	// actor - a fake thin enough to record a call is not thin enough to make one happen,
	// which is how the first version of this test passed its cycling assertions and proved
	// nothing about what got laid (#104's lesson again).
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	const int32 Count = Actor->GetWidthCount(ERoadKind::Taxiway);
	if (!TestTrue(TEXT("the content set declares standard taxiway widths - run "
		"Tools/Python/build_road_profiles.py if this fails"), Count > 1))
	{
		return false;
	}

	// 1. A FRESH TOOL LAYS THE NARROWEST STANDARD WIDTH (ruled 2026-09-28). It used to lay the
	//    actor's own instance tuning (ARoadNetworkActor::ResolveProfile) until a width was
	//    asked for; the ruling puts every tool on its cheapest start instead. Roads already laid
	//    are untouched either way - a segment keeps the profile it was laid with.
	{
		FRoadDrawTool Tool(ERoadKind::Taxiway);
		TestEqual(TEXT("a fresh tool starts on the narrowest width"), Tool.GetWidthIndex(), 0);

		Tool.OnClick(TestTool::ContextAt(*Actor, FVector2D(0.0, 0.0)));
		Tool.OnClick(TestTool::ContextAt(*Actor, FVector2D(4000.0, 0.0)));

		const TArray<FRoadSegment>& Segments = Actor->Network->GetSegments();
		if (TestEqual(TEXT("one segment was laid"), Segments.Num(), 1))
		{
			TestEqual(TEXT("and it carries the narrowest standard profile"),
				Segments[0].Profile.Get(), Actor->ResolveWidthProfile(ERoadKind::Taxiway, 0));
		}
	}

	// 2. RESELECTING CYCLES, wrapping at the end. Key-again is the runway tool's gesture
	//    (FRunwayTool::OnReselect) and this is deliberately the same one. IT STEPS FROM THE LIT
	//    OPTION (2026-09-26, the variant row): the level's default may light a preset, and a
	//    first press that jumped back to the narrowest would read as the row and the key
	//    disagreeing. So the expectation is derived from what is lit, not assumed to be 0.
	{
		FRoadDrawTool Tool(ERoadKind::Taxiway);
		FToolContext Context = TestTool::ContextAt(*Actor, FVector2D::ZeroVector);

		TArray<FToolVariantAxis> Axes;
		Tool.GetVariantAxes(Context, Axes);
		const int32 Lit = Axes.Num() > 0 ? Axes[0].Current : INDEX_NONE;
		const int32 First = Lit == INDEX_NONE ? 0 : (Lit + 1) % Count;

		Tool.OnReselect(Context);
		TestEqual(TEXT("the first press steps on from what is lit"), Tool.GetWidthIndex(), First);
		for (int32 Press = 1; Press < Count; ++Press)
		{
			Tool.OnReselect(Context);
		}
		TestEqual(TEXT("and a full lap of the list comes back round to one before it"),
			Tool.GetWidthIndex(), (First + Count - 1) % Count);
		Tool.OnReselect(Context);
		TestEqual(TEXT("then wraps to where it started"), Tool.GetWidthIndex(), First);
	}

	// 3. THE CHOSEN WIDTH REACHES THE ROAD. Without this the cycle is a counter that logs a
	//    number and lays the same taxiway - the "a list nothing consumes" failure this
	//    codebase has shipped three times.
	{
		FRoadDrawTool Tool(ERoadKind::Taxiway);
		// A PICK, not a press: which index a press lands on now depends on what the level's
		// default lights (block 2), and this block needs the narrowest specifically.
		Tool.SelectVariant(TestTool::ContextAt(*Actor, FVector2D::ZeroVector), 0, 0);

		const URoadProfile* Narrowest = Actor->ResolveWidthProfile(ERoadKind::Taxiway, 0);
		if (!TestNotNull(TEXT("the narrowest profile loads"), Narrowest)) { return false; }

		const int32 Before = Actor->Network->GetSegments().Num();
		Tool.OnClick(TestTool::ContextAt(*Actor, FVector2D(0.0, 8000.0)));
		Tool.OnClick(TestTool::ContextAt(*Actor, FVector2D(4000.0, 8000.0)));

		const TArray<FRoadSegment>& Segments = Actor->Network->GetSegments();
		if (TestEqual(TEXT("a second road was laid"), Segments.Num(), Before + 1))
		{
			// Compared by WIDTH rather than by pointer: the figure is what the player sees
			// and what the junction geometry reads, and it says which profile landed
			// without the test caring which asset object backs it.
			const URoadProfile* Laid = Segments.Last().Profile.Get();
			if (TestNotNull(TEXT("the second road carries a profile"), Laid))
			{
				TestEqual(TEXT("at the width that was cycled to"),
					Laid->GetTotalWidth(), Narrowest->GetTotalWidth(), 1.0);
				TestNotEqual(TEXT("which is not the level's default, or this proves nothing"),
					Narrowest->GetTotalWidth(), Actor->ResolveProfile()->GetTotalWidth());
			}
		}
	}

	// 4. A SERVICE ROAD CYCLES ITS OWN TIERS, NEVER THE TAXIWAYS'. Until 2026-09-23 a road
	//    ignored the cycle outright, because the only list was the taxiways' and key 9 would
	//    have laid a road at a taxiway's width, paving wide for vans. The seam keys the list by
	//    kind now (Airside.Tool.ServiceRoadWidth covers the tiers); what must still hold is the
	//    danger this block was written for.
	{
		FRoadDrawTool Road(ERoadKind::ServiceRoad);
		Road.OnReselect(TestTool::ContextAt(*Actor, FVector2D::ZeroVector));
		const URoadProfile* Chosen = Actor->ResolveProfileFor(ERoadKind::ServiceRoad, Road.GetWidthIndex());
		TestTrue(TEXT("a road's first width is a road - two lanes, never a taxiway's one aircraft line"),
			Chosen != nullptr && Chosen->Guidelines.Num() == 2);
		TestTrue(TEXT("and narrower than the narrowest taxiway"),
			Chosen != nullptr && Chosen->GetTotalWidth() < Actor->ResolveWidthProfile(ERoadKind::Taxiway, 0)->GetTotalWidth());
	}

	// 5. AN EMPTY LIST REFUSES rather than choosing nothing quietly - a project that has
	//    not run build_road_profiles.py is a real state, and the one FRunwayTool's own
	//    empty-list branch exists for. A fake target is the only way to have no content.
	{
		FFakeWidthTarget Empty;
		FRoadDrawTool Tool(ERoadKind::Taxiway);
		FToolContext Context;
		Context.Target = &Empty;
		Tool.OnReselect(Context);
		TestEqual(TEXT("with nothing to cycle, the tool stays where it started"),
			Tool.GetWidthIndex(), 0);
	}

	return true;
}

namespace
{
	/** Records what the facade charged. Prefixed against the unity build (BuildPurseTest has
	 *  its own recorder). A fake, not ULedger, for that file's reason: the SEAM is under test. */
	class FUpgradeRecordingPurse : public IBuildPurse
	{
	public:
		double Funds = 1.0e9;
		TArray<double> Charges;
		virtual bool CanAfford(const FBuildQuote& Quote) const override { return Quote.BaseAmount() <= Funds; }
		virtual int32 Charge(const FBuildQuote& Quote) override { Funds -= Quote.BaseAmount(); Charges.Add(Quote.BaseAmount()); return Charges.Num(); }
		virtual void Reverse(int32) override {}
		virtual void Credit(const FBuildQuote&) override {}
		virtual FText Describe(const FBuildQuote& Quote) const override { return FText::AsNumber(Quote.BaseAmount()); }
	};

	/** The standard width index whose letter is Letter, INDEX_NONE if the content set has none -
	 *  looked up, never typed, so a re-authored width list moves the test with it. */
	int32 UpgradeWidthIndexFor(const ARoadNetworkActor& Actor, EIcaoCode Letter)
	{
		for (int32 Index = 0; Index < Actor.GetWidthCount(ERoadKind::Taxiway); ++Index)
		{
			const URoadProfile* Profile = Actor.ResolveWidthProfile(ERoadKind::Taxiway, Index);
			if (Profile != nullptr && IcaoCode::TaxiwayLetterForWidth(Profile->GetTotalWidth()) == Letter)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUpgradeSegmentTest,
	"Airside.Present.UpgradeSegment",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUpgradeSegmentTest::RunTest(const FString& Parameters)
{
	// THE UPGRADE SEAM (strip stage 6): an existing segment's width and surface change IN PLACE,
	// as one undo step, priced by the difference - on the facade, not a tool, so the tool's
	// Upgrade mode and anything later (a stand re-pave) refuse and charge one way.
	FUpgradeRecordingPurse Purse; // before the world - the facade holds a raw pointer to it
	FAirsideTestWorld World;
	if (!TestNotNull(TEXT("a world"), World.Actor)) { return false; }
	ARoadNetworkActor* Actor = World.Actor;
	Actor->GetEditFacade()->SetPurse(&Purse);
	ON_SCOPE_EXIT { Actor->GetEditFacade()->SetPurse(nullptr); };

	const int32 C = UpgradeWidthIndexFor(*Actor, EIcaoCode::C);
	const int32 F = UpgradeWidthIndexFor(*Actor, EIcaoCode::F);
	if (!TestTrue(TEXT("the content set has a C and an F taxiway width"), C != INDEX_NONE && F != INDEX_NONE)) { return false; }
	const URoadProfile* ProfileC = Actor->ResolveWidthProfile(ERoadKind::Taxiway, C);
	const URoadProfile* ProfileF = Actor->ResolveWidthProfile(ERoadKind::Taxiway, F);

	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(20000.0, 0.0));
	if (!TestTrue(TEXT("a Code C tarmac taxiway laid"), Actor->ConnectNodes(A, B, ERoadKind::Taxiway, C, EPavement::Tarmac))) { return false; }
	const int32 Seg = Actor->Network->GetSegments().Num() - 1;
	auto Piece = [Actor, Seg]() -> const FRoadSegment& { return Actor->Network->GetSegments()[Seg]; };
	const int32 ChargesAfterLay = Purse.Charges.Num();

	// 1. SAME WIDTH, SAME SURFACE: true (it IS so) and no edit - an undo step that changes nothing
	//    is a Ctrl+Z the player presses twice (SetRunwayFacts' rule).
	{
		const int32 Depth = Actor->History->UndoDepth();
		TestTrue(TEXT("already so is not a refusal"), Actor->UpgradeSegment(Seg, ERoadKind::Taxiway, C, EPavement::Tarmac));
		TestEqual(TEXT("and opens no edit"), Actor->History->UndoDepth(), Depth);
		TestEqual(TEXT("and charges nothing"), Purse.Charges.Num(), ChargesAfterLay);
	}

	// 2. REFUSALS, before the snapshot: nothing changes, nothing is charged, WhyUpgradeRefused says why.
	{
		const int32 Depth = Actor->History->UndoDepth();
		TestFalse(TEXT("a surface the profile does not offer (concrete on a taxiway) refuses"),
			Actor->UpgradeSegment(Seg, ERoadKind::Taxiway, F, EPavement::Concrete));
		TestFalse(TEXT("and says why"), Actor->WhyUpgradeRefused(Seg, ERoadKind::Taxiway, F, EPavement::Concrete).IsEmpty());
		TestFalse(TEXT("a service-road width on a taxiway refuses - the kind never changes in place"),
			Actor->UpgradeSegment(Seg, ERoadKind::ServiceRoad, 0, EPavement::Tarmac));
		TestFalse(TEXT("a dead slot refuses"), Actor->UpgradeSegment(Seg + 100, ERoadKind::Taxiway, F, EPavement::Tarmac));
		TestEqual(TEXT("no refusal opened an edit"), Actor->History->UndoDepth(), Depth);
		TestTrue(TEXT("the profile is untouched"), Piece().Profile.Get() == ProfileC);
	}

	// 3. UNAFFORDABLE refuses before the scope.
	{
		Purse.Funds = 0.0;
		const int32 Depth = Actor->History->UndoDepth();
		TestFalse(TEXT("an upgrade the player cannot pay for refuses"), Actor->UpgradeSegment(Seg, ERoadKind::Taxiway, F, EPavement::Tarmac));
		TestEqual(TEXT("with no edit"), Actor->History->UndoDepth(), Depth);
		TestTrue(TEXT("and no width change"), Piece().Profile.Get() == ProfileC);
		Purse.Funds = 1.0e9;
	}

	// 4. WIDTH AND SURFACE TOGETHER: one undo step, priced as the new ground less the old.
	{
		const double Length = BuildCost::SegmentLengthUu(*Actor->Network, Piece());
		const double Expected = BuildCost::ForSegment(*ProfileF, Length, EPavement::Grass).BaseAmount()
			- BuildCost::ForSegment(*ProfileC, Length, EPavement::Tarmac).BaseAmount();
		const int32 Depth = Actor->History->UndoDepth();
		if (!TestTrue(TEXT("C tarmac -> F grass upgrades"), Actor->UpgradeSegment(Seg, ERoadKind::Taxiway, F, EPavement::Grass)))
		{
			return false;   // the undo below would otherwise step back past the lay itself
		}
		TestTrue(TEXT("the segment carries F's profile"), Piece().Profile.Get() == ProfileF);
		TestEqual(TEXT("and grass"), static_cast<int32>(Piece().Surface), static_cast<int32>(EPavement::Grass));
		TestEqual(TEXT("as ONE undo step"), Actor->History->UndoDepth(), Depth + 1);
		if (Expected > 0.0 && TestEqual(TEXT("charged once"), Purse.Charges.Num(), ChargesAfterLay + 1))
		{
			TestEqual(TEXT("the difference between the new ground and the old"), Purse.Charges.Last(), Expected, 0.01);
		}

		if (!TestTrue(TEXT("undo"), Actor->GetEditFacade()->Undo()) || !Actor->Network->GetSegments().IsValidIndex(Seg)) { return false; }
		TestEqual(TEXT("one undo restores the width"), Actor->Network->GetSegments()[Seg].Profile->GetTotalWidth(), ProfileC->GetTotalWidth(), 0.5);
		TestEqual(TEXT("and the surface"), static_cast<int32>(Actor->Network->GetSegments()[Seg].Surface), static_cast<int32>(EPavement::Tarmac));
	}

	// 5. A DOWNGRADE works and refunds nothing (plan ruling 1: no refund on a downgrade).
	{
		TestTrue(TEXT("up to F again"), Actor->UpgradeSegment(Seg, ERoadKind::Taxiway, F, EPavement::Tarmac));
		const int32 Charged = Purse.Charges.Num();
		TestTrue(TEXT("and back down to C"), Actor->UpgradeSegment(Seg, ERoadKind::Taxiway, C, EPavement::Tarmac));
		TestEqual(TEXT("the downgrade lands"), Piece().Profile->GetTotalWidth(), ProfileC->GetTotalWidth(), 0.5);
		TestEqual(TEXT("and charges nothing"), Purse.Charges.Num(), Charged);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUpgradeSegmentRefusesIntoNeighbourTest,
	"Airside.Present.UpgradeSegmentRefusesIntoNeighbourStrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUpgradeSegmentRefusesIntoNeighbourTest::RunTest(const FString& Parameters)
{
	// WIDENING INTO A NEIGHBOUR IS LAYING PAVEMENT: the widened edge must stay out of another
	// taxiway's strip (stage 3's JudgeSegment, asked with the NEW shape). What the grown STRIP
	// swallows is not refused - that restricts (Task 3).
	FAirsideTestWorld World;
	if (!TestNotNull(TEXT("a world"), World.Actor)) { return false; }
	ARoadNetworkActor* Actor = World.Actor;
	const int32 C = UpgradeWidthIndexFor(*Actor, EIcaoCode::C);
	const int32 F = UpgradeWidthIndexFor(*Actor, EIcaoCode::F);
	if (!TestTrue(TEXT("C and F widths"), C != INDEX_NONE && F != INDEX_NONE)) { return false; }
	const double HalfC = Actor->ResolveWidthProfile(ERoadKind::Taxiway, C)->GetMaxHalfWidth();
	const double HalfF = Actor->ResolveWidthProfile(ERoadKind::Taxiway, F)->GetMaxHalfWidth();
	const double NeighbourReach = HalfC + IcaoCode::TaxiwayStripForWidth(2.0 * HalfC);
	// C's edge clear of the neighbour's strip, F's edge inside it.
	const double Gap = NeighbourReach + 0.5 * (HalfC + HalfF);

	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(20000.0, 0.0));
	TestTrue(TEXT("the taxiway to widen"), Actor->ConnectNodes(A, B, ERoadKind::Taxiway, C, EPavement::Tarmac));
	const int32 Seg = Actor->Network->GetSegments().Num() - 1;
	const int32 P = Actor->PlaceNode(FVector2D(0.0, Gap));
	const int32 Q = Actor->PlaceNode(FVector2D(20000.0, Gap));
	TestTrue(TEXT("a parallel C taxiway, clear of it at C"), Actor->ConnectNodes(P, Q, ERoadKind::Taxiway, C, EPavement::Tarmac));

	TestFalse(TEXT("widening to F puts pavement in the neighbour's strip: refused"),
		Actor->UpgradeSegment(Seg, ERoadKind::Taxiway, F, EPavement::Tarmac));
	TestTrue(TEXT("and the refusal names the strip"),
		Actor->WhyUpgradeRefused(Seg, ERoadKind::Taxiway, F, EPavement::Tarmac).Contains(TEXT("clearance strip")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUpgradeSegmentRestrictsTest,
	"Airside.Present.UpgradeRestrictsAndUndoLifts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUpgradeSegmentRestrictsTest::RunTest(const FString& Parameters)
{
	// THE SEAM, AT THE COMPOSITION (CLAUDE.md's refactor contract): the restriction pass runs in
	// the actor's own Topology rebuild, before the guideline builder. Unwired, the upgraded
	// taxiway would keep reading unrestricted. Review Focus 1 and 4: undo and a downgrade lift it.
	FAirsideTestWorld World;
	if (!TestNotNull(TEXT("a world"), World.Actor)) { return false; }
	ARoadNetworkActor* Actor = World.Actor;
	const int32 C = UpgradeWidthIndexFor(*Actor, EIcaoCode::C);
	const int32 F = UpgradeWidthIndexFor(*Actor, EIcaoCode::F);
	if (!TestTrue(TEXT("C and F widths"), C != INDEX_NONE && F != INDEX_NONE)) { return false; }
	const double WidthF = Actor->ResolveWidthProfile(ERoadKind::Taxiway, F)->GetTotalWidth();
	const URoadProfile* Road = Actor->ResolveProfileFor(ERoadKind::ServiceRoad, INDEX_NONE);
	if (!TestNotNull(TEXT("a service road profile"), Road)) { return false; }
	// The road's near edge between E's reach and F's at the F pavement - it restricts F to E.
	const double ReachE = 0.5 * WidthF + IcaoCode::TaxiwayStripFor(EIcaoCode::E, WidthF);
	const double ReachF = 0.5 * WidthF + IcaoCode::TaxiwayStripFor(EIcaoCode::F, WidthF);
	const double RoadY = 0.5 * (ReachE + ReachF) + Road->GetMaxHalfWidth();

	const int32 A = Actor->PlaceNode(FVector2D(-20000.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(20000.0, 0.0));
	TestTrue(TEXT("a C taxiway"), Actor->ConnectNodes(A, B, ERoadKind::Taxiway, C, EPavement::Tarmac));
	const int32 Seg = Actor->Network->GetSegments().Num() - 1;
	const int32 P = Actor->PlaceNode(FVector2D(-3000.0, RoadY));
	const int32 Q = Actor->PlaceNode(FVector2D(3000.0, RoadY));
	TestTrue(TEXT("a service road clear of its C strip"), Actor->ConnectNodes(P, Q, ERoadKind::ServiceRoad, INDEX_NONE, EPavement::Tarmac));
	auto Stored = [Actor, Seg]() { return static_cast<int32>(Actor->Network->GetSegments()[Seg].RestrictedLetter); };
	TestEqual(TEXT("at C nothing is restricted"), Stored(), static_cast<int32>(TaxiwayRestriction::Unrestricted));

	if (!TestTrue(TEXT("the upgrade to F lands - the strip swallowing the road is not a refusal"),
		Actor->UpgradeSegment(Seg, ERoadKind::Taxiway, F, EPavement::Tarmac)))
	{
		return false;
	}
	TestEqual(TEXT("the rebuild restricted the F taxiway to E, by the road"), Stored(), static_cast<int32>(EIcaoCode::E));

	TestTrue(TEXT("undo"), Actor->GetEditFacade()->Undo());
	TestEqual(TEXT("undo lifts the restriction with the width"), Stored(), static_cast<int32>(TaxiwayRestriction::Unrestricted));
	TestTrue(TEXT("redo"), Actor->GetEditFacade()->Redo());
	TestEqual(TEXT("redo restricts again"), Stored(), static_cast<int32>(EIcaoCode::E));
	TestTrue(TEXT("a downgrade back to C"), Actor->UpgradeSegment(Seg, ERoadKind::Taxiway, C, EPavement::Tarmac));
	TestEqual(TEXT("lifts it too"), Stored(), static_cast<int32>(TaxiwayRestriction::Unrestricted));
	return true;
}

#endif
