#pragma once

#include "CoreMinimal.h"
#include "Model/OpsDefinition.h"
#include "Model/OpsSave.h"
#include "Model/Ledger.h"
#include "FuelSupply.generated.h"

/** Why a fuel order was refused. None is the only success. */
enum class EFuelOrderRefusal : uint8 { None, NoRoom, CannotAfford, UnknownTier, AlreadyContracted, NoContract };

/** The running contract: which tier, and how much of the term is left. Tier INDEX_NONE = none. Saved. */
USTRUCT()
struct AIRPORTOPS_API FFuelContract
{
	GENERATED_BODY()
	UPROPERTY() int32 Tier = INDEX_NONE;
	UPROPERTY() int32 DaysLeft = 0;
};

/** A spot order paid for and on its way. Saved: a load mid-delay must still deliver. */
USTRUCT()
struct AIRPORTOPS_API FFuelSpotOrder
{
	GENERATED_BODY()
	UPROPERTY() double Litres = 0.0;
	UPROPERTY() double DueAt = 0.0;
};

/**
 * EVERYTHING THE DEPOT CARD'S FUEL ROW SHOWS, AND NOTHING IT COMPUTES - FFacilityQuote's rule (facility-upgrades spec §3: the UI
 * renders only the quote), for the supply. The card's three buttons and the three BuildActions rows behind them read their
 * refusals from HERE, so a button cannot be lit for an order the rules would refuse. A PLAIN STRUCT, rebuilt on every ask; ==
 * defaulted, so the inspector's card key can hold one whole and a field added here is compared without anyone remembering to.
 * ENFORCED BY: AirportOps.Model.FuelSupply.QuoteIsTheJudges
 */
struct FFuelQuote
{
	double StockLitres = 0.0;
	/** Capacity(); bBounded false while no capacity reader is wired (unbounded - a bare test's supply). */
	double CapacityLitres = 0.0;
	bool bBounded = false;
	double PendingLitres = 0.0;

	/** The running contract - Tier INDEX_NONE for none - and its day as the tier prices it. */
	int32 ContractTier = INDEX_NONE;
	int32 ContractDaysLeft = 0;
	double ContractLitresPerDay = 0.0;
	double ContractDailyCost = 0.0;
	/** What a cancel would charge now: CancelContract's own figure. */
	double CancelCharge = 0.0;

	/** The spot order the card offers: its litres, what it costs now, and how long it takes. */
	double SpotLitres = 0.0;
	double SpotCost = 0.0;
	double SpotDelaySeconds = 0.0;

	/** The tier the card's Sign button would sign (see Quote), its day, and the term it signs for; INDEX_NONE past the last. */
	int32 NextTier = INDEX_NONE;
	double NextLitresPerDay = 0.0;
	double NextDailyCost = 0.0;
	int32 TermDays = 0;

	/** The three verbs' verdicts: JudgeSpot, JudgeContract(NextTier), and NoContract with nothing to cancel. */
	EFuelOrderRefusal Spot = EFuelOrderRefusal::None;
	EFuelOrderRefusal Sign = EFuelOrderRefusal::None;
	EFuelOrderRefusal Cancel = EFuelOrderRefusal::None;

	bool operator==(const FFuelQuote& Other) const = default;
};

/**
 * The airport's fuel: what it holds, what it has contracted, what it has ordered (spec 2026-10-02-progression-and-
 * fuel-supply §7). Before this the depot held unlimited fuel for free.
 *
 * ONE POOL FOR THE AIRPORT, not one per depot - a named deviation from the spec's "the depot holds litres". Per-depot
 * stock would need a rule for which depot a delivery fills, and the player could not see it; every vehicle already
 * refills only at its own Home, so a shared pool changes nothing about where trucks drive.
 *
 * CAPACITY IS ASKED, NEVER STORED: CapacityOf reads the seated tanks every time (UJobBoard::FuelCapacityLitres via
 * UOpsRuntime). A stored capacity would be a second source of truth about what is built, and the mutator that forgot
 * it - a bulldozed depot, an undo - would leave a phantom tank.
 */
