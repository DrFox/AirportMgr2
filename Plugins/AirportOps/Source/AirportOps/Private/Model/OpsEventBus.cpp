#include "Model/OpsEventBus.h"
#include "AirportOpsLog.h"

namespace
{
	template <typename TVariantType> struct TOpsBusEventNames;
	template <typename... Ts> struct TOpsBusEventNames<TVariant<Ts...>>
	{
		static TArray<const TCHAR*> Get() { return { Ts::EventName()... }; }
	};

	const TCHAR* OpsBusTierName(EOpsTier Tier)
	{
		switch (Tier)
		{
		case EOpsTier::Sim: return TEXT("Sim");
		case EOpsTier::Reaction: return TEXT("Reaction");
		default: return TEXT("Presentation");
		}
	}
}

FString FAgentPhaseEvent::Describe() const
{
	// THE CAUSE IS SAID TOO (#436): it is what a handler decides on, so a log that showed only the pair would hide the
	// one fact the event now carries.
	return FString::Printf(TEXT("agent %d, %s -> %s (%s)"), AgentId, *UEnum::GetValueAsString(From), *UEnum::GetValueAsString(To),
		*UEnum::GetValueAsString(Cause));
}

FString FArrivalRefusedEvent::Describe() const
{
	return UEnum::GetValueAsString(Why);
}

FString FSpeedChangedEvent::Describe() const
{
	return UEnum::GetValueAsString(Speed);
}

FString FNotificationEvent::Describe() const
{
	return FString::Printf(TEXT("\"%s\""), *Text);
}

FString FOfferExpiredEvent::Describe() const
{
	return FString::Printf(TEXT("flight %d, airline %s, %s%s"), FlightId, *AirlineId.ToString(),
		*UEnum::GetValueAsString(Reason), bFloorAirline ? TEXT(", floor airline") : TEXT(""));
}

FString FOfferDeclinedEvent::Describe() const
{
	return FString::Printf(TEXT("flight %d, airline %s"), FlightId, *AirlineId.ToString());
}

FString FFlightAirborneEvent::Describe() const
{
	return FString::Printf(TEXT("flight %d, airline %s, %+.0f s against its contract"), FlightId, *AirlineId.ToString(), LateBySeconds);
}

FString FDayEndedEvent::Describe() const
{
	return FString::Printf(TEXT("day %d"), Day);
}

FString FAirlineSatisfactionEvent::Describe() const
{
	return FString::Printf(TEXT("airline %s, %.2f -> %.2f, %s"), *AirlineId.ToString(), Old, New, *Cause);
}

FString FNetworkChangedEvent::Describe() const
{
	return FString::Printf(TEXT("guideline revision %u"), GuidelineRevision);
}

FString FAlertRaisedEvent::Describe() const
{
	return FString::Printf(TEXT("%s %d%s, \"%s\""), *UEnum::GetValueAsString(Alert.Key.Kind), Alert.Key.Id,
		Alert.Key.Name.IsNone() ? TEXT("") : *(TEXT(" ") + Alert.Key.Name.ToString()), *Alert.Text.ToString());
}

FString FAlertClearedEvent::Describe() const
{
	return FString::Printf(TEXT("%s %d%s"), *UEnum::GetValueAsString(Key.Kind), Key.Id,
		Key.Name.IsNone() ? TEXT("") : *(TEXT(" ") + Key.Name.ToString()));
}

FString FAlertChangedEvent::Describe() const
{
	return FString::Printf(TEXT("%s %d%s"), *UEnum::GetValueAsString(Key.Kind), Key.Id,
		Key.Name.IsNone() ? TEXT("") : *(TEXT(" ") + Key.Name.ToString()));
}

FString FAlertsResetEvent::Describe() const
{
	return TEXT("every alert forgotten");
}

FString FMoneyPostedEvent::Describe() const
{
	return FString::Printf(TEXT("entry %d, %s, %+.0f, balance %.0f"), EntryId, *UEnum::GetValueAsString(Category), Amount, Balance);
}

FString FBalanceSignChangedEvent::Describe() const
{
	return bOverdrawn ? TEXT("overdrawn") : TEXT("back in credit");
}

FString FBuildRefusedEvent::Describe() const
{
	return FString::Printf(TEXT("%s, cannot afford %s, balance %s"), *What, *Price, *Balance);
}

