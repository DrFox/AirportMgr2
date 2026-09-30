#include "Model/FacilityPurchases.h"

#include "AirportOpsLog.h"
#include "Model/JobBoard.h"
#include "Model/Ledger.h"
#include "Model/OpsEventBus.h"
#include "Model/Pricing.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"

#define LOCTEXT_NAMESPACE "FacilityPurchases"

FText UFacilityPurchases::RefusalText(EPurchaseRefusal Why)
{
	switch (Why)
	{
	case EPurchaseRefusal::None:           return FText::GetEmpty();
	case EPurchaseRefusal::NotAFacility:   return LOCTEXT("NotAFacility", "Not a facility");
	case EPurchaseRefusal::CannotAfford:   return LOCTEXT("CannotAfford", "Can't afford");
	case EPurchaseRefusal::NoSlotReserved: return LOCTEXT("NoSlotReserved", "No space");
	case EPurchaseRefusal::NoFreeBay:      return LOCTEXT("NoFreeBay", "No free bay - buy a shed");
	case EPurchaseRefusal::UnknownType:    return LOCTEXT("UnknownType", "Not for sale");
	case EPurchaseRefusal::VehicleBusy:    return LOCTEXT("VehicleBusy", "Busy");
	case EPurchaseRefusal::NoStandAdmits:  return LOCTEXT("NoStandAdmits", "Too large for any stand");
	}
	return FText::GetEmpty();
}

const FEntityInstance* UFacilityPurchases::FacilityAt(const URoadNetwork& Network, FEntityInstanceId Entity)
{
	// A FUEL DEPOT IS THE ONE FACILITY TODAY (R1 is the generic shape; the fuel depot its first consumer).
	const FEntityInstance* Instance = Network.GetEntity(Entity);
	return Instance != nullptr && Instance->bAlive && Instance->IsDepot() ? Instance : nullptr;
}

int32 UFacilityPurchases::VehicleSlotsOf(FEntityInstanceId Id, const FEntityInstance& Depot) const
{
	// THE SEATED MODULES, not the owned list (#443): a shed the plot could not seat grants no bay. Every kind with an
	// offer, seated count times its slots - the sum the owned walk made one module at a time.
	const FDepotCapability Capability = FDepotCapability::Of(Id, Depot, ReservedSlotsOf);
	int32 Slots = 0;
	for (int32 Kind = 0; Kind < FDepotCapability::KindCount; ++Kind)
	{
		if (const FModuleOffer* Offer = ModuleOffers.Find(static_cast<EDepotModule>(Kind)))
		{
			Slots += Capability.Seated[Kind] * Offer->VehicleSlots;
		}
	}
	return Slots;
}

int32 UFacilityPurchases::OwnedOf(const FEntityInstance& Facility, EDepotModule Module)
{
	int32 Count = 0;
	for (const EDepotModule Each : Facility.Modules)
	{
		Count += Each == Module ? 1 : 0;
	}
	return Count;
}

int32 UFacilityPurchases::ReservedOf(FEntityInstanceId Entity, const FEntityInstance& Facility, EDepotModule Module) const
{
	return ReservedSlotsOf ? ReservedSlotsOf(Entity, Facility, Module) : 0;
}

bool UFacilityPurchases::CanPay(double Price) const
{
	return Ledger == nullptr || Ledger->CanPay(Price);
}

double UFacilityPurchases::NowOrZero() const
{
	return Clock != nullptr ? Clock->Now() : 0.0;
}

FText UFacilityPurchases::Money(double Amount) const
{
	return Pricing != nullptr ? Pricing->Format(Amount) : FText::AsNumber(FMath::RoundToInt(Amount));
}

FText UFacilityPurchases::VehicleName(FName TypeCode) const
{
	return JobBoard != nullptr ? JobBoard->Fleet().NameOf(TypeCode) : FText::FromName(TypeCode);
}

double UFacilityPurchases::RefundOf(FName TypeCode) const
{
	// THE FLEET'S OWN READ, so the card's "Sell" label and the credit the sale posts are one number.
	return JobBoard != nullptr ? JobBoard->Fleet().ResaleOf(TypeCode) : 0.0;
}

void UFacilityPurchases::LogRefused(int32 Depot, const FString& What, EPurchaseRefusal Why) const
{
	// THE BARE NAME ("NoFreeBay"), the spec's log wording - not GetValueAsString's "EPurchaseRefusal::".
	UE_LOG(LogAirportOps, Log, TEXT("Purchase refused: depot %d %s - %s"), Depot, *What,
		*StaticEnum<EPurchaseRefusal>()->GetNameStringByValue(static_cast<int64>(Why)));
}

