#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/JobBoard.h"
#include "Model/ServiceBid.h"
#include "Model/ServiceRolePolicy.h"
#include "Model/ServiceVehicle.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace ServiceBidTest
{
	/**
	 * The user's own figures (2026-09-28): the utility tow's 1,000 L trailer at 75 L/min and the
	 * bowser's 10,000 L at 200 L/min; UScenario::FuelVehicles carries the same.
	 */
	FServiceVehicleType Tow()
	{
		FServiceVehicleType Type;
		Type.TypeCode = TEXT("UTILITY");
		Type.Capacity = 1000.0;
		Type.RatePerMinute = 75.0;
		return Type;
	}

	FServiceVehicleType Bowser()
	{
		FServiceVehicleType Type;
		Type.TypeCode = TEXT("FUEL");
		Type.Capacity = 10000.0;
		Type.RatePerMinute = 200.0;
		return Type;
	}

	constexpr int32 Depot = 1;
	constexpr int32 StandA = 10;
	constexpr int32 StandB = 11;
	constexpr int32 StandC = 12;

	/** THREE GAME MINUTES between any two places - the figure the user's worked example assumed. */
	constexpr double Drive = 180.0;

	ServiceBid::FInput Input(const FServiceVehicleType& Type, const IServiceRolePolicy& Policy)
	{
		ServiceBid::FInput In;
		In.Type = &Type;
		In.Policy = &Policy;
		In.FacilityNode = Depot;
		In.NodeWhenFree = Depot;
		In.CargoWhenFree = Type.Capacity;
		In.DriveSeconds = [](int32 From, int32 To) { return From == To ? 0.0 : Drive; };
		return In;
	}

	/**
	 * A baggage-shaped role - a LOAD MOVED BETWEEN TWO PLACES - kept to the test module. It always
	 * goes via its facility (the hall, where outbound bags come from) and comes away empty. If the
	 * policy interface had been shaped around fuel, this could not be written against it.
	 */
	class FTransferPolicy final : public IServiceRolePolicy
	{
	public:
		virtual EServiceRole Role() const override { return EServiceRole::Baggage; }
		virtual bool NeedsPumpAtHome() const override { return false; }
		// THE FUEL FIGURE, so this test's arithmetic reads as it always did: a count of bags has no slack of its own to
		// argue, and the tolerance test below is the one that varies it.
		virtual double DoneWithin() const override { return 0.5; }
		virtual EServiceStep NextStep(double, const FServiceVehicleType&, double) const override { return EServiceStep::ViaFacility; }
		virtual double TripQuantity(double, const FServiceVehicleType& Type, double Owed) const override { return FMath::Min(Type.Capacity, Owed); }
		virtual double ServeSeconds(const FServiceVehicleType&, double) const override { return 300.0; }
		virtual double CargoAfterServe(double, double) const override { return 0.0; }
		virtual double FacilitySeconds(double, const FServiceVehicleType&, int32, double) const override { return 60.0; }
		virtual double CargoAfterFacility(double, const FServiceVehicleType& Type, double) const override { return Type.Capacity; }
	};

	/** A role whose "done" has a wide slack (50 units) - what proves the bid reads the POLICY's DoneWithin, not a number
	 *  of its own. Goes straight to every job with a full load. */
	class FSlackPolicy final : public IServiceRolePolicy
	{
	public:
		virtual EServiceRole Role() const override { return EServiceRole::Baggage; }
		virtual bool NeedsPumpAtHome() const override { return false; }
		virtual double DoneWithin() const override { return 50.0; }
		virtual EServiceStep NextStep(double, const FServiceVehicleType&, double) const override { return EServiceStep::Direct; }
		virtual double TripQuantity(double, const FServiceVehicleType& Type, double Owed) const override { return FMath::Min(Type.Capacity, Owed); }
		virtual double ServeSeconds(const FServiceVehicleType&, double) const override { return 10.0; }
		virtual double CargoAfterServe(double Cargo, double Quantity) const override { return FMath::Max(Cargo - Quantity, 0.0); }
		virtual double FacilitySeconds(double, const FServiceVehicleType&, int32, double) const override { return 0.0; }
		virtual double CargoAfterFacility(double, const FServiceVehicleType& Type, double) const override { return Type.Capacity; }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FServiceBidBusyBowserBeatsIdleTowTest, "AirportOps.Service.Bid.BusyBowserBeatsIdleTow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceBidBusyBowserBeatsIdleTowTest::RunTest(const FString& Parameters)
{
	using namespace ServiceBidTest;
	// THE USER'S FIRST EXAMPLE: a 4,000 L job, an idle utility tow and a bowser five minutes from
	// finishing elsewhere. "You wouldn't want a utility taking it because it's free, as the bowser
	// could fulfil it much quicker." Free-first or nearest-first both send the tow.
	FFuelRolePolicy Policy;
	const FServiceVehicleType TowType = Tow();
	const FServiceVehicleType BowserType = Bowser();

	ServiceBid::FInput TowIn = Input(TowType, Policy);
	TowIn.Appended = { StandA, 4000.0 };
	const ServiceBid::FResult TowBid = ServiceBid::Finish(TowIn);

	ServiceBid::FInput BowserIn = Input(BowserType, Policy);
	BowserIn.FreeAt = 300.0;
	BowserIn.NodeWhenFree = StandB;
	BowserIn.CargoWhenFree = 9000.0;
	BowserIn.Appended = { StandA, 4000.0 };
	const ServiceBid::FResult BowserBid = ServiceBid::Finish(BowserIn);

	AddInfo(FString::Printf(TEXT("tow finishes at %.0f s, bowser at %.0f s"), TowBid.Finish, BowserBid.Finish));
	TestTrue(TEXT("the busy bowser finishes first"), BowserBid.Finish < TowBid.Finish);
	TestEqual(TEXT("the tow would need four trips"), TowBid.Trips, 4);
	TestEqual(TEXT("and three refills between them - the cost the bid exists to see"), TowBid.FacilityVisits, 3);
	TestEqual(TEXT("the bowser needs none"), BowserBid.FacilityVisits, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FServiceBidStockSpentOnceTest, "AirportOps.Service.Bid.StockIsSpentOnceAcrossTrips",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceBidStockSpentOnceTest::RunTest(const FString& Parameters)
{
	using namespace ServiceBidTest;
	// A MULTI-TRIP BID SPENDS THE STOCK ONCE (spec 2026-10-02 §7): an empty tow, a 2,000 L job and 1,500 L in the airport.
	// The first refill takes 1,000 and leaves 500; the second can take only that. A simulation that priced every refill
	// against the whole snapshot would promise two full loads and finish the job in two trips - fuel the airport has not got.
	FFuelRolePolicy Policy;
	const FServiceVehicleType TowType = Tow();

	ServiceBid::FInput Short = Input(TowType, Policy);
	Short.CargoWhenFree = 0.0;
	Short.FacilityAvailable = 1500.0;
	Short.Appended = { StandA, 2000.0 };
	const ServiceBid::FResult ShortBid = ServiceBid::Finish(Short);

	ServiceBid::FInput Plenty = Short;
	Plenty.FacilityAvailable = TNumericLimits<double>::Max();
	const ServiceBid::FResult PlentyBid = ServiceBid::Finish(Plenty);

	AddInfo(FString::Printf(TEXT("short: %d trips, %d refills, %.0f s; plenty: %d trips, %d refills, %.0f s"),
		ShortBid.Trips, ShortBid.FacilityVisits, ShortBid.Finish, PlentyBid.Trips, PlentyBid.FacilityVisits, PlentyBid.Finish));
	TestEqual(TEXT("CONTROL: with the stock unbounded two full loads finish it"), PlentyBid.Trips, 2);
	TestEqual(TEXT("short: the second load was the 500 L left, so a third trip is attempted"), ShortBid.Trips, 3);
	TestTrue(TEXT("and the bid does not pretend the second refill was a full one"), ShortBid.Finish != PlentyBid.Finish);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FServiceBidQueueDepthDecidesTest, "AirportOps.Service.Bid.QueueDepthDecides",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceBidQueueDepthDecidesTest::RunTest(const FString& Parameters)
{
	using namespace ServiceBidTest;
	// THE USER'S QUEUE ADDENDUM: "the utility could still win even if it has to service 1 more
	// vehicle ahead of the bidding job but probably not if it already has 2 queued." A 600 L job; the
	// bowser is twenty minutes from free. 300 L QUEUED JOBS, not the 500 L the chat's example used:
	// 500 + 600 does not fit a 1,000 L tank, so that example's "one ahead, no refill" was wrong.
	FFuelRolePolicy Policy;
	const FServiceVehicleType TowType = Tow();
	const FServiceVehicleType BowserType = Bowser();

	ServiceBid::FInput BowserIn = Input(BowserType, Policy);
	BowserIn.FreeAt = 1200.0;
	BowserIn.NodeWhenFree = StandB;
	BowserIn.CargoWhenFree = 9000.0;
	BowserIn.Appended = { StandC, 600.0 };
	const ServiceBid::FResult BowserBid = ServiceBid::Finish(BowserIn);

	ServiceBid::FInput OneAhead = Input(TowType, Policy);
	OneAhead.Queued = { { StandA, 300.0 } };
	OneAhead.Appended = { StandC, 600.0 };
	const ServiceBid::FResult OneBid = ServiceBid::Finish(OneAhead);

	ServiceBid::FInput TwoAhead = Input(TowType, Policy);
	TwoAhead.Queued = { { StandA, 300.0 }, { StandB, 300.0 } };
	TwoAhead.Appended = { StandC, 600.0 };
	const ServiceBid::FResult TwoBid = ServiceBid::Finish(TwoAhead);

	AddInfo(FString::Printf(TEXT("tow one ahead %.0f s, bowser %.0f s, tow two ahead %.0f s"),
		OneBid.Finish, BowserBid.Finish, TwoBid.Finish));
	TestTrue(TEXT("with one job ahead the tow still beats the busy bowser"), OneBid.Finish < BowserBid.Finish);
	TestTrue(TEXT("with two ahead it does not"), BowserBid.Finish < TwoBid.Finish);
	TestEqual(TEXT("one ahead: 300 + 600 fits the tank, no refill"), OneBid.FacilityVisits, 0);
	TestEqual(TEXT("two ahead: the third job waits on a refill - that is WHY it loses"), TwoBid.FacilityVisits, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FServiceBidFullTankIsDirectTest, "AirportOps.Service.Bid.FullTankIsDirect",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceBidFullTankIsDirectTest::RunTest(const FString& Parameters)
{
	using namespace ServiceBidTest;
	// A JOB NO TANK COVERS: a full tank goes straight there. Refilling a full tank first would add a
	// visit that delivers nothing - the refill count below would be four, not three.
	FFuelRolePolicy Policy;
	const FServiceVehicleType TowType = Tow();

	TestEqual(TEXT("a full tank short of the job still goes direct"),
		static_cast<int32>(Policy.NextStep(1000.0, TowType, 4000.0)), static_cast<int32>(EServiceStep::Direct));
	TestEqual(TEXT("a part tank short of the job refills first"),
		static_cast<int32>(Policy.NextStep(400.0, TowType, 600.0)), static_cast<int32>(EServiceStep::ViaFacility));
	TestEqual(TEXT("a part tank that covers it goes direct"),
		static_cast<int32>(Policy.NextStep(700.0, TowType, 600.0)), static_cast<int32>(EServiceStep::Direct));

	ServiceBid::FInput In = Input(TowType, Policy);
	In.Appended = { StandA, 4000.0 };
	const ServiceBid::FResult Bid = ServiceBid::Finish(In);
	TestEqual(TEXT("four trips"), Bid.Trips, 4);
	TestEqual(TEXT("three refills, not four"), Bid.FacilityVisits, 3);

	// AND THE ARITHMETIC: drive out, pump 1000 L at 75 L/min, then three rounds of (drive back,
	// refill 1000 L at 500 L/min, drive out, pump).
	const double Pump = 1000.0 / 75.0 * 60.0;
	const double Refill = 1000.0 / 500.0 * 60.0;
	TestEqual(TEXT("the finish is the sum of its legs"), Bid.Finish, Drive + Pump + 3.0 * (Drive + Refill + Drive + Pump), 0.01);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FServiceBidTransferPolicyTest, "AirportOps.Service.Bid.TransferPolicyAlwaysVisits",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceBidTransferPolicyTest::RunTest(const FString& Parameters)
{
	using namespace ServiceBidTest;
	// THE SAME SIMULATION, A DIFFERENT ROLE: a cart of 100 for a 250 hold visits the hall before
	// every trip. Nothing in ServiceBid names fuel; if it did, this would not come out at three.
	FTransferPolicy Policy;
	FServiceVehicleType Cart;
	Cart.TypeCode = TEXT("CART");
	Cart.Role = EServiceRole::Baggage;
	Cart.Capacity = 100.0;
	Cart.RatePerMinute = 1.0;

	ServiceBid::FInput In = Input(Cart, Policy);
	In.NodeWhenFree = StandB;   // out on the apron, so even the first trip drives to the hall
	In.Appended = { StandA, 250.0 };
	const ServiceBid::FResult Bid = ServiceBid::Finish(In);
	TestEqual(TEXT("three trips"), Bid.Trips, 3);
	TestEqual(TEXT("a hall visit before each"), Bid.FacilityVisits, 3);
	TestEqual(TEXT("each trip: to the hall, 60 s there, to the stand, 300 s serving"),
		Bid.Finish, 3.0 * (Drive + 60.0 + Drive + 300.0), 0.01);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FServiceBidToleranceTest, "AirportOps.Service.Policy.OneToleranceForBidAndVehicle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceBidToleranceTest::RunTest(const FString& Parameters)
{
	using namespace ServiceBidTest;
	// #443: THE HALF-LITRE "FUELLED" SLACK WAS TYPED FIVE TIMES IN THREE FILES, and changing one made the bid price a
	// different number of trips than the truck makes. It is the policy's DoneWithin now, and each reader is pinned to it:
	// the vehicle's NextStep, the bid's simulation, the job's outcome.
	FFuelRolePolicy Policy;
	const FServiceVehicleType TowType = Tow();
	TestEqual(TEXT("the policy answers the one figure"), Policy.DoneWithin(), FFuelRolePolicy::FuelledWithinLitres, 1e-12);
	const double Slack = Policy.DoneWithin();

	// THE VEHICLE: a tank within the slack of covering the job goes direct, one beyond it refills first; a tank within the
	// slack of full is full.
	TestEqual(TEXT("a tank exactly the slack short of the job covers it"),
		static_cast<int32>(Policy.NextStep(600.0 - Slack, TowType, 600.0)), static_cast<int32>(EServiceStep::Direct));
	TestEqual(TEXT("a tank a hair more short of it refills first"),
		static_cast<int32>(Policy.NextStep(600.0 - Slack - 0.01, TowType, 600.0)), static_cast<int32>(EServiceStep::ViaFacility));
	TestEqual(TEXT("a tank exactly the slack short of full is full: direct even for a job no tank covers"),
		static_cast<int32>(Policy.NextStep(1000.0 - Slack, TowType, 4000.0)), static_cast<int32>(EServiceStep::Direct));
	TestEqual(TEXT("a hair less is not"),
		static_cast<int32>(Policy.NextStep(1000.0 - Slack - 0.01, TowType, 4000.0)), static_cast<int32>(EServiceStep::ViaFacility));

	// THE JOB'S OUTCOME, decided by the same figure with no policy in hand (a departing aircraft).
	TestEqual(TEXT("delivered exactly the slack short of wanted is fuelled"),
		static_cast<int32>(UJobBoard::FuelOutcomeOf(1000.0 - Slack, 1000.0)), static_cast<int32>(EFuelOutcome::Fuelled));
	TestEqual(TEXT("a hair more short is part-fuelled"),
		static_cast<int32>(UJobBoard::FuelOutcomeOf(1000.0 - Slack - 0.01, 1000.0)), static_cast<int32>(EFuelOutcome::PartFuelled));

	// THE BID reads the POLICY'S figure and not a number of its own: a role whose slack is 50 calls a 30-unit remainder done,
	// so a 60-unit job in 30-unit trips is ONE trip - with the old literal half-unit it was two.
	FSlackPolicy Coarse;
	FServiceVehicleType Cart;
	Cart.TypeCode = TEXT("CART");
	Cart.Role = EServiceRole::Baggage;
	Cart.Capacity = 30.0;
	Cart.RatePerMinute = 1.0;
	ServiceBid::FInput In = Input(Cart, Coarse);
	In.Appended = { StandA, 60.0 };
	TestEqual(TEXT("a 60 job in 30 trips, done within 50: one trip"), ServiceBid::Finish(In).Trips, 1);
	In.Appended = { StandA, 30.0 };
	TestEqual(TEXT("a 30 job, already done within 50: no trip at all"), ServiceBid::Finish(In).Trips, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FServiceBidUnreachableTest, "AirportOps.Service.Bid.UnreachableIsFlagged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceBidUnreachableTest::RunTest(const FString& Parameters)
{
	using namespace ServiceBidTest;
	FFuelRolePolicy Policy;
	const FServiceVehicleType TowType = Tow();
	ServiceBid::FInput In = Input(TowType, Policy);
	In.DriveSeconds = [](int32 From, int32 To) { return From == To ? 0.0 : -1.0; };
	In.Appended = { StandA, 300.0 };
	TestFalse(TEXT("no way to the stand is not a finish time"), ServiceBid::Finish(In).bReachable);
	return true;
}

#endif
