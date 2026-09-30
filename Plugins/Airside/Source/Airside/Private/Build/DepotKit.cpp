#include "Build/DepotKit.h"

#include "AirsideLog.h"
#include "Build/PlotLayoutStrategy.h"
#include "Content/AirsideContent.h"
#include "Entities/EntityDefinition.h"
#include "Entities/PlotModuleKit.h"
#include "Model/DepotCapability.h"
#include "Model/RoadNetwork.h"

PlotYard::FFootprint DepotFootprint(EDepotModule Module, const UAirsideContent* Content)
{
	// THE AUTHORED KIT WINS WHOLE. Taking the footprint from the kit and the back-fence flag
	// from the table below would be two sources for one module's behaviour, and they would
	// disagree the first time a kit was authored for something that does not front the gate.
	if (Content != nullptr)
	{
		if (const TObjectPtr<UPlotModuleKit>* Found = Content->DepotKits.Find(Module))
		{
			if (const UPlotModuleKit* Kit = *Found)
			{
				PlotYard::FFootprint Out;
				Out.LengthUu = Kit->Footprint.X;
				Out.WidthUu = Kit->Footprint.Y;
				Out.bAgainstTheBackFence = Kit->bAgainstTheBackFence;
				return Out;
			}
		}
	}

	// NO KIT IS A LEGAL ANSWER, not an error: the grey-box table below is what every caller
	// used before kits existed and what they still get until one is authored.
	return DepotFootprint(Module);
}

PlotYard::FFootprint DepotFootprint(EDepotModule Module)
{
	// THEY DIFFER, and that is the point. All three were one bay and drew at bay size, so
	// scattering them would give a jumble of IDENTICAL boxes - varied placement is what
	// makes differing footprints legible, where the grid hid them. Grey-box figures chosen
	// for LEGIBILITY like the heights in PlotPresenter, not measured off the concept sheet;
	// the real ones arrive with the meshes.
	PlotYard::FFootprint Out;
	switch (Module)
	{
	case EDepotModule::Shed:
		Out.LengthUu = 800.0;
		Out.WidthUu = 400.0;
		// THE ONLY ONE STOOD AGAINST THE BACK FENCE. A truck drives out of it, so its heading
		// is functional and may not be turned for looks - and it stands at the back rather
		// than in the gateway, which is where it used to be.
		Out.bAgainstTheBackFence = true;
		return Out;
	case EDepotModule::Tank:
		Out.LengthUu = 500.0;
		Out.WidthUu = 500.0;
		return Out;
	case EDepotModule::Pump:
		Out.LengthUu = 300.0;
		Out.WidthUu = 200.0;
		return Out;
	case EDepotModule::Count:
		// THE SENTINEL, NOT A MODULE - named explicitly rather than folded into a default so
		// a genuinely new module still falls through with no case here and keeps warning.
		break;
	}

	// A module added to the enum with no entry here gets a bay-sized box rather than a
	// zero-sized one, which would sample into every other module without ever colliding.
	Out.LengthUu = 400.0;
	Out.WidthUu = 400.0;
	return Out;
}

TArray<PlotYard::FKitSpec> DepotKitSpecs(const UAirsideContent* Content)
{
	TArray<PlotYard::FKitSpec> Specs;

	// WALKED TO THE SENTINEL, not to Pump: that was the last value once, and "adding a module
	// after it extends this loop with no edit" was false the moment it stopped being last -
	// issue #193. EDepotModule::Count is UMETA(Hidden), never a real module, and it moves
	// itself every time a real value is added before it, which is what makes this true again.
	for (int32 Raw = 0; Raw < static_cast<int32>(EDepotModule::Count); ++Raw)
	{
		const EDepotModule Module = static_cast<EDepotModule>(Raw);

		PlotYard::FKitSpec Spec;
		Spec.Footprint = DepotFootprint(Module, Content);

		// THE GREY-BOX MIX, and it lives here rather than only on the assets so that an
		// unauthored game still reserves a sensible depot instead of one of everything. A
		// shed comes up three times a cycle and runs three bays; a pump comes up once and
		// never groups. These are the figures the design doc names.
		switch (Module)
		{
		case EDepotModule::Shed: Spec.ReserveWeight = 3; Spec.RunCap = 3; break;
		case EDepotModule::Tank: Spec.ReserveWeight = 2; Spec.RunCap = 1; break;
		case EDepotModule::Pump: Spec.ReserveWeight = 1; Spec.RunCap = 1; break;
		default:                 Spec.ReserveWeight = 1; Spec.RunCap = 1; break;
		}

		// AN AUTHORED KIT OVERRIDES BOTH. Tuning the mix is then one number on an asset
		// rather than a recompile, which is the whole reason the weight lives on the kit.
		if (Content != nullptr)
		{
			if (const TObjectPtr<UPlotModuleKit>* Found = Content->DepotKits.Find(Module))
			{
				if (const UPlotModuleKit* Kit = *Found)
				{
					Spec.ReserveWeight = FMath::Max(Kit->ReserveWeight, 0);
					Spec.RunCap = FMath::Max(Kit->RunCap, 1);
					Spec.ApronUu = Kit->ApronUu;
					// ONLY A PARTS KIT HAS CAPS. A baked mesh's footprint already is the
					// whole building, so a cap width left on one after switching it to
					// Baked must not reserve ground nothing draws.
					Spec.RunEndUu = Kit->Assembly == EKitAssembly::Parts
						? FMath::Max(Kit->PartCapUu, 0.0) : 0.0;
				}
			}
		}

		Specs.Add(Spec);
	}

	return Specs;
}

