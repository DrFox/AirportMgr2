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
	return FString::Printf(TEXT("agent %d, %s -> %s"), AgentId, *UEnum::GetValueAsString(From), *UEnum::GetValueAsString(To));
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

FString FAlertsResetEvent::Describe() const
{
	return TEXT("every alert forgotten");
}

FString FBuildRefusedEvent::Describe() const
{
	return FString::Printf(TEXT("%s, cannot afford %s, balance %s"), *What, *Price, *Balance);
}

FString FLandRefusedEvent::Describe() const
{
	return UEnum::GetValueAsString(Why);
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
void FOpsEventBus::EndWiring() { bWiring = false; }

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

void FOpsEventBus::RegisterPass(FName Name, TFunction<void()> Run)
{
	check(bWiring && !bDraining);
	Passes.Add({ Name, MoveTemp(Run), false });
	UE_LOG(LogOpsBus, Log, TEXT("Bus: pass %s registered"), *Name.ToString());
}

void FOpsEventBus::MarkDirty(FName Pass)
{
	for (FPass& Each : Passes)
	{
		if (Each.Name == Pass)
		{
			Each.bDirty = true;
			return;
		}
	}
	// A MISSPELT PASS IS A PASS THAT NEVER RUNS - said, not swallowed.
	UE_LOG(LogOpsBus, Warning, TEXT("Bus: MarkDirty(%s) names no registered pass"), *Pass.ToString());
}

void FOpsEventBus::MarkDirtyNextDrain(FName Pass)
{
	for (FPass& Each : Passes)
	{
		if (Each.Name == Pass)
		{
			Each.bDirtyNextDrain = true;
			return;
		}
	}
	UE_LOG(LogOpsBus, Warning, TEXT("Bus: MarkDirtyNextDrain(%s) names no registered pass"), *Pass.ToString());
}

void FOpsEventBus::MarkAllDirty()
{
	for (FPass& Each : Passes) { Each.bDirty = true; }
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
		Pass.bDirty |= Pass.bDirtyNextDrain;
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
			++Dispatched;
		}
		// Passes run after every tier of the round: they mutate the sim and may publish.
		for (FPass& Pass : Passes)
		{
			if (Pass.bDirty)
			{
				Pass.bDirty = false;
				Pass.Run();
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
