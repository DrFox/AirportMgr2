// THE ONE DERIVATION, MEASURED THROUGH ITS DOORS (#438). URoadSurfacePresenter::RebuildInternal
// used to own the routing graph's derivation outright, behind its render-component check, and
// TestGraph, the upgrade tool's what-if and three ops tests each re-typed the sequence - drifting
// (closed #101 and #311 were both a test-side copy that production never ran). These tests pin
// that there is one sequence and that every door reaches it.

#include "Misc/AutomationTest.h"
#include "Build/AirsideDerivation.h"
#include "Content/AirsideSettings.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/TaxiwayRestriction.h"
#include "Present/RoadNetworkActor.h"
#include "Present/RoadSurfacePresenter.h"
#include "Profiles/RoadProfile.h"
#include "Solve/IcaoCode.h"
#include "Testing/AirsideTestGraph.h"
#include "Testing/AirsideTestWorld.h"
#include "UObject/UnrealType.h"

namespace
{
	/**
	 * The first reflected field on which two slot arrays disagree, as "[slot].Field", or empty when
	 * every UPROPERTY of every slot - dead ones included - is identical. THROUGH REFLECTION rather
	 * than a hand-picked field list, so a field added to FGuidelineNode or FGuidelineEdge is compared
	 * the day it is added: a list typed here would be a second list that must agree with the struct.
	 * FProperty::Identical compares doubles with ==, so this is exact - no tolerance, which is the
	 * point (CLAUDE.md: the guideline graph samples ONCE; a second derivation is a second evaluator).
	 * Prefixed: unity build.
	 */
	template <typename TSlot>
	FString DerivationTestFirstDifference(const TArray<TSlot>& A, const TArray<TSlot>& B)
	{
		if (A.Num() != B.Num())
		{
			return FString::Printf(TEXT("slot count %d vs %d"), A.Num(), B.Num());
		}
		const UScriptStruct* Struct = TSlot::StaticStruct();
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				if (!It->Identical_InContainer(&A[Index], &B[Index], 0, PPF_None))
				{
					return FString::Printf(TEXT("[%d].%s"), Index, *It->GetName());
				}
			}
		}
		return FString();
	}

	/** Live guideline edges. */
	int32 DerivationTestLiveEdges(const URoadNetwork& Net)
	{
		int32 Live = 0;
		for (const FGuidelineEdge& Edge : Net.GetGuidelineEdges())
		{
			Live += Edge.bAlive ? 1 : 0;
		}
		return Live;
	}

	/** Live segments whose stored letter the restriction pass lowered. */
	int32 DerivationTestRestrictedCount(const URoadNetwork& Net)
	{
		int32 Restricted = 0;
		for (const FRoadSegment& Segment : Net.GetSegments())
		{
			Restricted += (Segment.bAlive && Segment.RestrictedLetter != TaxiwayRestriction::Unrestricted) ? 1 : 0;
		}
		return Restricted;
	}

	/**
	 * ONE FIELD EVERY PASS WRITES INTO: FTestAirport's two-exit shape (the crossbar bends, so the
	 * guideline builder lays turn paths the solve measured), two stands (so the anchor links join
	 * and split a taxiway), and a service road just off exit 1's taxiway, inside its strip (so the
	 * restriction pass lowers its letter and the builder writes a smaller MaxWingspan). A fixture
	 * missing any of these would let that pass drift between two doors unseen.
	 */
	void DerivationTestLayField(URoadNetwork& Net)
	{
		const FTestAirport Field = FTestAirport::Build(UAirsideSettings::ResolveDefaultAirframe(),
			{ .StandCount = 2, .ExitCount = 2, .bDerived = false }, &Net);

		// WEST of exit 1's taxiway (the stands are east of exit 2's), a gap of 5 m past its edge:
		// inside the pavement letter's strip whatever the letter is, so SOME lower letter wins.
		// AddStraightSegment, not the facade - the facade would refuse to lay it (strip stage 3),
		// and what an upgrade or an old map leaves behind is exactly this.
		const URoadProfile* Taxiway = TestProfiles::Taxiway();
		URoadProfile* Road = URoadProfile::MakeServiceRoadTransient();
		const double RoadX = Field.Exits[0].X - (Taxiway->GetMaxHalfWidth() + Road->GetMaxHalfWidth() + 500.0);
		Net.AddStraightSegment(Net.AddNode(FVector2D(RoadX, -12000.0)), Net.AddNode(FVector2D(RoadX, -8000.0)), Road);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDerivationTestGraphMatchesActorTest, "Airside.Build.Derivation.TestGraphMatchesTheActor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FDerivationTestGraphMatchesActorTest::RunTest(const FString&)
{
	// #438's PIN, the same-derivation twin of "the guideline graph samples ONCE": the fixture door
	// (TestGraph::Rebuild) and the production door (ARoadNetworkActor::RebuildMesh, through the
	// presenter) must derive the SAME graph from the same road - bit for bit. A pass that reaches
	// one door and not the other (the strip restriction and #324's &Solved each had to be added
	// twice) turns this red on the first field it changes.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor))
	{
		return false;
	}
	// URoadEditFacade::EnsureNetwork's own line - an empty network, with no click to put a node in
	// it that the twin would have to copy.
	Actor->Network = NewObject<URoadNetwork>(Actor);
	URoadNetwork* Twin = NewObject<URoadNetwork>(GetTransientPackage());
	DerivationTestLayField(*Actor->Network);
	DerivationTestLayField(*Twin);

	Actor->RebuildMesh();
	// THE DEFAULT THE ACTOR RESOLVED: TestGraph has no actor to resolve one (ARoadNetworkActor::
	// ResolveProfile is per-instance tuning), so a fixture that means production is handed the one
	// production used. Every segment here carries its own profile; this is so a fallback reader
	// cannot tell the two doors apart either.
	Twin->DefaultProfile = Actor->Network->DefaultProfile;
	TestGraph::Rebuild(*Twin);

	// CONTROLS, so a pass this fixture does not exercise cannot hide in an empty comparison.
	const URoadNetwork& Live = *Actor->Network;
	if (!TestTrue(TEXT("the actor derived a guideline graph (or there is nothing to compare)"), Live.GetGuidelineEdges().Num() > 0))
	{
		return false;
	}
	TestTrue(TEXT("the restriction pass lowered a taxiway through the actor's door (or the pin cannot see it)"),
		DerivationTestRestrictedCount(Live) > 0);
	TestEqual(TEXT("and the same taxiways through TestGraph's"), DerivationTestRestrictedCount(*Twin), DerivationTestRestrictedCount(Live));

	const FString Nodes = DerivationTestFirstDifference(Live.GetGuidelineNodes(), Twin->GetGuidelineNodes());
	TestTrue(FString::Printf(TEXT("guideline nodes are bitwise identical through both doors (first difference: %s)"), *Nodes),
		Nodes.IsEmpty());
	const FString Edges = DerivationTestFirstDifference(Live.GetGuidelineEdges(), Twin->GetGuidelineEdges());
	TestTrue(FString::Printf(TEXT("guideline edges are bitwise identical through both doors (first difference: %s)"), *Edges),
		Edges.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDerivationWithoutRenderComponentTest, "Airside.Present.Derivation.RunsWithoutARenderComponent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FDerivationWithoutRenderComponentTest::RunTest(const FString&)
{
	// THE LOAD PATH MUST NOT DEPEND ON A RENDER COMPONENT (#438). The presenter returned before
	// deriving anything when its Road mesh component was null, so the routing graph - what traffic,
	// the ops load and the planners read - existed only as a side effect of there being a mesh to
	// draw. A presenter never Initialize'd has every layer component null.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	DerivationTestLayField(*Net);
	URoadSurfacePresenter* Presenter = NewObject<URoadSurfacePresenter>(GetTransientPackage());
	URoadSurfacePresenter::FSurfaceSettings Settings;
	Settings.DesignVehicles = UAirsideSettings::ResolveRoadDesignVehicles();
	Presenter->Rebuild(*Net, Settings);

	TestTrue(TEXT("the guideline graph is derived with no mesh to draw it on"), Net->GetGuidelineEdges().Num() > 0);
	TestTrue(TEXT("and the restriction with it"), DerivationTestRestrictedCount(*Net) > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDerivationScopeTableTest, "Airside.Build.Derivation.ScopeTable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FDerivationScopeTableTest::RunTest(const FString&)
{
	// EACH SCOPE RUNS ITS ROW'S PASSES AND NO OTHERS (#438) - the seam every door goes through: the
	// presenter's Topology (Full) and drag frame (Surface), TestGraph::Derive (Graph), TestGraph::Link
	// (Links, the ops tests' hand-laid graphs) and the upgrade hover's what-if (Facts). Each pass is
	// seen by what it overwrites, planted before the call:
	//   solve       - URoadNetwork::DefaultProfile becomes the marker profile handed in;
	//   restriction - a runway's RestrictedLetter, poisoned with Code A, is rewritten (a runway has no taxiway
	//                 strip, so the pass writes Unrestricted; a letter, not an out-of-range byte, because the
	//                 pass's own log line names the letter it replaced);
	//   guidelines  - a sweepable (bDerived) edge between two authored nodes is swept;
	//   links       - live edges grow past what the graph alone has (a lead-in joins, a taxiway splits);
	//   stamp       - a road edit made after the last derive stops reading as "graph behind the road".
	using AirsideDerivation::EDeriveScope;
	struct FRow
	{
		EDeriveScope Scope;
		const TCHAR* Name;
		bool bSolve, bFacts, bGraph, bLinks, bStamp;
	};
	const FRow Rows[] = {
		{ EDeriveScope::Full,    TEXT("Full"),    true,  true,  true,  true,  true  },
		{ EDeriveScope::Surface, TEXT("Surface"), true,  false, false, false, false },
		{ EDeriveScope::Graph,   TEXT("Graph"),   true,  true,  true,  false, true  },
		{ EDeriveScope::Links,   TEXT("Links"),   false, false, false, true,  true  },
		{ EDeriveScope::Facts,   TEXT("Facts"),   false, true,  false, false, false },
	};
	static_assert(UE_ARRAY_COUNT(Rows) == static_cast<int32>(EDeriveScope::Count), "a row per scope - a new scope is tested the day it exists");

	constexpr uint8 Poison = static_cast<uint8>(EIcaoCode::A);
	const FRoadDesignVehicles Vehicles = UAirsideSettings::ResolveRoadDesignVehicles();
	for (const FRow& Row : Rows)
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FTestAirport Field = FTestAirport::Build(UAirsideSettings::ResolveDefaultAirframe(),
			{ .StandCount = 1, .bDerived = false }, Net);

		// A GRAPH TO START FROM, so the Links row has one to join, and every row's stamp starts set.
		AirsideDerivation::FDeriveInputs First;
		First.Scope = EDeriveScope::Graph;
		First.DefaultProfile = Net->DefaultProfile;
		First.DesignVehicles = &Vehicles;
		AirsideDerivation::Derive(*Net, First);
		const int32 GraphEdges = DerivationTestLiveEdges(*Net);

		URoadProfile* Marker = URoadProfile::MakeTransient(1234.0, 567.0);
		Net->WriteSegmentRestriction(Field.ThresholdSegment, Poison);
		const FGuidelineEdgeId Sweepable = TestGraph::Join(*Net, TestGraph::Node(*Net, 900000.0, 900000.0),
			TestGraph::Node(*Net, 910000.0, 900000.0));
		Net->AddNode(FVector2D(-900000.0, -900000.0));
		if (!TestTrue(FString::Printf(TEXT("%s: the edit leaves the graph behind the road (or the stamp column measures nothing)"), Row.Name),
			Net->AreGuidelinesBehindRoad()))
		{
			return false;
		}

		AirsideDerivation::FDeriveInputs Inputs;
		Inputs.Scope = Row.Scope;
		Inputs.DefaultProfile = Marker;
		Inputs.DesignVehicles = &Vehicles;
		AirsideDerivation::Derive(*Net, Inputs);

		const bool bGraphRan = Net->GetGuidelineEdge(Sweepable) == nullptr;
		TestEqual(FString::Printf(TEXT("%s: the solve (and the default profile before it) ran"), Row.Name),
			Net->DefaultProfile == Marker, Row.bSolve);
		TestEqual(FString::Printf(TEXT("%s: the restriction pass ran"), Row.Name),
			Net->GetSegment(Field.ThresholdSegment)->RestrictedLetter != Poison, Row.bFacts);
		TestEqual(FString::Printf(TEXT("%s: the guideline builder ran"), Row.Name), bGraphRan, Row.bGraph);
		TestEqual(FString::Printf(TEXT("%s: the anchor links ran"), Row.Name),
			DerivationTestLiveEdges(*Net) > GraphEdges + (bGraphRan ? 0 : 1), Row.bLinks);
		TestEqual(FString::Printf(TEXT("%s: the Derived stamp was set"), Row.Name), !Net->AreGuidelinesBehindRoad(), Row.bStamp);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDerivationRefusesWithoutVehiclesTest, "Airside.Build.Derivation.RefusesWithoutVehicles",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FDerivationRefusesWithoutVehiclesTest::RunTest(const FString&)
{
	// REFUSED, NOT GUESSED: a scope that solves or links needs the design vehicles the caller
	// resolved, and this layer has no honest default. Said as an Error, derives nothing.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	FTestAirport::Build(UAirsideSettings::ResolveDefaultAirframe(), { .StandCount = 1, .bDerived = false }, Net);
	AddExpectedError(TEXT("no design vehicles given - nothing derived"), EAutomationExpectedErrorFlags::Contains, 1);
	AirsideDerivation::FDeriveInputs Inputs;
	AirsideDerivation::Derive(*Net, Inputs);
	TestEqual(TEXT("nothing derived"), Net->GetGuidelineEdges().Num(), 0);
	return true;
}