// --- The judgements: the ONE place each rule is written. Quote and command both call these. --------
// ORDER: what the player cannot fix by waiting first (not a facility, not for sale, no room), money last -
// a button reads "No space" rather than "Can't afford" when both are true, because saving up would not help.

EPurchaseRefusal UFacilityPurchases::JudgeModule(const FEntityInstance* Facility, EDepotModule Module, int32 Reserved) const
{
	// NO WRITE HOOK IS NO FACILITY, judged HERE rather than only in BuyModule: a quote that ignored it lit
	// "Buy Shed" for a command that could only refuse (task-7 review finding 1).
	// ENFORCED BY: AirportOps.Model.Facility.QuoteEqualsCommandForEveryOffer ("no module hook")
	if (Facility == nullptr || !ApplyModulePurchase)
	{
		return EPurchaseRefusal::NotAFacility;
	}
	const FModuleOffer* Offer = ModuleOffers.Find(Module);
	if (Offer == nullptr)
	{
		return EPurchaseRefusal::UnknownType;
	}
	if (OwnedOf(*Facility, Module) >= Reserved)
	{
		return EPurchaseRefusal::NoSlotReserved;
	}
	return CanPay(Offer->Price) ? EPurchaseRefusal::None : EPurchaseRefusal::CannotAfford;
}

EPurchaseRefusal UFacilityPurchases::JudgeVehicle(const URoadNetwork& Network, const FEntityInstance* Facility,
	FEntityInstanceId Entity, FName TypeCode) const
{
	if (Facility == nullptr || JobBoard == nullptr)
	{
		return EPurchaseRefusal::NotAFacility;
	}
	// A KIND THE CATALOGUE LACKS IS NOT FOR SALE (#430): a scenario row whose code has no chassis was dropped at attach,
	// and offering it would sell a vehicle the door refuses to make.
	const FServiceVehicleType* Kind = JobBoard->GetCatalogue().Find(TypeCode);
	if (Kind == nullptr)
	{
		return EPurchaseRefusal::UnknownType;
	}
	// A KIND NO STAND ADMITS IS NOT FOR SALE EITHER (#478): larger than every stand's design vehicle, it would be bought and
	// then refuse every job it bid on as VehicleTooLarge - the shop took the player's money for a vehicle that cannot work.
	// Refused here, where the module purchase refuses what the plot cannot seat (owner: "a player should not be able to
	// purchase upgrades that don't fit"), and BEFORE NoFreeBay: no shed fixes this one, so the player is not sent to buy one.
	// The refusal the inspector's rows already render (RefusalText), so no row changes with it.
	// ENFORCED BY: AirportOps.Model.Facility.KindNoStandAdmitsIsRefused, AirportOps.Model.Facility.QuoteEqualsCommandForEveryOffer
	if (!JobBoard->AnyStandAdmits(Kind->Vehicle, Network))
	{
		return EPurchaseRefusal::NoStandAdmits;
	}
	if (JobBoard->VehiclesAt(Entity) >= VehicleSlotsOf(Entity, *Facility))
	{
		return EPurchaseRefusal::NoFreeBay;
	}
	// THE FLEET'S PRICE, the one the charge and the ledger line post - see FServiceFleet::PriceOf.
	return CanPay(JobBoard->Fleet().PriceOf(TypeCode)) ? EPurchaseRefusal::None : EPurchaseRefusal::CannotAfford;
}

EPurchaseRefusal UFacilityPurchases::JudgeSale(int32 VehicleId) const
{
	if (JobBoard == nullptr || JobBoard->FindVehicle(VehicleId) == nullptr)
	{
		return EPurchaseRefusal::NotAFacility;
	}
	return JobBoard->CanRemoveVehicle(VehicleId) ? EPurchaseRefusal::None : EPurchaseRefusal::VehicleBusy;
}