FString DepotKitLabel(EDepotModule Module)
{
	switch (Module)
	{
	case EDepotModule::Shed: return TEXT("Sheds");
	case EDepotModule::Tank: return TEXT("Tanks");
	case EDepotModule::Pump: return TEXT("Pumps");
	// THE SENTINEL, NOT A MODULE - named explicitly so a genuinely new module still falls
	// through with no case here and keeps warning.
	case EDepotModule::Count: break;
	}

	// A module added to the enum with no label here says SOMETHING rather than nothing: a
	// blank row on the bar reads as a broken readout, and the number beside it is still true.
	return TEXT("Modules");
}

int32 DepotYardSeed(FVector2D Where)
{
	const int32 X = FMath::RoundToInt(Where.X);
	const int32 Y = FMath::RoundToInt(Where.Y);
	return static_cast<int32>(HashCombine(GetTypeHash(X), GetTypeHash(Y)));
}

void DepotKit::ReportIncomplete(const URoadNetwork& Network, TArrayView<const PlotYard::FKitSpec> Specs)
{
	// MOVED FROM FAnchorLink::Build, 2026-09-26 (#306): a fuel-depot module census had
	// nothing to do with joining lead-ins and was only ever run from inside that function
	// because it was the last thing Topology touched Network with. See this function's own
	// header comment for why it is called again from the presenter after every edit.
	for (const FEntityInstance& Entity : Network.GetEntities())
	{
		if (!Entity.bAlive || !Entity.IsDepot())
		{
			continue;
		}

		// THE SEATED MODULES (#443): the plot's own solve, the ceilings the presenter draws from, so this warns of a
		// depot whose shed or pump the player cannot see standing. A depot with no plot to solve (no Specs, or one drawn
		// with no outline - a test's) has nothing to be smaller than, and its owned list stands.
		TOptional<PlotYard::FReservation> Reserved;
		const bool bSolvable = Specs.Num() > 0 && FDepotCapability::HasPlot(Entity);
		if (bSolvable)
		{
			Reserved = ReservationOf(Entity, Specs);
		}
		const FDepotCapability Capability = FDepotCapability::Seat(Entity, [&](EDepotModule Module)
			{
				if (!bSolvable)
				{
					return MAX_int32;
				}
				return Reserved.IsSet() ? Reserved->CeilingFor(static_cast<int32>(Module)) : 0;
			});

		// THE LEGACY PLOTLESS DEPOT (no ground, no module list) is not censused: the question does not apply to it. A
		// plotted depot with an empty kit is - it seats nothing.
		if (Capability.bLegacyPlotless)
		{
			continue;
		}
		const int32 OwnedSheds = Capability.OwnedOf(EDepotModule::Shed);
		const int32 OwnedPumps = Capability.OwnedOf(EDepotModule::Pump);

		// "NO TRUCKS" WAS UNTRUE for a starter depot, whose vehicles are seeded whatever it holds; what a shed gives is
		// the BAY a bought vehicle stands in, so that is what the warning says is missing.
		if (Capability.SeatedOf(EDepotModule::Shed) == 0)
		{
			UE_LOG(LogAirside, Warning,
				TEXT("Fuel depot at (%.0f, %.0f): no shed placed (%d owned), so no bay for a vehicle. Build one in a bay."),
				Entity.Position.X, Entity.Position.Y, OwnedSheds);
		}
		if (Capability.SeatedOf(EDepotModule::Pump) == 0)
		{
			UE_LOG(LogAirside, Warning,
				TEXT("Fuel depot at (%.0f, %.0f): no pump placed (%d owned), so nothing can be fuelled. "
					 "Build one in a bay."),
				Entity.Position.X, Entity.Position.Y, OwnedPumps);
		}
	}
}

