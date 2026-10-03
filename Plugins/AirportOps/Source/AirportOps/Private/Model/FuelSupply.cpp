#include "Model/FuelSupply.h"

#include "AirportOpsLog.h"
#include "Model/Ledger.h"
#include "Model/OpsEventBus.h"
#include "Model/ServiceRolePolicy.h"

void UFuelSupply::OnBeforeRestore()
{
	StockLitres = 0.0;
	Contract = FFuelContract();
	SpotOrders.Reset();
}

double UFuelSupply::Capacity() const
{
	return CapacityOf ? FMath::Max(CapacityOf(), 0.0) : TNumericLimits<double>::Max();
}

bool UFuelSupply::IsLow() const
{
	// CAPACITY 0 IS NO TANKS, NOT LOW FUEL (0 is under a quarter of nothing). NO CapacityOf IS UNBOUNDED, huge, so any stock would
	// read as under a quarter of it - and a detached runtime (CapacityOf cleared) must not raise an alert for tanks nobody asked
	// about, so no hook is never low. A contract means deliveries are already coming; spot orders on the way are stock the player
	// has paid for.
	// ENFORCED BY: AirportOps.Model.Alerts.FuelLowWhenUnderAQuarter
	if (!CapacityOf) { return false; }
	const double Cap = Capacity();
	return Cap > 0.0 && Contract.Tier == INDEX_NONE && Available() + PendingSpotLitres() < 0.25 * Cap;
}

bool UFuelSupply::IsDry() const
{
	return Available() < FFuelRolePolicy::FuelledWithinLitres;
}

double UFuelSupply::Draw(double Litres)
{
	const double Granted = FMath::Clamp(Litres, 0.0, Available());
	StockLitres -= Granted;
	return Granted;
}

double UFuelSupply::Receive(double Litres)
{
	const double Added = FMath::Clamp(Litres, 0.0, FreeSpace());
	StockLitres += Added;
	return Added;
}

double UFuelSupply::PendingSpotLitres() const
{
	double Total = 0.0;
	for (const FFuelSpotOrder& Order : SpotOrders) { Total += Order.Litres; }
	return Total;
}

EFuelOrderRefusal UFuelSupply::JudgeSpot(double Litres) const
{
	// ROOM COUNTS WHAT IS ON THE WAY: two orders that each fit alone must not together overflow - a spot order is not
	// take-or-pay, and refusing at the order is the only place the player can still change their mind. `!(Litres > 0)`, not
	// `Litres <= 0`: NaN compares false both ways, so the latter - and the room test after it - let a NaN order through.
	// ENFORCED BY: AirportOps.Model.FuelSupply.SpotRefusalsChargeNothing
	if (!(Litres > 0.0) || Litres > FreeSpace() - PendingSpotLitres()) { return EFuelOrderRefusal::NoRoom; }
	if (Ledger != nullptr && Ledger->Balance() < Litres * Figures.SpotPricePerLitre) { return EFuelOrderRefusal::CannotAfford; }
	return EFuelOrderRefusal::None;
}

EFuelOrderRefusal UFuelSupply::OrderSpot(double Litres, double Now)
{
	const EFuelOrderRefusal Why = JudgeSpot(Litres);
	if (Why != EFuelOrderRefusal::None) { return Why; }
	if (Ledger != nullptr)
	{
		Ledger->Post(Now, ELedgerCategory::FuelPurchase, -Litres * Figures.SpotPricePerLitre,
			FText::Format(NSLOCTEXT("Ledger", "FuelSpot", "Spot fuel, {0} L"), FText::AsNumber(FMath::RoundToInt(Litres))));
	}
	FFuelSpotOrder Order;
	Order.Litres = Litres;
	Order.DueAt = Now + Figures.SpotDelaySeconds;
	SpotOrders.Add(Order);
	UE_LOG(LogAirportOps, Log, TEXT("Fuel: spot order %.0f L, due at %.0f"), Litres, Order.DueAt);
	return EFuelOrderRefusal::None;
}

int32 UFuelSupply::ReceiveDueSpot(double Now)
{
	int32 Delivered = 0;
	for (int32 Index = SpotOrders.Num() - 1; Index >= 0; --Index)
	{
		if (SpotOrders[Index].DueAt <= Now)
		{
			const double Litres = SpotOrders[Index].Litres;
			const double Added = Receive(Litres);
			SpotOrders.RemoveAt(Index);
			++Delivered;
			UE_LOG(LogAirportOps, Log, TEXT("Fuel: spot delivery %.0f L (%.0f added), stock %.0f L"), Litres, Added, StockLitres);
			if (Bus != nullptr) { Bus->Publish(FFuelDeliveredEvent{ Litres, Added, false }); }
		}
	}
	return Delivered;
}

EFuelOrderRefusal UFuelSupply::JudgeContract(int32 Tier) const
{
	if (Contract.Tier != INDEX_NONE) { return EFuelOrderRefusal::AlreadyContracted; }
	if (!Figures.ContractTiers.IsValidIndex(Tier)) { return EFuelOrderRefusal::UnknownTier; }
	// STORAGE GATES THE TIERS (spec §7): a tier whose day the tanks cannot hold would pour fuel away from the first day.
	if (Figures.ContractTiers[Tier].LitresPerDay > Capacity()) { return EFuelOrderRefusal::NoRoom; }
	return EFuelOrderRefusal::None;
}

