#include "Build/DepotKit.h"
#include "AirsideLog.h"
#include "Content/AirsideContent.h"
#include "CoreMinimal.h"
#include "Entities/EntityDefinition.h"
#include "Entities/PlotModuleKit.h"
#include "Logging/LogVerbosity.h"
#include "Misc/AutomationTest.h"
#include "Model/DepotCapability.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * Catches every LogAirside line at Warning verbosity - not FLogLineSpy (AirsideTestWorld.h),
	 * which only ever matches ELogVerbosity::Log and would silently see nothing here.
	 * CanBeUsedOnMultipleThreads override for the same reason FLogLineSpy's own comment gives
	 * (issue #216): the dedicated log thread outlives a spy declared on the test's stack unless
	 * this says otherwise.
	 */
	struct FWarningLogSpy : public FOutputDevice
	{
		TArray<FString> Lines;

		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& InCategory) override
		{
			if (InCategory == FName(TEXT("LogAirside")) && Verbosity == ELogVerbosity::Warning)
			{
				Lines.Add(FString(V));
			}
		}
	};
}

/**
 * A kit's figures win; no kit falls back to the grey-box table.
 *
 * THE FALLBACK IS THE POINT, not a convenience. Every other change in this slice - the
 * reservation solver, the presenter, the readout - lands and is testable before a single
 * mesh exists, and that is only true while a missing kit keeps working. A resolver that
 * returned a zero footprint instead would put every module on top of every other one, which
 * reads as a solver bug rather than as missing content.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDepotKitFallsBackTest,
	"Airside.Build.DepotKitFallsBackWhenUnauthored",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDepotKitFallsBackTest::RunTest(const FString& Parameters)
{
	// No content at all: the grey-box figures DepotKit.cpp has carried since the yard solver
	// was written.
	const PlotYard::FFootprint Bare = DepotFootprint(EDepotModule::Shed, nullptr);
	TestEqual(TEXT("unauthored shed keeps its grey-box length"), Bare.LengthUu, 800.0);
	TestEqual(TEXT("unauthored shed keeps its grey-box width"), Bare.WidthUu, 400.0);
	TestTrue(TEXT("unauthored shed still stands against the back fence"),
		Bare.bAgainstTheBackFence);

	// An authored kit overrides all three.
	UAirsideContent* Content = NewObject<UAirsideContent>();
	UPlotModuleKit* Kit = NewObject<UPlotModuleKit>();
	Kit->Footprint = FVector2D(1100.0, 500.0);
	Kit->bAgainstTheBackFence = false;
	Content->DepotKits.Add(EDepotModule::Shed, Kit);

	const PlotYard::FFootprint Authored = DepotFootprint(EDepotModule::Shed, Content);
	TestEqual(TEXT("an authored shed uses the kit's length"), Authored.LengthUu, 1100.0);
	TestEqual(TEXT("an authored shed uses the kit's width"), Authored.WidthUu, 500.0);
	TestFalse(TEXT("an authored shed uses the kit's back-fence flag"),
		Authored.bAgainstTheBackFence);

	// A kit for one module does not silently answer for another - the map is keyed, and a
	// lookup that fell through to "the first kit" would dress every module as a shed.
	const PlotYard::FFootprint Tank = DepotFootprint(EDepotModule::Tank, Content);
	TestEqual(TEXT("the tank still falls back"), Tank.LengthUu, 500.0);
	TestEqual(TEXT("the tank still falls back on width too"), Tank.WidthUu, 500.0);

	return true;
}

/**
 * A kit's apron reaches the solver, and is NOT folded into the footprint.
 *
 * TWO RECTANGLES, NOT ONE. The footprint is the object - what the mesh is and what the
 * presenter draws - and the apron is the working room in front of it. Inflating the shed to
 * 4 x 10 m to buy its apron would draw a ten-metre shed today and disagree with a six-metre
 * mesh tomorrow, which is exactly what the footprint-versus-bounds test exists to catch.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDepotKitCarriesItsApronTest,
	"Airside.Build.DepotKitCarriesItsApron",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDepotKitCarriesItsApronTest::RunTest(const FString& Parameters)
{
	UAirsideContent* Content = NewObject<UAirsideContent>();
	UPlotModuleKit* Kit = NewObject<UPlotModuleKit>();
	Kit->Footprint = FVector2D(600.0, 400.0);
	Kit->ApronUu = FVector2D(400.0, 0.0);
	Content->DepotKits.Add(EDepotModule::Shed, Kit);

	const TArray<PlotYard::FKitSpec> Specs = DepotKitSpecs(Content);
	if (!TestTrue(TEXT("a spec per module"),
		Specs.Num() > static_cast<int32>(EDepotModule::Shed)))
	{
		return false;
	}

	const PlotYard::FKitSpec& Shed = Specs[static_cast<int32>(EDepotModule::Shed)];

	// THE FOOTPRINT IS UNTOUCHED BY THE APRON. If these ever merge, the presenter draws the
	// apron as building.
	TestEqual(TEXT("the footprint is the object"), Shed.Footprint.LengthUu, 600.0);
	TestEqual(TEXT("and its width too"), Shed.Footprint.WidthUu, 400.0);

	TestEqual(TEXT("the apron reaches the spec"), Shed.ApronUu.X, 400.0);
	TestEqual(TEXT("and its lateral half"), Shed.ApronUu.Y, 0.0);

	// AN UNAUTHORED KIT HAS NO APRON rather than a default one: a module that needs clear
	// ground says so, and one that does not keeps the clearance every module already gets.
	const TArray<PlotYard::FKitSpec> Bare = DepotKitSpecs(nullptr);
	TestEqual(TEXT("an unauthored tank has no apron"),
		Bare[static_cast<int32>(EDepotModule::Tank)].ApronUu.X, 0.0);

	return true;
}

/**
 * Every real EDepotModule gets a spec - walked to the sentinel, not to a hardcoded last value.
 *
 * "Pump is the last value; adding a module after it extends this loop with no edit" was false
 * the moment a module was actually added after Pump: DepotKitSpecs walked to
 * static_cast<int32>(EDepotModule::Pump) by name, so a new member got no spec and its owned
 * instances vanished from Specs uncounted (issue #193). EDepotModule::Count is the sentinel
 * that moves itself whenever a real member is inserted before it, which is what this pins:
 * Specs.Num() must equal Count, not a number copied from today's enum.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDepotKitSpecsCoverEveryModuleTest,
	"Airside.Build.DepotKitSpecsCoverEveryModule",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDepotKitSpecsCoverEveryModuleTest::RunTest(const FString& Parameters)
{
	const TArray<PlotYard::FKitSpec> Specs = DepotKitSpecs(nullptr);

	// THE SENTINEL ITSELF, not Pump: a loop that stopped one short of Count would still pass
	// a count taken from Pump, which is exactly the bug this test exists to catch.
	TestEqual(TEXT("one spec per real module, sized to the sentinel"),
		Specs.Num(), static_cast<int32>(EDepotModule::Count));

	// AND IN ENUM ORDER: the presenter and the purchase service's ceiling hook index Specs by
	// static_cast<int32>(Module), so spec i must be module i's footprint - a reordered or
	// filtered walk would light a tank where a shed was bought.
	for (int32 Raw = 0; Raw < Specs.Num(); ++Raw)
	{
		const PlotYard::FFootprint Expected = DepotFootprint(static_cast<EDepotModule>(Raw), nullptr);
		TestTrue(FString::Printf(TEXT("spec %d is module %d's footprint - index IS the module"), Raw, Raw),
			Specs[Raw].Footprint.LengthUu == Expected.LengthUu && Specs[Raw].Footprint.WidthUu == Expected.WidthUu);
	}
	TestEqual(TEXT("and the Shed row is the one that runs bays (RunCap 3) - footprints alone could tie"),
		Specs[static_cast<int32>(EDepotModule::Shed)].RunCap, 3);

	return true;
}

/**
 * DepotKit::ReportIncomplete (#306), MOVED here from FAnchorLink::Build. Pinned directly
 * rather than through a whole Topology rebuild: a depot missing its shed or its pump warns,
 * one placed with both stays quiet, and the two log lines are the ones FAnchorLink::Build
 * used to print - verbatim is the refactor contract's own requirement for a moved UE_LOG.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDepotKitReportIncompleteTest,
	"Airside.Build.DepotKitReportIncomplete",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDepotKitReportIncompleteTest::RunTest(const FString& Parameters)
{
	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();

	// A TANK ONLY: neither module the warning names.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		FEntityPlacement Placement;
		Placement.Definition = Depot;
		Placement.Anchors = Depot->Anchors;
		Placement.Position = FVector2D(500.0, -500.0);
		Placement.PoseRole = Depot->PoseRole;
		Placement.Modules = { EDepotModule::Tank };
		Net->PlaceEntity(Placement);

		FWarningLogSpy Spy;
		GLog->AddOutputDevice(&Spy);
		DepotKit::ReportIncomplete(*Net);
		GLog->RemoveOutputDevice(&Spy);

		const bool bSawShed = Spy.Lines.ContainsByPredicate(
			[](const FString& Line) { return Line.Contains(TEXT("no shed placed")); });
		const bool bSawPump = Spy.Lines.ContainsByPredicate(
			[](const FString& Line) { return Line.Contains(TEXT("no pump placed")); });
		TestTrue(TEXT("a depot with no shed warns about it"), bSawShed);
		TestTrue(TEXT("and, separately, about its missing pump"), bSawPump);
	}

	// BOTH MODULES: no warning has anything left to name.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		FEntityPlacement Placement;
		Placement.Definition = Depot;
		Placement.Anchors = Depot->Anchors;
		Placement.Position = FVector2D(500.0, -500.0);
		Placement.PoseRole = Depot->PoseRole;
		Placement.Modules = { EDepotModule::Shed, EDepotModule::Pump };
		Net->PlaceEntity(Placement);

		FWarningLogSpy Spy;
		GLog->AddOutputDevice(&Spy);
		DepotKit::ReportIncomplete(*Net);
		GLog->RemoveOutputDevice(&Spy);

		TestEqual(TEXT("a complete depot warns about nothing"), Spy.Lines.Num(), 0);
	}

	return true;
}

/**
 * FDepotCapability (#443, ruled 2026-09-30 under #266): what a depot's modules GIVE it counts only the SEATED ones -
 * min(owned, the plot's ceiling) of each kind - with the legacy plotless exemption named. World-free: the ceiling is a
 * lambda standing for the plot solve.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDepotCapabilitySeatsAgainstTheCeilingTest,
	"Airside.Model.DepotCapability.SeatsOwnedAgainstTheCeiling",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDepotCapabilitySeatsAgainstTheCeilingTest::RunTest(const FString& Parameters)
{
	FEntityInstance Depot;
	Depot.Modules = { EDepotModule::Shed, EDepotModule::Shed, EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump, EDepotModule::Pump };
	auto Ceilings = [](int32 Sheds, int32 Tanks, int32 Pumps)
	{
		return [Sheds, Tanks, Pumps](EDepotModule Module)
		{
			return Module == EDepotModule::Shed ? Sheds : Module == EDepotModule::Tank ? Tanks : Pumps;
		};
	};

	{
		const FDepotCapability Capability = FDepotCapability::Seat(Depot, Ceilings(1, 0, 5));
		TestEqual(TEXT("three sheds owned, a plot for one: one seated"), Capability.SeatedOf(EDepotModule::Shed), 1);
		TestEqual(TEXT("a tank owned, a plot for none: none seated"), Capability.SeatedOf(EDepotModule::Tank), 0);
		TestEqual(TEXT("two pumps owned, room for five: both seated - a ceiling never seats more than is owned"), Capability.SeatedOf(EDepotModule::Pump), 2);
		TestTrue(TEXT("so it can fuel"), Capability.HasWorkingPump());
		TestEqual(TEXT("with two pumps"), Capability.Pumps(), 2);
		TestFalse(TEXT("a depot with modules is not the legacy exemption"), Capability.bLegacyPlotless);
	}
	{
		const FDepotCapability Capability = FDepotCapability::Seat(Depot, Ceilings(3, 1, 0));
		TestFalse(TEXT("pumps owned but none seated: the depot cannot fuel"), Capability.HasWorkingPump());
		TestEqual(TEXT("and the refill divisor is floored at one - a floor, not a claim"), Capability.Pumps(), 1);
	}
	{
		const FDepotCapability Capability = FDepotCapability::Seat(Depot, Ceilings(-4, -4, -4));
		TestEqual(TEXT("a negative ceiling seats nothing rather than a negative count"), Capability.SeatedOf(EDepotModule::Shed), 0);
	}

	// THE NAMED LEGACY EXEMPTION: no ground drawn and no module list is a depot placed without a plot.
	FEntityInstance Plotless;
	{
		const FDepotCapability Capability = FDepotCapability::Seat(Plotless, Ceilings(0, 0, 0));
		TestTrue(TEXT("no outline, no modules: the legacy plotless depot"), Capability.bLegacyPlotless);
		TestTrue(TEXT("which fuels as it always did"), Capability.HasWorkingPump());
		TestEqual(TEXT("as one pump"), Capability.Pumps(), 1);
		TestEqual(TEXT("and it has no bays: nothing is seated"), Capability.SeatedOf(EDepotModule::Shed), 0);
	}
	// A PLOTTED DEPOT WITH AN EMPTY KIT IS NOT LEGACY: its plot seats nothing, so it has no pump - "only placed modules count".
	{
		FEntityInstance EmptyPlot;
		EmptyPlot.PoseRole = EServiceRole::Fuel;
		EmptyPlot.Outline = { FVector2D(0.0, 0.0), FVector2D(5000.0, 0.0), FVector2D(5000.0, 2400.0), FVector2D(0.0, 2400.0) };
		const FDepotCapability Capability = FDepotCapability::Seat(EmptyPlot, Ceilings(3, 1, 1));
		TestFalse(TEXT("a plotted depot with no modules is not the legacy exemption"), Capability.bLegacyPlotless);
		TestFalse(TEXT("and cannot fuel"), Capability.HasWorkingPump());
	}
	// AN UNPLOTTED DEPOT WITH MODULES is a test's stand-in for a modular one: its list is the truth, no pump in it is no pump.
	{
		FEntityInstance Modular;
		Modular.Modules = { EDepotModule::Shed, EDepotModule::Tank };
		const FDepotCapability Capability = FDepotCapability::Seat(Modular, Ceilings(9, 9, 9));
		TestFalse(TEXT("modules and no ground: not legacy"), Capability.bLegacyPlotless);
		TestFalse(TEXT("and no pump in the list is no pump"), Capability.HasWorkingPump());
		TestEqual(TEXT("the owned counts are read off the view"), Capability.OwnedOf(EDepotModule::Shed), 1);
	}

	// A DEPOT WITH NO PLOT has nothing to be seated against - a module list on ground that was never drawn - so the owned list
	// stands even when a ceiling is asked (the plot solve says 0 for it, which is right for a purchase and wrong for what it
	// already holds). A PLOTTED depot is seated by the ceiling.
	{
		FEntityInstance Unplotted = Depot;
		Unplotted.PoseRole = EServiceRole::Fuel;
		const FDepotCapability Owned = FDepotCapability::Of(FEntityInstanceId(), Unplotted,
			[](FEntityInstanceId, const FEntityInstance&, EDepotModule) { return 0; });
		TestEqual(TEXT("unplotted: a ceiling of nothing seats nothing away - the owned sheds stand"), Owned.SeatedOf(EDepotModule::Shed), 3);
		FEntityInstance Plotted = Unplotted;
		Plotted.Outline = { FVector2D(0.0, 0.0), FVector2D(5000.0, 0.0), FVector2D(5000.0, 2400.0), FVector2D(0.0, 2400.0) };
		const FDepotCapability Seated = FDepotCapability::Of(FEntityInstanceId(), Plotted,
			[](FEntityInstanceId, const FEntityInstance&, EDepotModule Module) { return Module == EDepotModule::Shed ? 1 : 0; });
		TestEqual(TEXT("plotted: the same ceiling seats one shed"), Seated.SeatedOf(EDepotModule::Shed), 1);
		TestEqual(TEXT("and no pump"), Seated.SeatedOf(EDepotModule::Pump), 0);
	}

	// NO CEILING TO ASK (a world-free board or shop): the owned list stands - unseating needs a solve, and zero would make every
	// such depot pumpless and slotless.
	{
		const FDepotCapability Capability = FDepotCapability::Of(FEntityInstanceId(), Depot, FModuleCeilingFn());
		TestEqual(TEXT("with no plot solve every owned shed is seated"), Capability.SeatedOf(EDepotModule::Shed), 3);
		TestEqual(TEXT("and every owned pump"), Capability.SeatedOf(EDepotModule::Pump), 2);
	}
	return true;
}

/**
 * THE CENSUS READS THE SEATED MODULES (#443): a depot whose plot cannot hold its shed or its pump warns of both, naming what
 * is owned - which the player cannot see standing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDepotKitCensusSeatsTest,
	"Airside.Build.DepotKitReportIncompleteCountsOnlyWhatIsSeated",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDepotKitCensusSeatsTest::RunTest(const FString& Parameters)
{
	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	const TArray<PlotYard::FKitSpec> Specs = DepotKitSpecs(nullptr);

	auto Census = [&](const TArray<FVector2D>& Outline, const FVector2D& Frontage, TArrayView<const PlotYard::FKitSpec> WithSpecs,
		const TArray<EDepotModule>& Modules = { EDepotModule::Shed, EDepotModule::Pump })
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		FEntityPlacement Placement;
		Placement.Definition = Depot;
		Placement.Anchors = Depot->Anchors;
		Placement.Position = Frontage;
		Placement.PoseRole = Depot->PoseRole;
		Placement.Outline = Outline;
		Placement.Modules = Modules;
		Net->PlaceEntity(Placement);
		FWarningLogSpy Spy;
		GLog->AddOutputDevice(&Spy);
		DepotKit::ReportIncomplete(*Net, WithSpecs);
		GLog->RemoveOutputDevice(&Spy);
		return Spy.Lines;
	};
	auto Says = [](const TArray<FString>& Lines, const TCHAR* Text)
	{
		return Lines.ContainsByPredicate([Text](const FString& Line) { return Line.Contains(Text); });
	};

	// A PLOT TOO SMALL FOR ANYTHING: a 1 m square. The frontage is the edge whose midpoint is the pose.
	const TArray<FVector2D> Tiny = { FVector2D(0.0, 0.0), FVector2D(100.0, 0.0), FVector2D(100.0, 100.0), FVector2D(0.0, 100.0) };
	{
		const TArray<FString> Lines = Census(Tiny, FVector2D(50.0, 0.0), Specs);
		TestTrue(TEXT("a plot that cannot seat the shed warns of it, and says one is owned"), Says(Lines, TEXT("no shed placed (1 owned)")));
		TestTrue(TEXT("and of the pump"), Says(Lines, TEXT("no pump placed (1 owned)")));
	}
	// THE CONTROL: the same modules on a plot that holds them warn of nothing.
	const TArray<FVector2D> Roomy = { FVector2D(0.0, 0.0), FVector2D(5000.0, 0.0), FVector2D(5000.0, 2400.0), FVector2D(0.0, 2400.0) };
	{
		const TArray<FString> Lines = Census(Roomy, FVector2D(2500.0, 0.0), Specs);
		TestEqual(TEXT("control: a plot that seats a shed and a pump warns of nothing"), Lines.Num(), 0);
	}
	// AND WITH NO SPECS the owned list stands, as it always did: the tiny plot then warns of nothing.
	{
		const TArray<FString> Lines = Census(Tiny, FVector2D(50.0, 0.0), TArrayView<const PlotYard::FKitSpec>());
		TestEqual(TEXT("with no plot solve to ask, a depot holding a shed and a pump warns of nothing"), Lines.Num(), 0);
	}
	// THE TWO EMPTY DEPOTS, which FDepotCapability tells apart (#461 final review): an UNPLOTTED one with no modules is the
	// legacy plotless depot, which the census skips - the question does not apply; a PLOTTED one with an empty kit is not
	// legacy, its plot seats nothing, and it is warned of like any other depot missing a shed.
	{
		const TArray<FString> Lines = Census(TArray<FVector2D>(), FVector2D(50.0, 0.0), Specs, TArray<EDepotModule>());
		TestEqual(TEXT("an unplotted depot with no modules is the legacy exemption: not censused at all"), Lines.Num(), 0);
	}
	{
		const TArray<FString> Lines = Census(Roomy, FVector2D(2500.0, 0.0), Specs, TArray<EDepotModule>());
		TestTrue(FString::Printf(TEXT("a plotted depot with an empty kit is warned of, owning none (%s)"), *FString::Join(Lines, TEXT(" | "))),
			Says(Lines, TEXT("no shed placed (0 owned)")));
		TestTrue(TEXT("and of its missing pump"), Says(Lines, TEXT("no pump placed (0 owned)")));
	}
	return true;
}

/**
 * THE PLACEMENT'S REFUSAL, WORLD-FREE (#266): DepotKit::WhyUnseated over a reservation whose ceilings are known. Empty when
 * every module of the mix seats; otherwise naming each kind that does not, as seated-of-owned, and the total.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDepotKitWhyUnseatedTest,
	"Airside.Build.DepotKitWhyUnseatedNamesWhatDoesNotFit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDepotKitWhyUnseatedTest::RunTest(const FString& Parameters)
{
	// Two tank stands and one pump stand, no shed: KitIndex IS the EDepotModule (DepotKitSpecs walks the enum).
	PlotYard::FReservation Reservation;
	PlotYard::FReservedStand Tanks;
	Tanks.KitIndex = static_cast<int32>(EDepotModule::Tank);
	Tanks.RunLength = 2;
	PlotYard::FReservedStand Pump;
	Pump.KitIndex = static_cast<int32>(EDepotModule::Pump);
	Reservation.Stands = { Tanks, Pump };

	TestTrue(TEXT("a mix it holds is not refused"),
		DepotKit::WhyUnseated(Reservation, { EDepotModule::Tank, EDepotModule::Tank, EDepotModule::Pump }).IsEmpty());
	TestTrue(TEXT("nor is an empty mix - nothing to seat"), DepotKit::WhyUnseated(Reservation, {}).IsEmpty());

	const FString Why = DepotKit::WhyUnseated(Reservation, DepotKit::StarterModules());
	TestTrue(FString::Printf(TEXT("the starter mix is refused, naming the shed that does not fit (%s)"), *Why), Why.Contains(TEXT("Sheds 0 of 1")));
	TestFalse(TEXT("and not the kinds that do"), Why.Contains(TEXT("Tanks")) || Why.Contains(TEXT("Pumps")));
	TestTrue(TEXT("with the count seated out of the mix"), Why.Contains(TEXT("seats 2 of its 3")));

	const FString Pumps = DepotKit::WhyUnseated(Reservation, { EDepotModule::Pump, EDepotModule::Pump, EDepotModule::Shed });
	TestTrue(FString::Printf(TEXT("every kind short is named (%s)"), *Pumps), Pumps.Contains(TEXT("Pumps 1 of 2")) && Pumps.Contains(TEXT("Sheds 0 of 1")));
	TestEqual(TEXT("the starter mix is one of each"), DepotKit::StarterModules().Num(), 3);
	return true;
}

#endif
