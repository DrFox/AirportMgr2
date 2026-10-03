// THE HEADLINE TEST of space-time taxi planning (spec 2026-10-02 §4): the spike harness (branch feature/taxi-deadlock-spike,
// ac4562b4, TaxiDeadlockSpikeTest.cpp) made permanent - the spike's own map, rates, duration and metrics, with the
// admission strategies it implemented in the harness replaced by THE MODEL'S OWN: an arrival is dispatched only when
// UGroundTraffic::TaxiInRefusal says a taxi-in plan exists (what the arrival queue asks), a departure is asked of
// DepartAgent (pushback gated on a plan). Plus seeded random delays: aircraft held still for a while, mid-taxi, so a
// plan's TIMES go wrong and only its ORDER is left to keep the field moving (spec §2).

#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "AirsideTestsLog.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Math/RandomStream.h"
#include "Misc/AutomationTest.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Model/ArrivalPlanner.h"
#include "Model/DeparturePlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Model/TaxiPlanning.h"
#include "Model/TrafficClaims.h"
#include "Entities/AircraftType.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadNetworkActor.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace TaxiPlanDeadlock
{
	/**
	 * Counts the deadlock resolver's lines by member set - the spike's spy, with its reservation cycles counted as jams too:
	 * a cycle the resolver can only answer by yielding, again and again, is one nobody gets out of. UNBUFFERED (memory: a log spy that is not
	 * gets its lines on the log thread after it is gone - the #216 flake).
	 */
	struct FDeadlockSpy : public FOutputDevice
	{
		TMap<FString, int32> NoTurnBySet;
		TMap<FString, int32> ResolvedBySet;
		TMap<FString, double> LastSeen;
		TMap<FString, double> FirstSeen;
		int32 NotMyTurnLines = 0;
		double Now = 0.0;

		/**
		 * RESOLVER ACTIVITY ON PLANNED AIRCRAFT (review of #528 finding 3): sets with a member that held a taxi plan when the
		 * line was written. The plan's order is what keeps planned aircraft out of cycles; the resolver turning one is a
		 * planner bug, whether or not it untangles it. Read on the game thread's log call, as the model writes the line.
		 */
		const UTaxiPlanning* Planning = nullptr;
		const UGroundTraffic* Traffic = nullptr;
		TSet<FString> PlannedSets;
		TArray<FString> PendingDumps;
		TSet<FString> Dumped;

		/** Off the log call (logging from inside Serialize re-enters the redirector): after the tick. */
		void FlushDumps()
		{
			for (const FString& Set : PendingDumps)
			{
				if (!Dumped.Contains(Set))
				{
					Dumped.Add(Set);
					DumpSet(Set);
				}
			}
			PendingDumps.Reset();
		}

		/** DIAGNOSIS, once per planned set: each member's wait, the windows on what it waits for, and its plan ahead. */
		void DumpSet(const FString& Set) const
		{
			if (Planning == nullptr || Traffic == nullptr)
			{
				return;
			}
			TArray<FString> Ids;
			Set.ParseIntoArray(Ids, TEXT(","));
			for (const FString& IdText : Ids)
			{
				const int32 Id = FCString::Atoi(*IdText.TrimStartAndEnd());
				const FRoadAgent* Agent = Traffic->FindAgent(Id);
				const FTaxiClearance* Clearance = Planning->Find(Id);
				if (Agent == nullptr || Clearance == nullptr)
				{
					continue;
				}
				const FTrafficResource& On = Agent->GetBlockedResource();
				const FTaxiResource R = On.Kind == ETrafficResourceKind::Edge ? FTaxiResource::Edge(On.Edge) : FTaxiResource::Node(On.Node);
				FString Windows;
				for (const FTaxiWindow& W : Planning->GetTable().WindowsOn(R))
				{
					Windows += FString::Printf(TEXT(" [%d %.0f-%.0f w%d]"), W.Holder, W.From, W.To >= FTaxiReservations::Forever ? -1.0 : W.To, (int32)W.Way);
				}
				const FRoutePlan& Route = Agent->PlanInProgress();
				const int32 Cur = UGroundTraffic::CurrentStep(Route, Agent->Follower.Travelled);
				FString Ahead;
				for (int32 I = FMath::Max(0, Cur - 2); I < Route.Steps.Num() && I < Cur + 10; ++I)
				{
					const double Reach = Clearance->Plan.Legs.IsValidIndex(I) ? Clearance->Plan.Legs[I].Reach : -1.0;
					Ahead += FString::Printf(TEXT(" %s%d(e%d@%.0f)"), I == Cur ? TEXT("*") : TEXT(""), Route.Steps[I].To.Index, Route.Steps[I].Edge.Index, Reach);
				}
				FString Steps;
				const double Centre = FClaimPass::CentreOf(*Agent);
				for (int32 I = FMath::Max(0, Cur - 3); I <= Cur && I < Route.Steps.Num(); ++I)
				{
					const bool bNodeHeld = Traffic->GetOccupancy().FindClaim(Id, FTrafficResource::OfNode(Route.Steps[I].To)) != nullptr;
					const bool bEdgeHeld = Traffic->GetOccupancy().FindClaim(Id, FTrafficResource::OfEdge(Route.Steps[I].Edge)) != nullptr;
					Steps += FString::Printf(TEXT(" [%d end %.0f e%d n%d]"), Route.Steps[I].To.Index, Route.Steps[I].EndDistance, bEdgeHeld ? 1 : 0, bNodeHeld ? 1 : 0);
				}
				UE_LOG(LogAirsideTests, Display, TEXT("TaxiPlanDeadlock:   diag2 agent %d centre %.0f released-through %d start-released %d phase %s steps%s"),
					Id, Centre, Clearance->ReleasedThrough, Clearance->bStartReleased ? 1 : 0, *UEnum::GetValueAsString(Agent->Phase), *Steps);
				UE_LOG(LogAirsideTests, Display, TEXT("TaxiPlanDeadlock:   diag t=%.0f agent %d waits %d on %s:%s | moves granted %d/%d end %d queue %d | route%s"),
					Traffic->GetSimSeconds(), Id, Agent->GetWaitingOn(), *On.Describe(), *Windows, Clearance->GrantedMove,
					Clearance->Plan.MoveStarts.Num(), Clearance->GrantedEnd, Clearance->QueueFor.Index, *Ahead);
			}
		}

		bool AnyMemberPlanned(const FString& Set) const
		{
			if (Planning == nullptr)
			{
				return false;
			}
			TArray<FString> Ids;
			Set.ParseIntoArray(Ids, TEXT(","));
			for (const FString& Id : Ids)
			{
				if (Planning->Find(FCString::Atoi(*Id.TrimStartAndEnd())) != nullptr)
				{
					return true;
				}
			}
			return false;
		}

		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
		virtual bool CanBeUsedOnAnyThread() const override { return true; }

		static FString SetOf(const FString& Line)
		{
			int32 Open = INDEX_NONE;
			int32 Close = INDEX_NONE;
			if (Line.FindChar(TEXT('['), Open) && Line.FindChar(TEXT(']'), Close) && Close > Open)
			{
				return Line.Mid(Open + 1, Close - Open - 1);
			}
			return FString();
		}

		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Category != FName(TEXT("LogAirsideTraffic")))
			{
				return;
			}
			const FString Line(V);
			if (Line.Contains(TEXT("not my turn")))
			{
				++NotMyTurnLines;
			}
			else if (Line.Contains(TEXT("no member can turn")) || Line.Contains(TEXT("Reservation cycle among agents")))
			{
				const FString S = SetOf(Line);
				NoTurnBySet.FindOrAdd(S)++;
				LastSeen.Add(S, Now);
				FirstSeen.FindOrAdd(S, Now);
				if (AnyMemberPlanned(S))
				{
					PendingDumps.AddUnique(S);
					PlannedSets.Add(S);
				}
			}
			else if (Line.Contains(TEXT("eadlock among agents")) && Line.Contains(TEXT("resolved")))
			{
				const FString S = SetOf(Line);
				ResolvedBySet.FindOrAdd(S)++;
				LastSeen.Add(S, Now);
				FirstSeen.FindOrAdd(S, Now);
				if (AnyMemberPlanned(S))
				{
					PendingDumps.AddUnique(S);
					PlannedSets.Add(S);
				}
			}
		}
	};

	struct FAgentStat
	{
		int32 Id = 0;
		double Dispatched = 0.0;
		double ScheduledAt = 0.0;
		double FirstParked = -1.0;
		double DepartOrderAt = -1.0;
		double DepartAdmitted = -1.0;
		double DepartingAt = -1.0;
		double TaxiIn = 0.0;
		double TaxiOut = 0.0;
		double StoppedRun = 0.0;
		double MaxStoppedRun = 0.0;
		/** When a stop first passed 300 s: who it waited on, and whether it was queueing for a runway entry (its plan's end). */
		int32 Stuck300WaitingOn = 0;
		bool bStuck300Queued = false;
		double Stuck300At = -1.0;
		bool bGone = false;
		bool bInbound = true;
	};

	/** What one run measured - the assertions' inputs. */
	struct FRunResult
	{
		int32 Scheduled = 0;
		int32 Admitted = 0;
		int32 Pending = 0;
		int32 Permanent = 0;
		int32 Unparked = 0;
		int32 Undeparted = 0;
		int32 PlannedResolverSets = 0;
		int32 Stuck300 = 0;
		int32 LargeAdmitted = 0;
	};

	struct FSim
	{
		URoadNetwork* Net = nullptr;
		UGroundTraffic* Traffic = nullptr;
		FAirframe Airframe;

		/**
		 * A LARGER AIRFRAME IN THE MIX (review of #528 finding 3d): every LargeEvery-th scheduled arrival is this one -
		 * DA_Aircraft_A320, a Code C jet, where the spike flew Pipers only - so sizes, stand fit and longer pieces are in
		 * the plans the order is checked on. 0: none.
		 */
		FAirframe Large;
		int32 LargeEvery = 0;
		int32 LargeAdmitted = 0;
		TArray<FVector2D> RunwayNear;
		FRandomStream Random;
		double Now = 0.0;

		// The spike's figures (ac4562b4), unchanged: 30 Hz, two hours, a 20-minute dwell.
		double Dt = 1.0 / 30.0;
		double Duration = 7200.0;
		double ArrivalInterval = 180.0;
		double Dwell = 1200.0;

		/**
		 * After Duration nothing more is scheduled, and the field is run until it is empty - or this long. 5400 s since PR 3
		 * (2026-10-02): at 80/h it admits 67, not 41, and the last of them lands, dwells 20 minutes and taxis out in a runway
		 * queue - 3600 s ended the run with five still moving, none stuck (no stop of 300 s at the end).
		 */
		double DrainLimit = 5400.0;

		/** SEEDED DELAYS: every DelayEvery seconds one taxiing aircraft, picked by Random, is held still 20-90 s. */
		double DelayEvery = 90.0;

		TMap<int32, FAgentStat> Stats;
		TArray<double> PendingArrivals;
		int32 ArrivalRefusals = 0;
		int32 DepartRefusals = 0;
		int32 Delays = 0;
		double ArrivalDelaySum = 0.0;
		int32 ArrivalDelayN = 0;
		int32 MaxTaxiing = 0;
		TArray<double> TickMs;

		void TryArrivals()
		{
			for (int32 I = 0; I < PendingArrivals.Num();)
			{
				const double Sched = PendingArrivals[I];
				const int32 Ordinal = static_cast<int32>(Sched / ArrivalInterval);
				const FVector2D Near = RunwayNear[Ordinal % RunwayNear.Num()];
				const bool bLarge = LargeEvery > 0 && Ordinal % LargeEvery == LargeEvery - 1;
				const FAirframe& Flying = bLarge ? Large : Airframe;
				// THE ARRIVAL QUEUE'S QUESTION, asked as it asks it: the plan, then whether it gets a taxi-in plan. A
				// refusal keeps it holding - FIFO, as the spike's admission did - and it is asked again next second.
				const FArrivalPlan Plan = ArrivalPlanner::Plan(*Net, Near, Flying, &Traffic->GetOccupancy());
				if (!Plan.IsValid() || Traffic->TaxiInRefusal(*Net, Plan, Flying) != EArrivalRefusal::None)
				{
					++ArrivalRefusals;
					break;
				}
				const int32 Id = Traffic->DispatchArrival(*Net, Near, Flying, 1.0);
				if (Id <= 0)
				{
					++ArrivalRefusals;
					break;
				}
				LargeAdmitted += bLarge ? 1 : 0;
				FAgentStat& S = Stats.Add(Id);
				S.Id = Id;
				S.Dispatched = Now;
				S.ScheduledAt = Sched;
				ArrivalDelaySum += Now - Sched;
				++ArrivalDelayN;
				PendingArrivals.RemoveAt(I);
			}
		}

		void TryDeparture(FAgentStat& S)
		{
			if (Traffic->DepartAgent(S.Id, *Net) == EDepartureRefusal::None)
			{
				S.DepartAdmitted = Now;
				S.bInbound = false;
			}
			else
			{
				++DepartRefusals;
			}
		}

		void MaybeDelay()
		{
			TArray<int32> Taxiing;
			for (const FRoadAgent& Agent : Traffic->GetAgents())
			{
				if (Agent.Phase == EAgentPhase::Taxiing)
				{
					Taxiing.Add(Agent.Id);
				}
			}
			if (Taxiing.Num() == 0)
			{
				return;
			}
			const int32 Id = Taxiing[Random.RandRange(0, Taxiing.Num() - 1)];
			FGroundTrafficTestAccess(*Traffic).TaxiPlanning()->DelayForTest(Id, Now + Random.FRandRange(20.0, 90.0));
			++Delays;
		}

		/** One tick of the field: admissions once a second, the model, the bookkeeping. */
		void Step(FDeadlockSpy& Spy, double& NextAdmit, double& NextDelay, bool bAdmit)
		{
			Spy.Now = Now;
			if (Now >= NextAdmit)
			{
				NextAdmit = Now + 1.0;
				if (bAdmit)
				{
					TryArrivals();
				}
				for (TPair<int32, FAgentStat>& Pair : Stats)
				{
					FAgentStat& S = Pair.Value;
					const FRoadAgent* Agent = Traffic->FindAgent(S.Id);
					if (Agent == nullptr || S.bGone || !S.bInbound || Agent->Phase != EAgentPhase::Parked || S.FirstParked < 0.0)
					{
						continue;
					}
					if (Now - S.FirstParked >= Dwell && (S.DepartOrderAt < 0.0 || FMath::Fmod(Now, 5.0) < 1.0))
					{
						if (S.DepartOrderAt < 0.0)
						{
							S.DepartOrderAt = Now;
						}
						TryDeparture(S);
					}
				}
			}
			if (Now >= NextDelay)
			{
				NextDelay = Now + DelayEvery;
				MaybeDelay();
			}

			const double Begin = FPlatformTime::Seconds();
			Traffic->Advance(Dt, Net);
			TickMs.Add((FPlatformTime::Seconds() - Begin) * 1000.0);
			Spy.FlushDumps();

			int32 Moving = 0;
			for (TPair<int32, FAgentStat>& Pair : Stats)
			{
				FAgentStat& S = Pair.Value;
				if (S.bGone)
				{
					continue;
				}
				const FRoadAgent* Agent = Traffic->FindAgent(S.Id);
				if (Agent == nullptr)
				{
					S.bGone = true;
					continue;
				}
				if (Agent->Phase == EAgentPhase::Parked && S.FirstParked < 0.0)
				{
					S.FirstParked = Now;
				}
				if (Agent->Phase == EAgentPhase::Departing && S.DepartingAt < 0.0)
				{
					S.DepartingAt = Now;
				}
				// A departure booked to push later is admitted by its push, not by DepartAgent's answer.
				if (S.bInbound && S.DepartOrderAt >= 0.0 && Agent->Phase == EAgentPhase::Manoeuvring)
				{
					S.DepartAdmitted = Now;
					S.bInbound = false;
				}
				const bool bMoving = Agent->Phase == EAgentPhase::Taxiing || Agent->Phase == EAgentPhase::Manoeuvring;
				if (bMoving)
				{
					++Moving;
					(S.bInbound ? S.TaxiIn : S.TaxiOut) += Dt;
					if (FMath::Abs(Agent->SpeedAlongPlan()) < 1.0)
					{
						S.StoppedRun += Dt;
						if (S.StoppedRun >= 300.0 && S.MaxStoppedRun < 300.0)
						{
							const FTaxiClearance* Clearance = Traffic->GetTaxiPlanning()->Find(S.Id);
							S.Stuck300WaitingOn = Agent->GetWaitingOn();
							S.bStuck300Queued = Clearance != nullptr && Clearance->QueueFor.IsSet();
							S.Stuck300At = Now;
						}
						S.MaxStoppedRun = FMath::Max(S.MaxStoppedRun, S.StoppedRun);
					}
					else
					{
						S.StoppedRun = 0.0;
					}
				}
			}
			MaxTaxiing = FMath::Max(MaxTaxiing, Moving);
		}

		void Run(FDeadlockSpy& Spy)
		{
			TArray<double> Scheduled;
			for (double T = 0.0; T < Duration; T += ArrivalInterval)
			{
				Scheduled.Add(T);
			}
			double NextAdmit = 0.0;
			double NextDelay = DelayEvery;
			int32 NextSched = 0;
			const int32 Ticks = FMath::CeilToInt32(Duration / Dt);
			for (int32 Tick = 0; Tick < Ticks; ++Tick)
			{
				Now = Tick * Dt;
				while (NextSched < Scheduled.Num() && Scheduled[NextSched] <= Now)
				{
					PendingArrivals.Add(Scheduled[NextSched++]);
				}
				Step(Spy, NextAdmit, NextDelay, true);
			}
			// THE DRAIN: no more arrivals; everything admitted is let finish - parks, dwells, departs - or the limit says
			// it never would. Pending arrivals not yet admitted are not the field's: they never landed.
			const int32 DrainTicks = FMath::CeilToInt32(DrainLimit / Dt);
			for (int32 Tick = 0; Tick < DrainTicks && OnGround() > 0; ++Tick)
			{
				Now += Dt;
				Step(Spy, NextAdmit, NextDelay, false);
			}
		}

		int32 OnGround() const
		{
			int32 Count = 0;
			for (const TPair<int32, FAgentStat>& Pair : Stats)
			{
				Count += Pair.Value.bGone ? 0 : 1;
			}
			return Count;
		}

		static double Pct(TArray<double> V, double P)
		{
			if (V.Num() == 0)
			{
				return 0.0;
			}
			V.Sort();
			return V[FMath::Clamp(FMath::FloorToInt32(P * (V.Num() - 1)), 0, V.Num() - 1)];
		}

		static double Mean(const TArray<double>& V)
		{
			double Sum = 0.0;
			for (const double X : V)
			{
				Sum += X;
			}
			return V.Num() > 0 ? Sum / V.Num() : 0.0;
		}

		/** The spike's row, plus what this harness adds (delays, refusals, planned resolver sets). */
		FRunResult Report(const FDeadlockSpy& Spy, const TCHAR* Label, int32 Scheduled) const
		{
			int32 Arrived = 0;
			int32 Departed = 0;
			int32 Stuck300 = 0;
			int32 OutUnparked = 0;
			int32 OutUndeparted = 0;
			TArray<double> In;
			TArray<double> Out;
			TArray<double> DepWait;
			for (const TPair<int32, FAgentStat>& Pair : Stats)
			{
				const FAgentStat& S = Pair.Value;
				if (S.FirstParked >= 0.0) { ++Arrived; In.Add(S.TaxiIn); } else { ++OutUnparked; }
				if (S.DepartingAt >= 0.0) { ++Departed; Out.Add(S.TaxiOut); }
				if (S.DepartOrderAt >= 0.0 && S.DepartingAt < 0.0) { ++OutUndeparted; }
				if (S.DepartAdmitted >= 0.0 && S.DepartOrderAt >= 0.0) { DepWait.Add(S.DepartAdmitted - S.DepartOrderAt); }
				if (S.MaxStoppedRun >= 300.0) { ++Stuck300; }
			}
			// PERMANENT: a set still reported "no member can turn" in the last 120 s of the run - the spike's definition.
			TSet<FString> Sets;
			for (const auto& P : Spy.NoTurnBySet) { Sets.Add(P.Key); }
			for (const auto& P : Spy.ResolvedBySet) { Sets.Add(P.Key); }
			int32 Permanent = 0;
			int32 Resolved = 0;
			for (const FString& S : Sets)
			{
				const double* Last = Spy.LastSeen.Find(S);
				Permanent += (Spy.NoTurnBySet.Contains(S) && Last != nullptr && *Last > Now - 120.0) ? 1 : 0;
				Resolved += Spy.ResolvedBySet.Contains(S) ? 1 : 0;
			}
			UE_LOG(LogAirsideTests, Display,
				TEXT("TaxiPlanDeadlock: [%s] admitted=%d parked=%d departed=%d stillOnGround=%d pendingArrivals=%d | ")
				TEXT("deadlockSets=%d resolvedSets=%d permanentSets=%d notMyTurnLines=%d | stuck>=300s=%d | ")
				TEXT("taxiIn mean=%.0f p95=%.0f | taxiOut mean=%.0f p95=%.0f | depWait mean=%.0f p95=%.0f | arrDelay mean=%.0f | ")
				TEXT("maxTaxiing=%d | tick ms mean=%.3f p95=%.3f max=%.3f | arrRefusals=%d depRefusals=%d delays=%d | simEnd=%.0f s"),
				Label, Stats.Num(), Arrived, Departed, OnGround(), PendingArrivals.Num(),
				Sets.Num(), Resolved, Permanent, Spy.NotMyTurnLines, Stuck300,
				Mean(In), Pct(In, 0.95), Mean(Out), Pct(Out, 0.95), Mean(DepWait), Pct(DepWait, 0.95),
				ArrivalDelayN > 0 ? ArrivalDelaySum / ArrivalDelayN : 0.0,
				MaxTaxiing, Mean(TickMs), Pct(TickMs, 0.95), Pct(TickMs, 1.0), ArrivalRefusals, DepartRefusals, Delays, Now);
			UE_LOG(LogAirsideTests, Display, TEXT("TaxiPlanDeadlock: [%s] scheduled=%d largeAdmitted=%d plannedResolverSets=%d"),
				Label, Scheduled, LargeAdmitted, Spy.PlannedSets.Num());
			for (const auto& P : Spy.FirstSeen)
			{
				UE_LOG(LogAirsideTests, Display, TEXT("TaxiPlanDeadlock:   set [%s] first %.0f s last %.0f s noTurn=%d resolved=%d"),
					*P.Key, P.Value, Spy.LastSeen.FindRef(P.Key), Spy.NoTurnBySet.FindRef(P.Key), Spy.ResolvedBySet.FindRef(P.Key));
			}
			for (const TPair<int32, FAgentStat>& Pair : Stats)
			{
				const FAgentStat& S = Pair.Value;
				if (S.MaxStoppedRun >= 300.0)
				{
					UE_LOG(LogAirsideTests, Display,
						TEXT("TaxiPlanDeadlock:   stood 300 s: agent %d %s from %.0f s, longest %.0f s, waitingOn=%d queued=%d"),
						S.Id, S.bInbound ? TEXT("IN") : TEXT("OUT"), S.Stuck300At - 300.0, S.MaxStoppedRun, S.Stuck300WaitingOn,
						S.bStuck300Queued ? 1 : 0);
				}
			}
			for (const TPair<int32, FAgentStat>& Pair : Stats)
			{
				const FAgentStat& S = Pair.Value;
				const FRoadAgent* A = Traffic->FindAgent(S.Id);
				if (A != nullptr && S.StoppedRun >= 300.0)
				{
					const FVector2D P = A->GroundPosition();
					UE_LOG(LogAirsideTests, Display,
						TEXT("TaxiPlanDeadlock:   stuck agent %d %s phase=%s at (%.0f, %.0f) waitingOn=%d stoppedRun=%.0f"),
						S.Id, S.bInbound ? TEXT("IN") : TEXT("OUT"), *UEnum::GetValueAsString(A->Phase), P.X, P.Y,
						A->GetWaitingOn(), S.StoppedRun);
				}
			}
			FRunResult Result;
			Result.Scheduled = Scheduled;
			Result.Admitted = Stats.Num();
			Result.Pending = PendingArrivals.Num();
			Result.Permanent = Permanent;
			Result.Unparked = OutUnparked;
			Result.Undeparted = OutUndeparted;
			Result.PlannedResolverSets = Spy.PlannedSets.Num();
			Result.Stuck300 = Stuck300;
			Result.LargeAdmitted = LargeAdmitted;
			return Result;
		}
	};

	/** The saved network actor in M_ScaleGatwick, or null - the spike's loader. */
	ARoadNetworkActor* LoadSavedActor()
	{
		UPackage* Package = LoadPackage(nullptr, TEXT("/Game/Maps/M_ScaleGatwick"), LOAD_None);
		UWorld* World = Package != nullptr ? UWorld::FindWorldInPackage(Package) : nullptr;
		if (World == nullptr || World->PersistentLevel == nullptr)
		{
			return nullptr;
		}
		for (AActor* Actor : World->PersistentLevel->Actors)
		{
			if (ARoadNetworkActor* Road = Cast<ARoadNetworkActor>(Actor))
			{
				return Road;
			}
		}
		return nullptr;
	}

	/** One run on a fresh copy of the saved network. False only when the field could not be set up. */
	bool RunOne(FAutomationTestBase& Test, double ArrivalInterval, const TCHAR* Label, bool bEnforceOrder, FRunResult& Out)
	{
		ARoadNetworkActor* Saved = LoadSavedActor();
		if (!Test.TestNotNull(TEXT("M_ScaleGatwick's road network actor loaded"), Saved) || Saved->Network == nullptr)
		{
			return false;
		}

		ARoadNetworkActor* Actor = NewObject<ARoadNetworkActor>(GetTransientPackage());
		Actor->PlaceNode(FVector2D(-2000000.0, -2000000.0));
		Actor->Network->RestoreFrom(*Saved->Network);
		Actor->TrafficRules = Saved->TrafficRules;
		Actor->RebuildMesh();
		URoadNetwork* Net = Actor->Network.Get();

		// One point per runway (its segments' mean midpoint), as the spike found them.
		TMap<int32, TPair<FVector2D, int32>> RunwayMid;
		for (int32 I = 0; I < Net->GetSegments().Num(); ++I)
		{
			const FRoadSegment& S = Net->GetSegments()[I];
			const FRoadSegmentId Id = Net->SegmentIdAt(I);
			if (!S.bAlive || !Net->IsRunwaySegment(Id))
			{
				continue;
			}
			int32 Head = MAX_int32;
			for (const FRoadSegmentId& C : Net->RunwayChainOrSeed(Id))
			{
				Head = FMath::Min(Head, static_cast<int32>(C.Index));
			}
			const FVector2D Mid = (Net->GetNode(S.A)->Position + Net->GetNode(S.B)->Position) * 0.5;
			TPair<FVector2D, int32>& Acc = RunwayMid.FindOrAdd(Head, TPair<FVector2D, int32>(FVector2D::ZeroVector, 0));
			Acc.Key += Mid;
			Acc.Value += 1;
		}

		FSim Sim;
		Sim.Net = Net;
		Sim.Traffic = Actor->GetTraffic()->GetModel();
		Sim.Traffic->Rules = Saved->TrafficRules;
		Sim.Airframe = TestAirframes::Piper();
		if (const UAircraftType* A320 = Cast<UAircraftType>(StaticLoadObject(UAircraftType::StaticClass(), nullptr,
			TEXT("/Game/Entities/DA_Aircraft_A320"))))
		{
			Sim.Large = A320->Airframe();
			Sim.LargeEvery = 4;
		}
		Test.TestTrue(TEXT("the A320 loads - the larger airframe in the mix"), Sim.LargeEvery > 0);
		// A PAPER TYPE (AirsideSettings.cpp: DA_Aircraft_A320 carries no mesh) with no axle layout: its follower says once per
		// flight that it pivots rather than steers, at Error. That is the content's gap, not this test's subject - its size,
		// pavement and stand fit are what it is in the mix for. Expected, not hidden: it must still be said.
		Test.AddExpectedMessage(TEXT("Chassis declares RollingSteer with no wheelbase"), ELogVerbosity::Error,
			EAutomationExpectedMessageFlags::Contains, 0);
		Sim.ArrivalInterval = ArrivalInterval;
		Sim.Random.Initialize(20261002);
		FGroundTrafficTestAccess(*Sim.Traffic).TaxiPlanning()->bEnforceOrderForTest = bEnforceOrder;
		for (const auto& Pair : RunwayMid)
		{
			Sim.RunwayNear.Add(Pair.Value.Key / Pair.Value.Value);
		}
		if (!Test.TestTrue(TEXT("found the runways"), Sim.RunwayNear.Num() > 0))
		{
			return false;
		}

		FDeadlockSpy Spy;
		Spy.Planning = Sim.Traffic->GetTaxiPlanning();
		Spy.Traffic = Sim.Traffic;
		GLog->AddOutputDevice(&Spy);
		Sim.Run(Spy);
		GLog->RemoveOutputDevice(&Spy);
		Out = Sim.Report(Spy, Label, FMath::CeilToInt32(Sim.Duration / ArrivalInterval));
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTaxiPlanNoPermanentDeadlockTest,
	"Airside.Perf.TaxiPlan.NoPermanentDeadlock",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanNoPermanentDeadlockTest::RunTest(const FString& Parameters)
{
	// THE SPIKE'S TWO RATES: an arrival every 180 s (~40 movements an hour at steady state) and every 90 s (~80).
	// ZERO PERMANENT DEADLOCKS, AND THE FIELD EMPTIES: every aircraft admitted parks, and every one ordered off departs,
	// within the drain - an aircraft stuck for good fails here even when the resolver never names a set.
	// Baseline on this map (spec table): one permanent jam at each rate (27 and 53 aircraft stuck).
	//
	// NOT VACUOUS (review of #528 finding 3): a field that admits nobody has no deadlock either, so each rate also asserts
	// how many arrivals LANDED, and that the resolver never touched a planned aircraft.
	// - 40/h: every scheduled arrival admitted - all 40 (2026-10-02, with every fourth an A320: 40 admitted).
	// - 80/h: an honest floor, not all 80: 67 of 80 admitted with the A320 in the mix (2026-10-02, PR 3). PR 2 admitted 39:
	//   a departure held its runway entry for ever until it lined up, every later one queued at a holding node held for
	//   ever too, and taxi-in routes through them found no window. PR 3 bounds the entry hold (TaxiPlanEntryHold). The
	//   floor is 67 less a slack of 7, so a regression in arrival capacity goes red.
	// MUTATION PROOF, not in the suite (it doubles the test's ~6 min): with UTaxiPlanning::bEnforceOrderForTest false this
	// went red, "40/h: 1 jam, 80/h: 2 jams" (one-off local build, 2026-10-02 - see the plan's PR 2 execution notes).
	struct FRate
	{
		double Interval;
		const TCHAR* Label;
		int32 AdmittedFloor;
	};
	const FRate Rates[] = { { 180.0, TEXT("40/h"), 40 }, { 90.0, TEXT("80/h"), 60 } };
	for (const FRate& Rate : Rates)
	{
		TaxiPlanDeadlock::FRunResult Run;
		if (!TaxiPlanDeadlock::RunOne(*this, Rate.Interval, Rate.Label, /*bEnforceOrder*/ true, Run))
		{
			return false;
		}
		TestEqual(FString::Printf(TEXT("%s: no permanent deadlock"), Rate.Label), Run.Permanent, 0);
		TestEqual(FString::Printf(TEXT("%s: every admitted aircraft parked"), Rate.Label), Run.Unparked, 0);
		TestEqual(FString::Printf(TEXT("%s: every aircraft ordered off departed"), Rate.Label), Run.Undeparted, 0);
		TestTrue(FString::Printf(TEXT("%s: arrivals landed - %d of %d admitted, floor %d"), Rate.Label, Run.Admitted, Run.Scheduled,
			Rate.AdmittedFloor), Run.Admitted >= Rate.AdmittedFloor);
		TestTrue(FString::Printf(TEXT("%s: the larger airframe flew too (%d admitted)"), Rate.Label, Run.LargeAdmitted),
			Run.LargeAdmitted > 0);
		TestEqual(FString::Printf(TEXT("%s: the resolver never touched a planned aircraft"), Rate.Label), Run.PlannedResolverSets, 0);
		// NOT ASSERTED: "stood still 300 s" (logged per aircraft, "stood 300 s: ... queued="). Measured 2026-10-02 every one was
		// a departure queueing at a holding node for its runway entry, an aircraft under a test's delay, or one waiting for
		// either - a queue for the runway, not a jam (a jam is the two assertions above and the drain). Asserting none would be
		// asserting the 80/h starvation solved, which is PR 3's to measure.
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