bool DepotKit::RecoverFrontage(const FEntityInstance& Entity, FVector2D& OutA, FVector2D& OutB)
{
	// Which edge of the plot is its frontage, recovered from the entity alone.
	//
	// EXACT, NOT A GUESS, and not a second search either. URoadEditFacade::PlaceEntityInPlot
	// puts the pose at the MIDPOINT of the frontage edge, so the edge whose midpoint equals
	// Position is that edge by construction - this reads back a value rather than deriving a
	// new opinion.
	//
	// ASKING FAnchorLink AGAIN WAS REJECTED. It would search the live graph, so a road laid
	// or deleted after the depot was built could move the frontage, and every shed in the
	// yard would jump to a new edge without the player touching the depot. Where the thing
	// faces was decided when it was placed, and it stays decided.
	double BestDistance = TNumericLimits<double>::Max();
	int32 BestEdge = INDEX_NONE;

	for (int32 I = 0; I < Entity.Outline.Num(); ++I)
	{
		const FVector2D& A = Entity.Outline[I];
		const FVector2D& B = Entity.Outline[(I + 1) % Entity.Outline.Num()];
		const double Distance = FVector2D::Distance((A + B) * 0.5, Entity.Position);
		if (Distance < BestDistance)
		{
			BestDistance = Distance;
			BestEdge = I;
		}
	}

	if (BestEdge == INDEX_NONE)
	{
		return false;
	}

	OutA = Entity.Outline[BestEdge];
	OutB = Entity.Outline[(BestEdge + 1) % Entity.Outline.Num()];
	return true;
}

TOptional<PlotYard::FReservation> DepotKit::ReservationOf(const FEntityInstance& Depot,
	TArrayView<const PlotYard::FKitSpec> Specs)
{
	if (!Depot.bAlive || !Depot.IsDepot() || !Depot.IsPlotted())
	{
		return {};
	}
	FVector2D FrontageA = FVector2D::ZeroVector;
	FVector2D FrontageB = FVector2D::ZeroVector;
	if (!RecoverFrontage(Depot, FrontageA, FrontageB))
	{
		return {};
	}
	// Depot.Outline OUTLIVES THE SOLVE - FPlotSite::Outline is a view (its own comment).
	FPlotSite Site;
	Site.Outline = Depot.Outline;
	Site.FrontageA = FrontageA;
	Site.FrontageB = FrontageB;
	Site.Gate = Depot.Position;
	Site.Seed = DepotYardSeed(Depot.Position);
	const EPlotLayout Layout = Depot.Definition != nullptr ? Depot.Definition->Layout : EPlotLayout::Scatter;
	return PlotLayoutFor(Layout)->Solve(Site, Specs);
}

TArray<EDepotModule> DepotKit::StarterModules()
{
	return { EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump };
}

FString DepotKit::WhyUnseated(const PlotYard::FReservation& Reservation, TConstArrayView<EDepotModule> Modules)
{
	// A PLOT, by construction: the reservation was solved for drawn ground. INDEXED BY EDepotModule, as every spec table
	// here is (DepotKitSpecs walks the enum), so a kind's ceiling is its own index's.
	const FDepotCapability Capability = FDepotCapability::Seat(Modules, /*bHasPlot=*/true,
		[&Reservation](EDepotModule Module) { return Reservation.CeilingFor(static_cast<int32>(Module)); });

	// EVERY KIND THAT DOES NOT FIT, named with its count - "Sheds 0 of 1" - so the player reads WHICH building wants the
	// room, not only that something does. Labels are DepotKitLabel's, the readout's own kit rows.
	FString Short;
	int32 Seated = 0;
	for (int32 Kind = 0; Kind < FDepotCapability::KindCount; ++Kind)
	{
		const EDepotModule Module = static_cast<EDepotModule>(Kind);
		Seated += Capability.SeatedOf(Module);
		if (Capability.UnseatedOf(Module) > 0)
		{
			Short += FString::Printf(TEXT("%s%s %d of %d"), Short.IsEmpty() ? TEXT("") : TEXT(", "),
				*DepotKitLabel(Module), Capability.SeatedOf(Module), Capability.OwnedOf(Module));
		}
	}
	if (Short.IsEmpty())
	{
		return FString();
	}
	return FString::Printf(TEXT("the plot seats %d of its %d starter modules (%s) - draw it larger"),
		Seated, Modules.Num(), *Short);
}