FFacilityQuote UFacilityPurchases::Quote(const URoadNetwork& Network, FEntityInstanceId Entity) const
{
	FFacilityQuote Out;
	const FEntityInstance* Facility = FacilityAt(Network, Entity);
	if (Facility == nullptr || JobBoard == nullptr)
	{
		return Out;
	}
	Out.Refusal = EPurchaseRefusal::None;
	Out.Bays = VehicleSlotsOf(Entity, *Facility);
	Out.Vehicles = JobBoard->VehiclesAt(Entity);

	// SORTED, so the card's rows and a menu's line indices do not move between two asks.
	TArray<EDepotModule> Modules;
	ModuleOffers.GenerateKeyArray(Modules);
	Modules.Sort();
	for (const EDepotModule Module : Modules)
	{
		const FModuleOffer& Offer = ModuleOffers[Module];
		FModuleOfferQuote& Row = Out.Modules.AddDefaulted_GetRef();
		Row.Module = Module;
		Row.Name = Offer.DisplayName;
		Row.PluralName = Offer.PluralName;
		Row.Price = Offer.Price;
		Row.UpkeepPerDay = Offer.UpkeepPerDay;
		Row.Owned = OwnedOf(*Facility, Module);
		Row.Reserved = ReservedOf(Entity, *Facility, Module);
		Row.Refusal = JudgeModule(Facility, Module, Row.Reserved);
		Row.Label = FText::Format(LOCTEXT("ModuleLabel", "Buy {0} {1}"), Offer.DisplayName, Money(Offer.Price));
	}

	// FOR SALE = THE CATALOGUE (#430), not the scenario map: a row with no chassis was dropped at attach.
	TArray<FName> Types;
	JobBoard->GetCatalogue().GenerateKeyArray(Types);
	Types.Sort(FNameLexicalLess());
	for (const FName TypeCode : Types)
	{
		const FServiceVehicleType& Kind = JobBoard->GetCatalogue()[TypeCode];
		FVehicleOfferQuote& Row = Out.VehicleOffers.AddDefaulted_GetRef();
		Row.TypeCode = TypeCode;
		Row.Name = VehicleName(TypeCode);
		Row.Price = JobBoard->Fleet().PriceOf(TypeCode);
		Row.UpkeepPerDay = Kind.UpkeepPerDay;
		// THE TANK THE VEHICLE WILL CARRY, by the one capacity rule (FFuelRolePolicy::CapacityOf, floored) - the figure
		// the bid and the stand write on a job, so the label cannot promise a different tank.
		Row.CapacityLitres = FFuelRolePolicy::CapacityOf(Kind);
		Row.Refusal = JudgeVehicle(Network, Facility, Entity, TypeCode);
		Row.Label = FText::Format(LOCTEXT("VehicleLabel", "{0} {1} \u00B7 {2} L"), Row.Name, Money(Row.Price),
			FText::AsNumber(FMath::RoundToInt(Row.CapacityLitres)));
	}

	for (const FServiceVehicle& Vehicle : JobBoard->GetVehicles())
	{
		if (Vehicle.Home != Entity)
		{
			continue;
		}
		FFleetRowQuote& Row = Out.Fleet.AddDefaulted_GetRef();
		Row.VehicleId = Vehicle.Id;
		Row.TypeCode = Vehicle.TypeCode;
		Row.Line = JobBoard->VehicleLine(Vehicle, &Network);
		Row.Refund = RefundOf(Vehicle.TypeCode);
		Row.Refusal = JudgeSale(Vehicle.Id);
		Row.SellLabel = FText::Format(LOCTEXT("SellLabel", "Sell {0}"), Money(Row.Refund));
	}
	return Out;
}

FPurchaseResult UFacilityPurchases::BuyModule(const URoadNetwork& Network, FEntityInstanceId Entity, EDepotModule Module)
{
	FPurchaseResult Result;
	const FEntityInstance* Facility = FacilityAt(Network, Entity);
	const int32 Reserved = Facility != nullptr ? ReservedOf(Entity, *Facility, Module) : 0;
	Result.Refusal = JudgeModule(Facility, Module, Reserved);
	const FString What = StaticEnum<EDepotModule>()->GetNameStringByValue(static_cast<int64>(Module));
	if (!Result.Succeeded())
	{
		// THE UNSET HOOK is now JudgeModule's NotAFacility; its own warning kept, since a bare "NotAFacility"
		// on a live depot would send a reader looking at the depot rather than the attach.
		if (Facility != nullptr && !ApplyModulePurchase)
		{
			UE_LOG(LogAirportOps, Warning, TEXT("Purchase refused: depot %d %s - no module hook (UOpsRuntime not attached)"), Entity.Index, *What);
		}
		LogRefused(Entity.Index, What, Result.Refusal);
		return Result;
	}
	// READ BEFORE THE WRITE: the hook rebuilds the airport, and nothing here touches Facility after it.
	const FModuleOffer Offer = ModuleOffers[Module];
	const int32 Owned = OwnedOf(*Facility, Module) + 1;
	if (!ApplyModulePurchase(Entity, Module))
	{
		LogRefused(Entity.Index, What, EPurchaseRefusal::NotAFacility);
		Result.Refusal = EPurchaseRefusal::NotAFacility;
		return Result;
	}
	// CHARGED AFTER THE WRITE SUCCEEDED and in the same call - a refusal is never charged, and nothing can
	// move the balance between the judgement above and this line.
	Result.Amount = Offer.Price;
	if (Ledger != nullptr)
	{
		Ledger->Post(NowOrZero(), ELedgerCategory::Placement, -Offer.Price,
			FText::Format(LOCTEXT("BoughtModule", "Bought {0}"), Offer.DisplayName));
	}
	UE_LOG(LogAirportOps, Log, TEXT("Purchase: depot %d bought %s for %.0f (%d/%d slots)"),
		Entity.Index, *What, Offer.Price, Owned, Reserved);
	if (Bus != nullptr)
	{
		Bus->Publish(FFacilityUpgradedEvent{ Entity.Index, Module, Offer.Price });
	}
	return Result;
}