UCLASS()
class AIRPORTOPS_API UFuelSupply : public UObject, public IOpsPersistent
{
	GENERATED_BODY()

public:
	virtual FName SaveBlobName() const override { return TEXT("FuelSupply"); }
	virtual UObject& AsPersistentObject() override { return *this; }
	/**
	 * BACK TO THE CLASS DEFAULTS BEFORE ANY RESTORE, blob or none (UAirport::OnBeforeRestore's shape): tagged serialisation
	 * writes no property equal to its default, so a save holding an empty tank, no orders and no contract carries none of
	 * them, and the restore left this session's stock, order and contract standing. A snapshot from before fuel had a blob
	 * loads dry, which no player save can be (2026-10-03: there are none).
	 * ENFORCED BY: AirportOps.Model.FuelSupply.RestoreClearsTheSession, AirportOps.Present.Fuel.LoadReplacesTheSessionsFuel
	 */
	virtual void OnBeforeRestore() override;

	/** Litres held. May exceed Capacity() after a tank is sold - see FreeSpace. */
	UPROPERTY() double StockLitres = 0.0;

	/** Design figures, from the scenario. Transient - see UPricing for why figures are never saved. */
	UPROPERTY(Transient) FFuelSupplyFigures Figures;

	/** Seated tank capacity in litres. Unset: unbounded, for a test that does not care. */
	TFunction<double()> CapacityOf;

	double Capacity() const;
	double Available() const { return FMath::Max(StockLitres, 0.0); }
	/** Room for a delivery; zero, never negative, while the stock is above a capacity that shrank. */
	double FreeSpace() const { return FMath::Max(Capacity() - StockLitres, 0.0); }
	/**
	 * What the FuelLow alert asks: tanks seated, nothing contracted, and stock plus spot orders on the way under a quarter of
	 * capacity. A question the supply answers rather than OpsAlerts re-deriving from four of its members (and Check-Architecture's
	 * 'vehicle row capacity' rule, which reads any `->Capacity` outside two files as a vehicle row's, would otherwise fire).
	 */
	bool IsLow() const;
	/** Below the half-litre a fuel job is judged done within - FFuelRolePolicy::FuelledWithinLitres, the ONE tolerance. */
	bool IsDry() const;

	/** Takes up to Litres from the stock; returns what was granted. */
	double Draw(double Litres);
	/** The running contract, if any. Saved. */
	UPROPERTY() FFuelContract Contract;
	/** Spot orders paid and not yet delivered. Saved. */
	UPROPERTY() TArray<FFuelSpotOrder> SpotOrders;

	/** The books fuel is paid from. Transient, set by the runtime; null (a bare test) buys for free. */
	UPROPERTY(Transient) TObjectPtr<ULedger> Ledger;
	/** Set and cleared with the runtime's other publishers (UOpsRuntime::Publishers). Null in a bare NewObject. */
	class FOpsEventBus* Bus = nullptr;

	double PendingSpotLitres() const;
	EFuelOrderRefusal JudgeSpot(double Litres) const;
	EFuelOrderRefusal OrderSpot(double Litres, double Now);
	int32 ReceiveDueSpot(double Now);

	EFuelOrderRefusal JudgeContract(int32 Tier) const;
	EFuelOrderRefusal SignContract(int32 Tier, double Now);
	EFuelOrderRefusal CancelContract(double Now);
	/** One day of the contract: charged whole (take-or-pay), added as far as the tanks allow. Called at day end. */
	void DeliverContractDay(double Now);

	/** Adds up to Litres, stopping at capacity; returns what was added. TAKE-OR-PAY: the caller has already paid. */
	double Receive(double Litres);

	/**
	 * The card's row, with a spot order of SpotLitres on offer - see FFuelQuote. THE NEXT TIER is the one after the running
	 * contract, the smallest while none runs; since one contract runs at a time (JudgeContract's AlreadyContracted), a running one
	 * makes the Sign verdict AlreadyContracted whatever the tier, and the card's Sign is in practice the smallest tier. Larger
	 * tiers wait for a tier picker (2026-10-03: the brief's ruling, said in the task report).
	 */
	FFuelQuote Quote(double SpotLitres) const;

private:
	/** What cancelling Of would charge: the days left at the tier's daily cost, times CancelFraction. Quote and CancelContract both. */
	double CancelChargeOf(const FFuelContract& Of) const;
};
