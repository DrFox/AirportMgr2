#pragma once

#include "CoreMinimal.h"

#include "OfferViewModels.generated.h"

class UAirlineRoster;
class UFlight;
struct FAirlineStanding;
class UFlightBoard;
class UGroundTraffic;
class URoadNetwork;
class USimClock;
struct FAirlineOffers;
class UAirlineDefinition;
class UPricing;

/**
 * EVERY INPUT of how an airline's mood is worded (#446): the percentage (rounded the way it prints), whether there is a newest change to
 * name, which way it went, and why. DescribeMood is a function of THIS and nothing else, so a row that memoises its mood sentence on the
 * key cannot part from the sentence - there is no second rounding to disagree with.
 */
struct FOfferMoodKey
{
	bool bStanding = false;
	int32 Percent = -1;
	bool bRecent = false;
	bool bUp = false;
	FString Cause;

	bool operator==(const FOfferMoodKey& Other) const = default;
};

/**
 * EVERY INPUT of the sentences a flight fixes at the offer - who is asking, what it pays, what it wants, the contract - which used to be
 * composed again every tick for every row (about a dozen FText::Format a row). They move only if the flight's own figures or the pricing
 * that words the fee do, and this key is those. The callsign, the airline's name and the type are not figures but the flight's IDENTITY - fixed
 * at the offer - so they ride on the Flight pointer: a row bound to another flight differs in the key and recomposes them.
 */
struct FOfferFixedKey
{
	TWeakObjectPtr<const UFlight> Flight;
	TWeakObjectPtr<const UPricing> Pricing;
	double LeadTimeSeconds = -1.0;
	double ContractSeconds = -1.0;
	double LandingFee = -1.0;
	double FuelLitres = -1.0;

	bool operator==(const FOfferFixedKey& Other) const = default;
};

/**
 * One row of the offer inbox.
 *
 * A VIEWMODEL, NOT THE MODEL. UFlight lives in AirportOps Model/ and must not learn about
 * the UI, for the same reason Airside Model/ includes nothing above it: a model that knows
 * about its view cannot be tested without one. This flattens a flight into the handful of
 * display fields a row draws.
 *
 * PLAIN UObject, NOT UMVVMViewModelBase (issue #191 dropped the base). This used to derive
 * from it and set every field through UE_MVVM_SET_PROPERTY_VALUE so a Blueprint binding
 * would hear about the change - but no Content/UI Blueprint ever bound one, so every
 * broadcast reached zero subscribers, and ModelViewViewModel was a Build.cs dependency for
 * a pattern used nowhere (CLAUDE.md: name a pattern and justify it at its site, or drop it).
 * OfferInboxWidget::PaintRows already polls these getters directly every refresh, which is
 * the actual, working design - see its own header.
 */
UCLASS(BlueprintType)
class AIRPORTMGR_API UOfferViewModel : public UObject
{
	GENERATED_BODY()

public:
	/** The flight this row shows. WEAK: the board owns flights and retires them. */
	UPROPERTY(Transient) TWeakObjectPtr<UFlight> Flight;

	/**
	 * Pull every field from the flight and the board's verdict - and, given the roster, how its
	 * airline feels (null: no satisfaction line, as in a test that does not care).
	 *
	 * EACH SENTENCE IS COMPOSED ONLY WHEN ITS INPUTS MOVED (#446): the fixed ones on FOfferFixedKey, the mood on FOfferMoodKey, the refusal and
	 * the two verdict flags on themselves. What still changes every call is a number - the seconds left and the bar's fraction - which is
	 * not composed. True when a sentence or a verdict flag changed, i.e. when GetRevision moved.
	 */
	bool Refresh(const UFlightBoard& Board, const UGroundTraffic& Traffic,
		const URoadNetwork& Network, const USimClock& Clock, const UAirlineRoster* Airlines = nullptr);

	/**
	 * The stamp of the last Refresh that changed a sentence or a verdict flag - UNIQUE ACROSS ROWS (one counter for all of them), so a panel
	 * that remembers the stamp it painted in a SLOT repaints when a different row lands there as well as when its own row changes. 0 before
	 * the first. The panel paints a row's texts when this has moved, and its seconds-left when THAT has (GetSecondsLeft).
	 */
	int32 GetRevision() const { return Revision; }

	FText GetCallsign() const { return Callsign; }
	FText GetAirline() const { return Airline; }
	FText GetTypeName() const { return TypeName; }
	FText GetFee() const { return Fee; }
	FText GetContract() const { return Contract; }
	bool IsAcceptable() const { return bAcceptable; }
	FText GetRefusal() const { return Refusal; }
	bool IsFuelServable() const { return bFuelServable; }
	/** "Fuel 2,900 L", or empty for a flight that wants none. */
	FText GetFuelText() const { return FuelText; }
	bool NeedsTug() const { return bNeedsTug; }
	int32 GetSecondsLeft() const { return SecondsLeft; }
	float GetTimeLeftFraction() const { return TimeLeftFraction; }
	FText GetAcceptLabel() const { return AcceptLabel; }