EFuelOrderRefusal UFuelSupply::SignContract(int32 Tier, double Now)
{
	const EFuelOrderRefusal Why = JudgeContract(Tier);
	if (Why != EFuelOrderRefusal::None) { return Why; }
	Contract.Tier = Tier;
	Contract.DaysLeft = Figures.ContractTermDays;
	UE_LOG(LogAirportOps, Log, TEXT("Fuel: contract signed at %.0f, %.0f L a day for %d day(s)"),
		Now, Figures.ContractTiers[Tier].LitresPerDay, Contract.DaysLeft);
	return EFuelOrderRefusal::None;
}

void UFuelSupply::DeliverContractDay(double Now)
{
	if (Contract.Tier == INDEX_NONE || !Figures.ContractTiers.IsValidIndex(Contract.Tier)) { return; }
	const FFuelContractTier& Tier = Figures.ContractTiers[Contract.Tier];
	// TAKE-OR-PAY (spec §7): the whole day is charged before any of it is measured against the tanks. Like upkeep it posts
	// whatever the balance - a negative balance locks placement (GDD §11), it does not stop the fuel.
	if (Ledger != nullptr)
	{
		Ledger->Post(Now, ELedgerCategory::FuelPurchase, -Tier.LitresPerDay * Tier.PricePerLitre,
			NSLOCTEXT("Ledger", "FuelContract", "Fuel contract delivery"));
	}
	const double Added = Receive(Tier.LitresPerDay);
	UE_LOG(LogAirportOps, Log, TEXT("Fuel: contract delivery %.0f L (%.0f added, %.0f poured away), stock %.0f L, %d day(s) left"),
		Tier.LitresPerDay, Added, Tier.LitresPerDay - Added, StockLitres, Contract.DaysLeft - 1);
	if (Bus != nullptr) { Bus->Publish(FFuelDeliveredEvent{ Tier.LitresPerDay, Added, true }); }
	if (--Contract.DaysLeft <= 0) { Contract = FFuelContract(); }
}

FFuelQuote UFuelSupply::Quote(double SpotLitres) const
{
	FFuelQuote Out;
	Out.StockLitres = StockLitres;
	Out.CapacityLitres = Capacity();
	Out.bBounded = static_cast<bool>(CapacityOf);
	Out.PendingLitres = PendingSpotLitres();
	Out.SpotLitres = SpotLitres;
	Out.SpotCost = SpotLitres * Figures.SpotPricePerLitre;
	Out.SpotDelaySeconds = Figures.SpotDelaySeconds;
	Out.TermDays = Figures.ContractTermDays;
	// THE VERDICTS ARE THE JUDGES' OWN CALLS, not re-derived: the command runs the same Judge, so the card cannot light a refusal.
	Out.Spot = JudgeSpot(SpotLitres);
	const bool bRunning = Contract.Tier != INDEX_NONE && Figures.ContractTiers.IsValidIndex(Contract.Tier);
	if (bRunning)
	{
		const FFuelContractTier& Tier = Figures.ContractTiers[Contract.Tier];
		Out.ContractTier = Contract.Tier;
		Out.ContractDaysLeft = Contract.DaysLeft;
		Out.ContractLitresPerDay = Tier.LitresPerDay;
		Out.ContractDailyCost = Tier.LitresPerDay * Tier.PricePerLitre;
		Out.CancelCharge = CancelChargeOf(Contract);
	}
	Out.NextTier = Contract.Tier == INDEX_NONE ? 0 : Contract.Tier + 1;
	if (Figures.ContractTiers.IsValidIndex(Out.NextTier))
	{
		const FFuelContractTier& Next = Figures.ContractTiers[Out.NextTier];
		Out.NextLitresPerDay = Next.LitresPerDay;
		Out.NextDailyCost = Next.LitresPerDay * Next.PricePerLitre;
	}
	else
	{
		Out.NextTier = INDEX_NONE;
	}
	// JudgeContract asks AlreadyContracted FIRST, so a running contract reads that whatever NextTier is - one at a time.
	Out.Sign = JudgeContract(Out.NextTier);
	Out.Cancel = bRunning ? EFuelOrderRefusal::None : EFuelOrderRefusal::NoContract;
	return Out;
}

double UFuelSupply::CancelChargeOf(const FFuelContract& Of) const
{
	if (!Figures.ContractTiers.IsValidIndex(Of.Tier)) { return 0.0; }
	const FFuelContractTier& Tier = Figures.ContractTiers[Of.Tier];
	return Of.DaysLeft * Tier.LitresPerDay * Tier.PricePerLitre * Figures.CancelFraction;
}

EFuelOrderRefusal UFuelSupply::CancelContract(double Now)
{
	if (Contract.Tier == INDEX_NONE || !Figures.ContractTiers.IsValidIndex(Contract.Tier)) { return EFuelOrderRefusal::NoContract; }
	// ONE CHARGE: the quote's figure and the posted one are CancelChargeOf, so the card's tooltip is what the ledger takes.
	const double Charge = CancelChargeOf(Contract);
	if (Ledger != nullptr)
	{
		Ledger->Post(Now, ELedgerCategory::FuelPurchase, -Charge, NSLOCTEXT("Ledger", "FuelCancel", "Fuel contract cancelled"));
	}
	UE_LOG(LogAirportOps, Log, TEXT("Fuel: contract cancelled with %d day(s) left, charge %.0f"), Contract.DaysLeft, Charge);
	Contract = FFuelContract();
	return EFuelOrderRefusal::None;
}