FPurchaseResult UFacilityPurchases::BuyVehicle(const URoadNetwork& Network, FEntityInstanceId Entity, FName TypeCode)
{
	FPurchaseResult Result;
	const FEntityInstance* Facility = FacilityAt(Network, Entity);
	Result.Refusal = JudgeVehicle(Network, Facility, Entity, TypeCode);
	if (!Result.Succeeded())
	{
		LogRefused(Entity.Index, TypeCode.ToString(), Result.Refusal);
		return Result;
	}
	// THE FLEET'S DOOR does the rest - the charge to Fleet, the FleetChanged event, the re-open of refused jobs - so a
	// purchase, a seeded vehicle and a withdrawn one owe the same things (#443). This judged it; that makes it so.
	const double Price = JobBoard->Fleet().PriceOf(TypeCode);
	Result.VehicleId = JobBoard->Fleet().Add(TypeCode, Entity, EFleetOrigin::Bought, NowOrZero());
	if (Result.VehicleId == 0)
	{
		Result.Refusal = EPurchaseRefusal::NotAFacility;
		LogRefused(Entity.Index, TypeCode.ToString(), Result.Refusal);
		return Result;
	}
	Result.Amount = Price;
	UE_LOG(LogAirportOps, Log, TEXT("Purchase: depot %d bought %s for %.0f (%d/%d bays)"),
		Entity.Index, *TypeCode.ToString(), Price, JobBoard->VehiclesAt(Entity), VehicleSlotsOf(Entity, *Facility));
	return Result;
}

FPurchaseResult UFacilityPurchases::SellVehicle(int32 VehicleId)
{
	FPurchaseResult Result;
	Result.Refusal = JudgeSale(VehicleId);
	const FServiceVehicle* Vehicle = JobBoard != nullptr ? JobBoard->FindVehicle(VehicleId) : nullptr;
	const int32 Depot = Vehicle != nullptr ? Vehicle->Home.Index : INDEX_NONE;
	if (!Result.Succeeded())
	{
		LogRefused(Depot, FString::Printf(TEXT("sell vehicle %d"), VehicleId), Result.Refusal);
		return Result;
	}
	// THE REFUND, READ BEFORE THE REMOVE (which invalidates Vehicle): the fleet's door credits this same figure, posts the
	// Fleet line and publishes FleetChanged{Sold}.
	const double Refund = RefundOf(Vehicle->TypeCode);
	if (!JobBoard->Fleet().Withdraw(VehicleId, EFleetReason::Sold, NowOrZero()))
	{
		// JudgeSale just said it could go; the door asks the same question (CanRemoveVehicle), so this is a race with
		// nothing between the two calls - said as busy rather than assumed away.
		Result.Refusal = EPurchaseRefusal::VehicleBusy;
		LogRefused(Depot, FString::Printf(TEXT("sell vehicle %d"), VehicleId), Result.Refusal);
		return Result;
	}
	Result.Amount = Refund;
	UE_LOG(LogAirportOps, Log, TEXT("Purchase: depot %d sold vehicle %d for %.0f"), Depot, VehicleId, Refund);
	return Result;
}

