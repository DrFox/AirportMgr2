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
	}
	return FText::GetEmpty();
}

const FEntityInstance* UFacilityPurchases::FacilityAt(const URoadNetwork& Network, FEntityInstanceId Entity)
{
	// A FUEL DEPOT IS THE ONE FACILITY TODAY (R1 is the generic shape; the fuel depot its first consumer).
	const FEntityInstance* Instance = Network.GetEntity(Entity);
	return Instance != nullptr && Instance->bAlive && Instance->IsDepot() ? Instance : nullptr;
}

int32 UFacilityPurchases::VehicleSlotsOf(const FEntityInstance& Depot) const
{
	int32 Slots = 0;
	for (const EDepotModule Module : Depot.Modules)
	{
		if (const FModuleOffer* Offer = ModuleOffers.Find(Module))
		{
			Slots += Offer->VehicleSlots;
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
	const FFuelVehicleSpec* Spec = JobBoard != nullptr ? JobBoard->VehicleSpecs.Find(TypeCode) : nullptr;
	return Spec != nullptr && !Spec->DisplayName.IsEmpty() ? Spec->DisplayName : FText::FromName(TypeCode);
}

double UFacilityPurchases::RefundOf(FName TypeCode) const
{
	const FFuelVehicleSpec* Spec = JobBoard != nullptr ? JobBoard->VehicleSpecs.Find(TypeCode) : nullptr;
	return Spec != nullptr ? Spec->Price * Spec->ResaleFraction : 0.0;
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

EPurchaseRefusal UFacilityPurchases::JudgeVehicle(const FEntityInstance* Facility, FEntityInstanceId Entity, FName TypeCode) const
{
	if (Facility == nullptr || JobBoard == nullptr)
	{
		return EPurchaseRefusal::NotAFacility;
	}
	const FFuelVehicleSpec* Spec = JobBoard->VehicleSpecs.Find(TypeCode);
	if (Spec == nullptr)
	{
		return EPurchaseRefusal::UnknownType;
	}
	if (JobBoard->VehiclesAt(Entity) >= VehicleSlotsOf(*Facility))
	{
		return EPurchaseRefusal::NoFreeBay;
	}
	return CanPay(Spec->Price) ? EPurchaseRefusal::None : EPurchaseRefusal::CannotAfford;
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
	Out.Bays = VehicleSlotsOf(*Facility);
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

	TArray<FName> Types;
	JobBoard->VehicleSpecs.GenerateKeyArray(Types);
	Types.Sort(FNameLexicalLess());
	for (const FName TypeCode : Types)
	{
		const FFuelVehicleSpec& Spec = JobBoard->VehicleSpecs[TypeCode];
		FVehicleOfferQuote& Row = Out.VehicleOffers.AddDefaulted_GetRef();
		Row.TypeCode = TypeCode;
		Row.Name = VehicleName(TypeCode);
		Row.Price = Spec.Price;
		Row.UpkeepPerDay = Spec.UpkeepPerDay;
		Row.CapacityLitres = Spec.CapacityLitres;
		Row.Refusal = JudgeVehicle(Facility, Entity, TypeCode);
		Row.Label = FText::Format(LOCTEXT("VehicleLabel", "{0} {1} \u00B7 {2} L"), Row.Name, Money(Spec.Price),
			FText::AsNumber(FMath::RoundToInt(Spec.CapacityLitres)));
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
		Row.Line = JobBoard->VehicleLine(Vehicle);
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
	Result.Refusal = JudgeVehicle(Facility, Entity, TypeCode);
	if (!Result.Succeeded())
	{
		LogRefused(Entity.Index, TypeCode.ToString(), Result.Refusal);
		return Result;
	}
	const double Price = JobBoard->VehicleSpecs[TypeCode].Price;
	Result.VehicleId = JobBoard->AddPurchasedVehicle(TypeCode, Entity);
	if (Result.VehicleId == 0)
	{
		Result.Refusal = EPurchaseRefusal::NotAFacility;
		LogRefused(Entity.Index, TypeCode.ToString(), Result.Refusal);
		return Result;
	}
	Result.Amount = Price;
	if (Ledger != nullptr)
	{
		Ledger->Post(NowOrZero(), ELedgerCategory::Fleet, -Price, FText::Format(LOCTEXT("BoughtVehicle", "Bought {0}"), VehicleName(TypeCode)));
	}
	UE_LOG(LogAirportOps, Log, TEXT("Purchase: depot %d bought %s for %.0f (%d/%d bays)"),
		Entity.Index, *TypeCode.ToString(), Price, JobBoard->VehiclesAt(Entity), VehicleSlotsOf(*Facility));
	if (Bus != nullptr)
	{
		Bus->Publish(FFleetChangedEvent{ Entity.Index, Result.VehicleId, TypeCode, EFleetChange::Bought, Price });
	}
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
	// COPIED BEFORE THE REMOVE, which invalidates Vehicle.
	const FName TypeCode = Vehicle->TypeCode;
	const double Refund = RefundOf(TypeCode);
	JobBoard->RemoveVehicle(VehicleId);
	Result.Amount = Refund;
	if (Ledger != nullptr)
	{
		Ledger->Post(NowOrZero(), ELedgerCategory::Fleet, Refund,
			FText::Format(LOCTEXT("SoldVehicle", "Sold {0} #{1}"), VehicleName(TypeCode), FText::AsNumber(VehicleId)));
	}
	UE_LOG(LogAirportOps, Log, TEXT("Purchase: depot %d sold vehicle %d for %.0f"), Depot, VehicleId, Refund);
	if (Bus != nullptr)
	{
		Bus->Publish(FFleetChangedEvent{ Depot, VehicleId, TypeCode, EFleetChange::Sold, Refund });
	}
	return Result;
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
			Out.Fleet += JobBoard->SpecFor(Vehicle.TypeCode).UpkeepPerDay;
		}
	}
	return Out;
}

#undef LOCTEXT_NAMESPACE