FString FLandRefusedEvent::Describe() const
{
	return Sentence.IsEmpty() ? UEnum::GetValueAsString(Why) : FString::Printf(TEXT("%s: %s"), *UEnum::GetValueAsString(Why), *Sentence);
}

FString FFacilityUpgradedEvent::Describe() const
{
	return FString::Printf(TEXT("depot %d, %s, %.0f"), Entity, *UEnum::GetValueAsString(Module), Amount);
}

FString FModulesRefundedEvent::Describe() const
{
	return FString::Printf(TEXT("depot %d, %d x %s removed, refunded %.0f"), Entity, Count, *UEnum::GetValueAsString(Module), Amount);
}

FString FFleetChangedEvent::Describe() const
{
	const TCHAR* Verb = TEXT("?");
	switch (Change)
	{
	case EFleetChange::Bought:    Verb = TEXT("bought"); break;
	case EFleetChange::Sold:      Verb = TEXT("sold"); break;
	case EFleetChange::Seeded:    Verb = TEXT("seeded"); break;
	case EFleetChange::Withdrawn: Verb = TEXT("withdrawn"); break;
	}
	return FString::Printf(TEXT("depot %d, vehicle %d %s, %s for %.0f"), Depot, VehicleId, *TypeCode.ToString(), Verb, Amount);
}

FString FOfferAcceptedEvent::Describe() const
{
	return FString::Printf(TEXT("flight %d, airline %s, stand %d"), FlightId, *AirlineId.ToString(), Stand.Index);
}

FString FTurnaroundEndedEvent::Describe() const
{
	return FString::Printf(TEXT("agent %d, stand %d, %s, %.0f of %.0f L"), AircraftAgentId, Stand.Index,
		*UEnum::GetValueAsString(Outcome), Delivered, Wanted);
}

FString FAirportStatusChangedEvent::Describe() const
{
	return FString::Printf(TEXT("%s -> %s"), *UEnum::GetValueAsString(Old), *UEnum::GetValueAsString(New));
}

FString FFlightCancelledEvent::Describe() const
{
	return FString::Printf(TEXT("flight %d, airline %s, %s"), FlightId, *AirlineId.ToString(), *UEnum::GetValueAsString(Reason));
}

FString FRunwayFreedEvent::Describe() const
{
	return FString::Printf(TEXT("runway seed %d"), Seed.Index);
}

FString FStandsFreedEvent::Describe() const
{
	FString Poses;
	for (const FGuidelineNodeId& Pose : PoseNodes)
	{
		Poses += (Poses.IsEmpty() ? TEXT("") : TEXT(", ")) + FString::FromInt(Pose.Index);
	}
	return FString::Printf(TEXT("%d stand(s), pose node(s) %s"), PoseNodes.Num(), *Poses);
}

FString FPushGroundFreedEvent::Describe() const
{
	return FString::Printf(TEXT("aircraft %d"), AgentId);
}

FString FFlightInboundEvent::Describe() const
{
	return FString::Printf(TEXT("flight %d, airline %s"), FlightId, *AirlineId.ToString());
}

FString FAirlineAdmissionChangedEvent::Describe() const
{
	return bCouldCome ? FString::Printf(TEXT("airline %s can use the airport"), *AirlineId.ToString())
		: FString::Printf(TEXT("airline %s cannot use the airport: %s"), *AirlineId.ToString(), *Reason);
}

FString FOpsEventBus::Describe(const FOpsEvent& Event)
{
	return Visit([](const auto& Each) { return Each.Describe(); }, Event);
}

TArray<const TCHAR*> FOpsEventBus::EventNames()
{
	return TOpsBusEventNames<FOpsEvent>::Get();
}

const TCHAR* FOpsEventBus::NameOf(const FOpsEvent& Event)
{
	return EventNames()[Event.GetIndex()];
}

void FOpsEventBus::LogSubscription(FName Who, EOpsTier Tier, const TCHAR* Event)
{
	UE_LOG(LogOpsBus, Log, TEXT("Bus: %s subscribes to %s (%s)"), *Who.ToString(), Event, OpsBusTierName(Tier));
}

void FOpsEventBus::BeginWiring() { check(!bDraining); bWiring = true; }

void FOpsEventBus::EndWiring()
{
	bWiring = false;
	OrderPasses();
}