	/** "62% \u25BC late departure (25 min)" - see DescribeSatisfaction. Empty with no roster. */
	FText GetSatisfaction() const { return Satisfaction; }

	/**
	 * How an airline feels and the latest reason why: the percentage, an arrow for which way the
	 * newest change went, and its cause. Just the percentage for an airline nothing has moved yet;
	 * empty for none (the debug flight's, or no roster). Static so a test can ask it of a row.
	 */
	static FText DescribeSatisfaction(const FAirlineStanding* Standing);

	/** Standing reduced to what DescribeMood reads - the row's memo key. */
	static FOfferMoodKey MoodKeyOf(const FAirlineStanding* Standing);

	/** The mood sentence from its key: DescribeSatisfaction's body, so the key and the sentence are one function's input and output. */
	static FText DescribeMood(const FOfferMoodKey& Mood);

	/**
	 * "lands in 15 min - airborne within 1 h 10 min", from the flight's lead time and contract.
	 * GAME time, in the clock's own words (GameTimeText::Duration, which the arrivals rows, the cards and the depot's backlog
	 * share - DescribeDuration lived here until #447): the player reads the clock, not a seconds count.
	 * Static so a test can ask it of numbers.
	 */
	static FText DescribeContract(double LeadTimeSeconds, double ContractSeconds);

private:
	UPROPERTY(Transient) FText Callsign;
	UPROPERTY(Transient) FText Airline;
	UPROPERTY(Transient) FText TypeName;

	/** The landing fee, fixed at the offer and formatted by UPricing - what accepting is worth. */
	UPROPERTY(Transient) FText Fee;

	/** See DescribeContract. */
	UPROPERTY(Transient) FText Contract;

	UPROPERTY(Transient) bool bAcceptable = true;

	/**
	 * Why not, in the words ArrivalPlanner::DescribeRefusal uses.
	 *
	 * The SAME sentence the arrival itself would print, because it comes from the same plan -
	 * a second wording here would be a second account of why an aeroplane cannot land.
	 */
	UPROPERTY(Transient) FText Refusal;

	/** See GetFuelText. */
	UPROPERTY(Transient) FText FuelText;

	/** The fuel chip: can the airport fuel it? See FOfferVerdict::bFuelServable. */
	UPROPERTY(Transient) bool bFuelServable = true;

	/** The tug chip, information only - no tug service exists to check against yet. */
	UPROPERTY(Transient) bool bNeedsTug = false;

	/** REAL seconds left, rounded up, and as a fraction of the airline's window, for the bar. */
	UPROPERTY(Transient) int32 SecondsLeft = 0;
	UPROPERTY(Transient) float TimeLeftFraction = 1.0f;

	/**
	 * "Accept", or "Accept (no fuel)" - the cost of a soft demand, on the button that incurs it.
	 */
	UPROPERTY(Transient) FText AcceptLabel;

	/** See GetSatisfaction. */
	UPROPERTY(Transient) FText Satisfaction;

	/** What each memoised group of sentences was last composed from, and whether it ever was. Not saved. */
	FOfferFixedKey FixedKey;
	bool bFixedValid = false;
	FOfferMoodKey MoodKey;
	bool bMoodValid = false;
	/** The refusal's source sentence - the plan's own words - and whether the verdict fields below were ever set from a quote. */
	FString RefusalSentence;
	bool bVerdictValid = false;
	int32 Revision = 0;
};

/**
 * The inbox: the offers awaiting an answer, and the two commands.
 *
 * NOTHING HERE MUTATES THE MODEL. Accept and Decline call UFlightBoard, which is the one
 * door for undo, save and tests - the rule URoadEditFacade already enforces on the build
 * side. A viewmodel that reached into a UFlight would be a second way to change the game.
 *
 * PLAIN UObject (issue #191 dropped UMVVMViewModelBase) - see UOfferViewModel's header for
 * why: nothing ever bound a field on this class either.
 */