int32 UFacilityPurchases::RemoveUnseated(const URoadNetwork& Network)
{
	// DECIDED FIRST, WRITTEN AFTER: each removal rebuilds the airport through the facade, and a walk of the entity list
	// that wrote as it went would be reading a list its own writes were rebuilding around it.
	struct FExcess
	{
		FEntityInstanceId Depot;
		FVector2D At = FVector2D::ZeroVector;
		EDepotModule Module = EDepotModule::Shed;
		int32 Owned = 0;
		int32 Seated = 0;
	};
	TArray<FExcess> Excess;
	const TArray<FEntityInstance>& Entities = Network.GetEntities();
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		const FEntityInstance& Entity = Entities[Index];
		if (!Entity.bAlive || !Entity.IsDepot())
		{
			continue;
		}
		// THE SEAT THE PRESENTER DRAWS AND THE PURCHASE JUDGES (FDepotCapability::Of over ReservedSlotsOf): a plotless depot,
		// or no ceiling hook, takes its owned list as seated and has nothing to remove.
		const FEntityInstanceId Id = Network.EntityIdAt(Index);
		const FDepotCapability Capability = FDepotCapability::Of(Id, Entity, ReservedSlotsOf);
		for (int32 Kind = 0; Kind < FDepotCapability::KindCount; ++Kind)
		{
			const EDepotModule Module = static_cast<EDepotModule>(Kind);
			if (Capability.UnseatedOf(Module) > 0)
			{
				Excess.Add({ Id, Entity.Position, Module, Capability.OwnedOf(Module), Capability.SeatedOf(Module) });
			}
		}
	}
	if (Excess.Num() > 0 && !ApplyModuleRemoval)
	{
		// SAID, NOT GUESSED AROUND: with no door to the network nothing can be removed, and a refund alone would be money
		// for nothing. The presenter's per-plot Warning is still there to name each depot.
		UE_LOG(LogAirportOps, Warning, TEXT("Repair: %d depot kind(s) own modules their plot cannot seat, and there is no removal hook (UOpsRuntime not attached)"),
			Excess.Num());
		return 0;
	}

	int32 RemovedTotal = 0;
	for (const FExcess& Each : Excess)
	{
		const FString What = StaticEnum<EDepotModule>()->GetNameStringByValue(static_cast<int64>(Each.Module));
		const int32 Removed = ApplyModuleRemoval(Each.Depot, Each.Module, Each.Owned - Each.Seated);
		if (Removed <= 0)
		{
			// THE FACADE REFUSED (a drag is open): nothing went, so nothing is paid. The next network change asks again.
			UE_LOG(LogAirportOps, Log, TEXT("Repair: depot %d's %d unseated %s not removed yet - the network refused the write"),
				Each.Depot.Index, Each.Owned - Each.Seated, *What);
			continue;
		}
		RemovedTotal += Removed;
		// THE OFFER'S PRICE, per module - see RemoveUnseated's header for why, and why a kind with no offer refunds nothing.
		const FModuleOffer* Offer = ModuleOffers.Find(Each.Module);
		const double Refund = Offer != nullptr ? Offer->Price * Removed : 0.0;
		if (Ledger != nullptr && Refund > 0.0)
		{
			Ledger->Post(NowOrZero(), ELedgerCategory::Refund, Refund,
				FText::Format(LOCTEXT("RefundedModules", "Refunded {0} x {1} (no room on its plot)"),
					FText::AsNumber(Removed), Offer->DisplayName));
		}
		UE_LOG(LogAirportOps, Warning,
			TEXT("Repair: depot %d at (%.0f, %.0f) owned %d %s but its plot seats %d - removed %d, refunded %.0f%s"),
			Each.Depot.Index, Each.At.X, Each.At.Y, Each.Owned, *What, Each.Seated, Removed, Refund,
			Offer != nullptr ? TEXT("") : TEXT(" (not sold alone: it came with the plot)"));
		if (Bus != nullptr)
		{
			Bus->Publish(FModulesRefundedEvent{ Each.Depot.Index, Each.Module, Removed, Refund });
		}
	}
	return RemovedTotal;
}

FFacilityUpkeep UFacilityPurchases::DailyUpkeep(const URoadNetwork& Network) const
{
	FFacilityUpkeep Out;
	for (const FEntityInstance& Entity : Network.GetEntities())
	{
		if (!Entity.bAlive || !Entity.IsDepot())
		{
			continue;
		}
		for (const EDepotModule Module : Entity.Modules)
		{
			if (const FModuleOffer* Offer = ModuleOffers.Find(Module))
			{
				Out.Modules += Offer->UpkeepPerDay;
			}
		}
	}
	if (JobBoard != nullptr)
	{
		for (const FServiceVehicle& Vehicle : JobBoard->GetVehicles())
		{
			Out.Fleet += JobBoard->TypeFor(Vehicle.TypeCode).UpkeepPerDay;
		}
	}
	return Out;
}

#undef LOCTEXT_NAMESPACE