void FOpsEventBus::OrderPasses()
{
	// A STABLE TOPOLOGICAL SORT by After (#445): each step takes the first pass, in registration order, whose dependencies are
	// all placed - so a pass with none declared keeps the place its registration gave it, and the declared orders are the only
	// ones that move anything. Kahn's walk over a handful of passes (5 on 2026-09-30), run once per wiring.
	TArray<FPass> Ordered;
	Ordered.Reserve(Passes.Num());
	TArray<FPass> Waiting = MoveTemp(Passes);
	Passes.Reset();
	while (Waiting.Num() > 0)
	{
		int32 Next = INDEX_NONE;
		for (int32 Index = 0; Index < Waiting.Num() && Next == INDEX_NONE; ++Index)
		{
			const bool bReady = !Waiting[Index].After.ContainsByPredicate([&Ordered](FName Dependency)
			{
				return !Ordered.ContainsByPredicate([Dependency](const FPass& Placed) { return Placed.Name == Dependency; });
			});
			Next = bReady ? Index : INDEX_NONE;
		}
		if (Next == INDEX_NONE)
		{
			// NOTHING IS READY: a dependency nobody registered, or a cycle. Said, with the passes it strands - a pass that never
			// runs is the silent failure this ordering exists to remove - and the rest kept in registration order so the
			// airport still runs while it is read.
			for (const FPass& Stranded : Waiting)
			{
				UE_LOG(LogOpsBus, Error, TEXT("Bus: pass %s cannot be ordered: it runs After a pass that is not registered, or the order is a cycle"),
					*Stranded.Name.ToString());
			}
			Ordered.Append(MoveTemp(Waiting));
			break;
		}
		Ordered.Add(MoveTemp(Waiting[Next]));
		Waiting.RemoveAt(Next);
	}
	Passes = MoveTemp(Ordered);
	FString Order;
	for (const FPass& Each : Passes) { Order += (Order.IsEmpty() ? TEXT("") : TEXT(", ")) + Each.Name.ToString(); }
	UE_LOG(LogOpsBus, Log, TEXT("Bus: passes run in this order: %s"), *Order);
}

TArray<FName> FOpsEventBus::PassOrder() const
{
	TArray<FName> Out;
	for (const FPass& Each : Passes) { Out.Add(Each.Name); }
	return Out;
}

TArray<FName> FOpsEventBus::PassesAfter(FName Pass) const
{
	const FPass* Found = Passes.FindByPredicate([Pass](const FPass& Each) { return Each.Name == Pass; });
	return Found != nullptr ? Found->After : TArray<FName>();
}

void FOpsEventBus::ResetWiring()
{
	check(!bDraining);
	for (SIZE_T Type = 0; Type < NumTypes; ++Type)
	{
		for (int32 Tier = 0; Tier < NumTiers; ++Tier)
		{
			Handlers[Type][Tier].Reset();
		}
	}
	Passes.Reset();
}

void FOpsEventBus::RegisterPass(FName Name, TFunction<void(const FPassRun&)> Run, std::initializer_list<FName> After)
{
	check(bWiring && !bDraining);
	FPass Pass;
	Pass.Name = Name;
	Pass.Run = MoveTemp(Run);
	Pass.After = TArray<FName>(After.begin(), static_cast<int32>(After.size()));
	Passes.Add(MoveTemp(Pass));
	FString Dependencies;
	for (const FName Each : After) { Dependencies += (Dependencies.IsEmpty() ? TEXT("") : TEXT(", ")) + Each.ToString(); }
	UE_LOG(LogOpsBus, Log, TEXT("Bus: pass %s registered%s%s"), *Name.ToString(),
		Dependencies.IsEmpty() ? TEXT("") : TEXT(", after "), *Dependencies);
}

void FOpsEventBus::MarkDirty(FName Pass, EPassCause Cause)
{
	for (FPass& Each : Passes)
	{
		if (Each.Name == Pass)
		{
			Each.bDirty = true;
			// THE STRONGEST CAUSE WINS (EPassCause's ordinal): a net mark does not demote an event's.
			Each.Cause = FMath::Max(Each.Cause, Cause);
			return;
		}
	}
	// A MISSPELT PASS IS A PASS THAT NEVER RUNS - said, not swallowed.
	UE_LOG(LogOpsBus, Warning, TEXT("Bus: MarkDirty(%s) names no registered pass"), *Pass.ToString());
}

void FOpsEventBus::MarkDirtyNextDrain(FName Pass, EPassCause Cause)
{
	for (FPass& Each : Passes)
	{
		if (Each.Name == Pass)
		{
			Each.NextCause = Each.bDirtyNextDrain ? FMath::Max(Each.NextCause, Cause) : Cause;
			Each.bDirtyNextDrain = true;
			return;
		}
	}
	UE_LOG(LogOpsBus, Warning, TEXT("Bus: MarkDirtyNextDrain(%s) names no registered pass"), *Pass.ToString());
}