UCLASS(BlueprintType)
class AIRPORTMGR_API UOfferInboxViewModel : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Rebuild the rows from the board.
	 *
	 * CALLED ON TICK (issue #169 revised this from its old claim of "and on
	 * UFlightBoard::OnChanged" - that delegate had fired from nearly every board method for
	 * years with zero subscribers; see UFlightBoard::Revision, which replaces it). The ROW
	 * SET - which flights have an offer at all - is rebuilt only when UFlightBoard::Revision
	 * has moved since the last call: Board.Offers() allocates a fresh array on every call,
	 * and diffing it against Rows was the same cost again for a tick where nothing happened.
	 * Each row's OWN fields still refresh every call, and each row composes only what its inputs moved (UOfferViewModel::Refresh: the flight's
	 * fixed figures, the airline's mood, the verdict; the seconds left are a number and are not composed) - bookkeeping about the OFFER SET is
	 * cheap to gate here, but each row already knows how to gate what is actually expensive.
	 */
	void Refresh(UFlightBoard& Board, UGroundTraffic& Traffic, const URoadNetwork& Network,
		const USimClock& Clock, const UAirlineRoster* Airlines = nullptr);

	/**
	 * Refresh's two halves, apart so a FOLDED window can keep the first (#446). SyncRows: the row SET (rebuilt only when the board's
	 * revision moved), the pending count and the cap - what the window's title-bar badge reads - and the handles Accept and Decline
	 * refresh through. RefreshRows: every row's fields, each composing only what moved. Refresh is both, in that order.
	 */
	void SyncRows(UFlightBoard& Board, UGroundTraffic& Traffic, const URoadNetwork& Network,
		const USimClock& Clock, const UAirlineRoster* Airlines = nullptr);
	void RefreshRows(UFlightBoard& Board, UGroundTraffic& Traffic, const URoadNetwork& Network,
		const USimClock& Clock, const UAirlineRoster* Airlines = nullptr);

	/** How many row Refreshes have changed a sentence or a verdict flag, in total - a delta across ticks is what a pin reads. */
	int32 ComposeCountForTest() const { return Composes; }

	/**
	 * The rows, as the raw pointers Blueprint wants (and the inbox's UListView path, removed in #447, did). Built ON DEMAND from
	 * Rows (below) every call rather than kept as a second stored array (issue #191): Rows
	 * and a hand-mirrored Offers used to have to agree at every place either changed, which
	 * is CLAUDE.md's "lists that must agree are one list" - a caller that touched one and
	 * forgot the other is exactly the bug that rule exists to rule out. The inbox is a
	 * handful of entries, so the copy costs nothing next to what Refresh already gates.
	 */
	UFUNCTION(BlueprintCallable, Category = "Offers")
	TArray<UOfferViewModel*> GetOffers() const;

	/** The inbox cap - the generator's MaxPendingOffers, for the header's "3/8". */
	int32 GetCapacity() const { return Capacity; }

	/**
	 * The demand strip: Count samples of UOfferGenerator::TotalRateAt across the day, each at
	 * its slot's midpoint. THE GENERATOR'S OWN FUNCTION, so the strip cannot draw a curve the
	 * offers do not follow. Static so a test can compare it with the generator directly. Each airline at
	 * AirlineFactorOf - the widget passes UOfferGenerator::AirlineFactor, the generator's own reader.
	 */
	static TArray<double> SampleDemand(TArrayView<const FAirlineOffers> Airlines,
		const USimClock& Clock, double DemandFactor, int32 Count,
		TFunctionRef<double(const UAirlineDefinition&)> AirlineFactorOf);

	int32 GetPendingCount() const { return PendingCount; }

	/** True if the board took it. False leaves the offer in the inbox with its reason shown. */
	bool Accept(UOfferViewModel* Row);
	void Decline(UOfferViewModel* Row);

private:
	/** The one list of rows. See GetOffers() above for why nothing else stores a second copy. */
	UPROPERTY(Transient) TArray<TObjectPtr<UOfferViewModel>> Rows;

	/** UFlightBoard::Revision as of the last time Rows was rebuilt from Board.Offers(); see
	 *  Refresh's own comment. Not a UPROPERTY: bookkeeping, not state a save would ever need. */
	uint32 BoardRevisionAt = 0;
	bool bRowsValid = false;

	/**
	 * Set with a plain assignment (issue #191 dropped UE_MVVM_SET_PROPERTY_VALUE): nothing
	 * ever subscribed to this field changing, so there was no binding to notify. The badge
	 * is repainted by OfferInboxWidget::PaintRows polling GetPendingCount() directly every
	 * refresh, which is the mechanism that actually keeps it current.
	 */
	UPROPERTY(BlueprintReadOnly, Transient, Getter = "GetPendingCount",
		Category = "Offers", meta = (AllowPrivateAccess))
	int32 PendingCount = 0;

	/** See GetCapacity. Read off the board's generator each refresh. */
	int32 Capacity = 0;

	/** See ComposeCountForTest. */
	int32 Composes = 0;

	/** What Accept and Decline call. Set by Refresh; weak for the usual lifetime reason. */
	UPROPERTY(Transient) TWeakObjectPtr<UFlightBoard> Board;
	UPROPERTY(Transient) TWeakObjectPtr<UGroundTraffic> Traffic;
	UPROPERTY(Transient) TWeakObjectPtr<URoadNetwork> Network;
	UPROPERTY(Transient) TWeakObjectPtr<USimClock> Clock;

	/** The roster the last Refresh was given, so Accept/Decline's own refresh keeps the rows'
	 *  satisfaction line rather than blanking it for a frame. Null when none was. */
	UPROPERTY(Transient) TWeakObjectPtr<const UAirlineRoster> Airlines;
};