void FOpsEventBus::MarkAllDirty()
{
	// AN EVENT'S CAUSE: a catch-up (an attach, a load) is asked for by the game, not by the net - a run of it that finds work
	// has found what nothing announced, which is the catch-up's whole point.
	for (FPass& Each : Passes)
	{
		Each.bDirty = true;
		Each.Cause = EPassCause::Event;
	}
}

bool FOpsEventBus::AnyDirty() const
{
	return Passes.ContainsByPredicate([](const FPass& Each) { return Each.bDirty; });
}

int32 FOpsEventBus::Drain()
{
	check(!bDraining);
	bDraining = true;
	// LAST FRAME'S "TRY AGAIN NEXT FRAME" becomes this frame's dirty - see MarkDirtyNextDrain.
	for (FPass& Pass : Passes)
	{
		if (Pass.bDirtyNextDrain)
		{
			Pass.bDirty = true;
			Pass.Cause = FMath::Max(Pass.Cause, Pass.NextCause);
		}
		Pass.bDirtyNextDrain = false;
	}
	int32 Dispatched = 0;
	int32 Round = 0;
	for (; Round < MaxRounds && (Queue.Num() > 0 || AnyDirty()); ++Round)
	{
		// THE WHOLE QUEUE, taken: anything a handler publishes lands in Queue again and is the next
		// round's, so no handler runs inside another event's dispatch.
		TArray<FOpsEvent> Batch = MoveTemp(Queue);
		Queue.Reset();
		for (const FOpsEvent& Event : Batch)
		{
			UE_LOG(LogOpsBus, Verbose, TEXT("Bus: > %s {%s}"), NameOf(Event), *Describe(Event));
			for (int32 Tier = 0; Tier < NumTiers; ++Tier)
			{
				for (const FHandler& Handler : Handlers[Event.GetIndex()][Tier])
				{
					Handler.Run(Event);
				}
			}
			++DispatchedCounts[Event.GetIndex()];
			++Dispatched;
		}
		// Passes run after every tier of the round: they mutate the sim and may publish. IN THE ORDER EndWiring made of
		// After - see RegisterPass.
		for (FPass& Pass : Passes)
		{
			if (Pass.bDirty)
			{
				// CLEANED BEFORE IT RUNS, and told what marked it: a pass that re-marks itself is the NEXT round's, and its own
				// re-mark starts a fresh cause.
				const FPassRun Run{ Pass.Cause };
				Pass.bDirty = false;
				Pass.Cause = EPassCause::SafetyNet;
				Pass.Run(Run);
			}
		}
	}
	if (Queue.Num() > 0 || AnyDirty())
	{
		FString Left;
		for (const FOpsEvent& Event : Queue) { Left += FString(NameOf(Event)) + TEXT(" "); }
		for (const FPass& Pass : Passes) { if (Pass.bDirty) { Left += TEXT("pass:") + Pass.Name.ToString() + TEXT(" "); } }
		UE_LOG(LogOpsBus, Error, TEXT("Bus: round cap (%d) hit; carried to next frame: %s"), MaxRounds, *Left.TrimEnd());
	}
	bDraining = false;
	return Dispatched;
}

int32 FOpsEventBus::Discard()
{
	const int32 Dropped = Queue.Num();
	// EACH ONE NAMED, not only counted: a discard is the one place an event dies unhandled, and "3
	// events" cannot say whether one of them was the one being chased.
	for (const FOpsEvent& Event : Queue)
	{
		UE_LOG(LogOpsBus, Log, TEXT("Bus: x %s {%s} (discarded)"), NameOf(Event), *Describe(Event));
	}
	Queue.Reset();
	UE_LOG(LogOpsBus, Log, TEXT("Bus: discarded %d queued event(s)"), Dropped);
	return Dropped;
}

TArray<FName> FOpsEventBus::SubscribersOf(SIZE_T TypeIndex) const
{
	TArray<FName> Out;
	if (TypeIndex < NumTypes)
	{
		for (int32 Tier = 0; Tier < NumTiers; ++Tier)
		{
			for (const FHandler& Handler : Handlers[TypeIndex][Tier]) { Out.Add(Handler.Who); }
		}
	}
	return Out;
}
