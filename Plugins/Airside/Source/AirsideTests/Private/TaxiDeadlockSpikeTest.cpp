// THROWAWAY SPIKE (2026-10-02, branch feature/taxi-deadlock-spike) - NOT FOR MERGE.
// Measures head-on taxi deadlocks on the user's M_ScaleGatwick layout under three admission
// strategies, all implemented HERE in the harness (no model change):
//   0 Baseline   - today's code.
//   1 DirLocks   - each two-way taxiway guideline edge lockable in one direction; an aircraft is
//                  admitted (arrival dispatched / departure pushed) only when every edge of its
//                  predicted route is free of opposite-direction locks; locks release behind the tail.
//   2 SpaceTime  - the same, but each edge is reserved only for a predicted TIME WINDOW (nominal speed,
//                  slack factor + margin); opposite directions may share an edge at different times.
//                  Crude: no in-route waits and no per-resource order enforcement (Hoenig 2019's ADG).
// Output is LogAirsideTests "Spike:" lines; the tests assert only that the map loaded.

#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "AirsideTestsLog.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Model/ArrivalPlanner.h"
#include "Model/DeparturePlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Model/RoadEntity.h"
#include "Present/RoadNetworkActor.h"
#include "Present/AirsideTraffic.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace SpikeTaxi
{
	enum class EStrategy : uint8 { Baseline, DirLocks, SpaceTime };

	const TCHAR* StrategyName(EStrategy S)
	{
		switch (S)
		{
		case EStrategy::Baseline:  return TEXT("Baseline");
		case EStrategy::DirLocks:  return TEXT("DirLocks");
		case EStrategy::SpaceTime: return TEXT("SpaceTime");
		}
		return TEXT("?");
	}

	/** Counts deadlock lines by member set. Unbuffered (memory: log spies must be). */
	struct FDeadlockSpy : public FOutputDevice
	{
		TMap<FString, int32> NoTurnBySet;
		TMap<FString, int32> ResolvedBySet;
		TMap<FString, int32> ReservationBySet;
		TMap<FString, double> LastSeen;
		TMap<FString, double> FirstSeen;
		double Now = 0.0;

		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
		virtual bool CanBeUsedOnAnyThread() const override { return true; }

		static FString SetOf(const FString& Line)
		{
			int32 Open = INDEX_NONE, Close = INDEX_NONE;
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
			if (Line.Contains(TEXT("no member can turn")))
			{
				const FString S = SetOf(Line);
				NoTurnBySet.FindOrAdd(S)++;
				LastSeen.Add(S, Now);
				if (!FirstSeen.Contains(S)) { FirstSeen.Add(S, Now); }
			}
			else if (Line.Contains(TEXT("eadlock among agents")) && Line.Contains(TEXT("resolved")))
			{
				const FString S = SetOf(Line);
				ResolvedBySet.FindOrAdd(S)++;
				LastSeen.Add(S, Now);
				if (!FirstSeen.Contains(S)) { FirstSeen.Add(S, Now); }
			}
			else if (Line.Contains(TEXT("Reservation cycle among agents")))
			{
				const FString S = SetOf(Line);
				ReservationBySet.FindOrAdd(S)++;
				LastSeen.Add(S, Now);
			}
		}
	};

	struct FAgentStat
	{
		int32 Id = 0;
		double Dispatched = 0.0;   // admitted
		double ScheduledAt = 0.0;  // when the schedule wanted it
		double FirstParked = -1.0;
		double DepartOrderAt = -1.0; // dwell elapsed; harness wants to depart from here
		double DepartAdmitted = -1.0;
		double DepartingAt = -1.0;
		double TaxiIn = 0.0, TaxiInStopped = 0.0;
		double TaxiOut = 0.0, TaxiOutStopped = 0.0;
		double StoppedRun = 0.0, MaxStoppedRun = 0.0;
		bool bGone = false;
		bool bInbound = true;
	};

	struct FLock
	{
		int32 AgentId = 0;
		bool bReversed = false;
		double From = 0.0, To = 1e30; // time window (DirLocks: whole life)
		double EndAlong = 0.0;        // distance along the agent's plan where this edge ends
		double PlanLength = 0.0;      // which plan the EndAlong is measured on
	};

	struct FSim
	{
		EStrategy Strategy = EStrategy::Baseline;
		URoadNetwork* Net = nullptr;
		UGroundTraffic* Traffic = nullptr;
		FAirframe Airframe;
		TArray<FVector2D> RunwayNear;
		double Now = 0.0;

		// Tunables
		double Dt = 1.0 / 30.0;
		double Duration = 7200.0;
		double ArrivalInterval = 180.0; // 20 arrivals/h -> ~40 movements/h at steady state
		double Dwell = 1200.0;          // 20 min, SHORTENED from a real turnaround
		double NominalSpeed = 800.0;    // uu/s, for SpaceTime; overwritten from the baseline measure if given
		double ArrivalLead = 90.0;      // s from dispatch to taxi start, SpaceTime
		double Slack = 0.5;             // SpaceTime: window end stretched by (1+Slack)
		double Margin = 20.0;           // s either side

		TMap<int32, FAgentStat> Stats;
		TArray<double> PendingArrivals; // scheduled times not yet admitted
		TMap<int32, TArray<FLock>> LocksByEdge; // guideline edge index -> locks
		int32 ArrivalRefusals = 0, DepartRefusalsPushBlocked = 0, DepartRefusalsOther = 0;
		int32 AdmissionWaitsArr = 0, AdmissionWaitsDep = 0;
		int32 LockViolations = 0;
		double ArrivalDelaySum = 0.0; int32 ArrivalDelayN = 0;
		int32 MaxTaxiing = 0;
		TArray<double> TickMs;
		double MovingSpeedSum = 0.0, MovingSpeedN = 0.0;
		double ArrLeadSum = 0.0; int32 ArrLeadN = 0;

		bool IsLockableEdge(int32 EdgeIndex) const
		{
			const TArray<FGuidelineEdge>& Edges = Net->GetGuidelineEdges();
			if (!Edges.IsValidIndex(EdgeIndex))
			{
				return false;
			}
			const FGuidelineEdge& E = Edges[EdgeIndex];
			if (E.Direction != EGuidelineDir::Bidirectional || !E.DerivedFrom.IsSet())
			{
				return false;
			}
			return !Net->IsRunwaySegment(E.DerivedFrom);
		}

		/** Windows the route would need, as (edge index, reversed, from, to, endAlong). */
		struct FNeed { int32 Edge; bool bRev; double From; double To; double EndAlong; };

		void NeedsFor(const FRoutePlan& Plan, double StartAt, TArray<FNeed>& Out) const
		{
			Out.Reset();
			double Along = 0.0;
			for (const FRouteStep& Step : Plan.Steps)
			{
				const double Begin = Along;
				Along = Step.EndDistance;
				const int32 Index = Step.Edge.IsSet() ? Step.Edge.Index : INDEX_NONE;
				if (!IsLockableEdge(Index))
				{
					continue;
				}
				FNeed N;
				N.Edge = Index;
				N.bRev = Step.bReversed;
				N.EndAlong = Along;
				if (Strategy == EStrategy::DirLocks)
				{
					N.From = Now; N.To = 1e30;
				}
				else
				{
					N.From = StartAt + Begin / NominalSpeed - Margin;
					N.To = StartAt + (Along / NominalSpeed) * (1.0 + Slack) + Margin;
				}
				Out.Add(N);
			}
		}

		bool Free(const TArray<FNeed>& Needs, int32 Self) const
		{
			for (const FNeed& N : Needs)
			{
				if (const TArray<FLock>* Locks = LocksByEdge.Find(N.Edge))
				{
					for (const FLock& L : *Locks)
					{
						if (L.AgentId != Self && L.bReversed != N.bRev && L.From < N.To && N.From < L.To)
						{
							return false;
						}
					}
				}
			}
			return true;
		}

		void Take(const TArray<FNeed>& Needs, int32 Id, double PlanLength)
		{
			if (!Free(Needs, Id))
			{
				++LockViolations;
			}
			for (const FNeed& N : Needs)
			{
				FLock L;
				L.AgentId = Id; L.bReversed = N.bRev; L.From = N.From; L.To = N.To;
				L.EndAlong = N.EndAlong; L.PlanLength = PlanLength;
				LocksByEdge.FindOrAdd(N.Edge).Add(L);
			}
		}

		void ReleaseAll(int32 Id)
		{
			for (TPair<int32, TArray<FLock>>& Pair : LocksByEdge)
			{
				Pair.Value.RemoveAll([Id](const FLock& L) { return L.AgentId == Id; });
			}
		}

		void ReleaseBehind(const FRoadAgent& Agent)
		{
			if (Agent.Phase != EAgentPhase::Taxiing)
			{
				return;
			}
			const FRoutePlan& Plan = Agent.PlanInProgress();
			const double Tail = Agent.DistanceAlongPlan() - 3000.0; // ~ a body length behind
			for (TPair<int32, TArray<FLock>>& Pair : LocksByEdge)
			{
				Pair.Value.RemoveAll([&](const FLock& L)
				{
					return L.AgentId == Agent.Id && FMath::IsNearlyEqual(L.PlanLength, Plan.Length, 1.0) && L.EndAlong < Tail;
				});
			}
		}

		/** A plan the model is following that the locks do not describe (a replan): re-lock it, counting violations. */
		void SyncLocksToPlan(const FRoadAgent& Agent)
		{
			if (Agent.Phase != EAgentPhase::Taxiing)
			{
				return;
			}
			const FRoutePlan& Plan = Agent.PlanInProgress();
			bool bAny = false, bMatches = false;
			for (const TPair<int32, TArray<FLock>>& Pair : LocksByEdge)
			{
				for (const FLock& L : Pair.Value)
				{
					if (L.AgentId == Agent.Id)
					{
						bAny = true;
						bMatches |= FMath::IsNearlyEqual(L.PlanLength, Plan.Length, 1.0);
					}
				}
			}
			if (bAny && !bMatches)
			{
				ReleaseAll(Agent.Id);
				TArray<FNeed> Needs;
				const double Started = Now - Agent.DistanceAlongPlan() / NominalSpeed;
				NeedsFor(Plan, Started, Needs);
				// drop edges already behind
				const double Tail = Agent.DistanceAlongPlan() - 3000.0;
				Needs.RemoveAll([Tail](const FNeed& N) { return N.EndAlong < Tail; });
				Take(Needs, Agent.Id, Plan.Length);
			}
		}

		void TryArrivals()
		{
			for (int32 I = 0; I < PendingArrivals.Num();)
			{
				const double Sched = PendingArrivals[I];
				const FVector2D Near = RunwayNear[(static_cast<int32>(Sched / ArrivalInterval)) % RunwayNear.Num()];
				TArray<FNeed> Needs;
				double PlanLength = 0.0;
				if (Strategy != EStrategy::Baseline)
				{
					const FArrivalPlan Plan = ArrivalPlanner::Plan(*Net, Near, Airframe, &Traffic->GetOccupancy());
					if (!Plan.IsValid())
					{
						++ArrivalRefusals;
						break; // FIFO; retry next second
					}
					NeedsFor(Plan.TaxiIn, Now + ArrivalLead, Needs);
					if (!Free(Needs, 0))
					{
						++AdmissionWaitsArr;
						break;
					}
					PlanLength = Plan.TaxiIn.Length;
				}
				const int32 Id = Traffic->DispatchArrival(*Net, Near, Airframe, 1.0);
				if (Id <= 0)
				{
					++ArrivalRefusals;
					break;
				}
				FAgentStat& S = Stats.Add(Id);
				S.Id = Id; S.Dispatched = Now; S.ScheduledAt = Sched;
				ArrivalDelaySum += Now - Sched; ++ArrivalDelayN;
				if (Strategy != EStrategy::Baseline)
				{
					Take(Needs, Id, PlanLength);
				}
				PendingArrivals.RemoveAt(I);
			}
		}

		void TryDeparture(FAgentStat& S, const FRoadAgent& Agent)
		{
			TArray<FNeed> Needs;
			if (Strategy != EStrategy::Baseline)
			{
				const FDeparturePlan Predicted = DeparturePlanner::PlanAny(*Net, Agent.GoalNode, Airframe,
					ETraversalClass::Aircraft, &Traffic->GetOccupancy());
				if (Predicted.Route.IsValid())
				{
					NeedsFor(Predicted.Route, Now + 60.0, Needs); // ~a push before the taxi
					if (!Free(Needs, S.Id))
					{
						++AdmissionWaitsDep;
						return;
					}
				}
			}
			const EDepartureRefusal Why = Traffic->DepartAgent(S.Id, *Net);
			if (Why == EDepartureRefusal::None)
			{
				S.DepartAdmitted = Now;
				S.bInbound = false;
				if (Strategy != EStrategy::Baseline)
				{
					ReleaseAll(S.Id);
					const FRoadAgent* After = Traffic->FindAgent(S.Id);
					if (After)
					{
						// Lock the push AND the taxi out the model actually chose.
						TArray<FNeed> Push, Taxi;
						if (After->Phase == EAgentPhase::Manoeuvring)
						{
							NeedsFor(After->Pushback.Plan, Now, Push);
							NeedsFor(After->TaxiOutPlan, Now + 60.0, Taxi);
							Take(Push, S.Id, After->Pushback.Plan.Length);
							Take(Taxi, S.Id, After->TaxiOutPlan.Length);
						}
						else
						{
							NeedsFor(After->PlanInProgress(), Now, Taxi);
							Take(Taxi, S.Id, After->PlanInProgress().Length);
						}
					}
				}
			}
			else if (Why == EDepartureRefusal::PushbackBlocked)
			{
				++DepartRefusalsPushBlocked;
			}
			else
			{
				++DepartRefusalsOther;
			}
		}

		void Run(FDeadlockSpy& Spy)
		{
			for (double T = 0.0; T < Duration; T += ArrivalInterval)
			{
				PendingArrivals.Add(T);
			}
			PendingArrivals.Sort();

			double NextAdmit = 0.0;
			const int32 Ticks = FMath::CeilToInt32(Duration / Dt);
			TArray<double> Scheduled = PendingArrivals;
			PendingArrivals.Reset();
			int32 NextSched = 0;
			for (int32 Tick = 0; Tick < Ticks; ++Tick)
			{
				Now = Tick * Dt;
				Spy.Now = Now;
				while (NextSched < Scheduled.Num() && Scheduled[NextSched] <= Now)
				{
					PendingArrivals.Add(Scheduled[NextSched++]);
				}
				if (Now >= NextAdmit)
				{
					NextAdmit = Now + 1.0;
					TryArrivals();
					for (TPair<int32, FAgentStat>& Pair : Stats)
					{
						FAgentStat& S = Pair.Value;
						const FRoadAgent* Agent = Traffic->FindAgent(S.Id);
						if (!Agent || S.bGone || !S.bInbound || Agent->Phase != EAgentPhase::Parked || S.FirstParked < 0.0)
						{
							continue;
						}
						if (Now - S.FirstParked >= Dwell && (S.DepartOrderAt < 0.0 || FMath::Fmod(Now, 5.0) < 1.0))
						{
							if (S.DepartOrderAt < 0.0) { S.DepartOrderAt = Now; }
							TryDeparture(S, *Agent);
						}
					}
				}

				const double Begin = FPlatformTime::Seconds();
				Traffic->Advance(Dt, Net);
				TickMs.Add((FPlatformTime::Seconds() - Begin) * 1000.0);

				int32 Taxiing = 0;
				for (TPair<int32, FAgentStat>& Pair : Stats)
				{
					FAgentStat& S = Pair.Value;
					if (S.bGone)
					{
						continue;
					}
					const FRoadAgent* Agent = Traffic->FindAgent(S.Id);
					if (!Agent)
					{
						S.bGone = true;
						ReleaseAll(S.Id);
						continue;
					}
					const bool bMoving = Agent->Phase == EAgentPhase::Taxiing || Agent->Phase == EAgentPhase::Manoeuvring;
					if (Agent->Phase == EAgentPhase::Parked && S.FirstParked < 0.0)
					{
						S.FirstParked = Now;
						ReleaseAll(S.Id);
					}
					if (Agent->Phase == EAgentPhase::Taxiing && S.bInbound && ArrLeadN < 100000 && S.TaxiIn == 0.0)
					{
						ArrLeadSum += Now - S.Dispatched; ++ArrLeadN;
					}
					if (Agent->Phase == EAgentPhase::Departing && S.DepartingAt < 0.0)
					{
						S.DepartingAt = Now;
						ReleaseAll(S.Id);
					}
					if (bMoving)
					{
						++Taxiing;
						const double Speed = FMath::Abs(Agent->SpeedAlongPlan());
						const bool bStopped = Speed < 1.0;
						(S.bInbound ? S.TaxiIn : S.TaxiOut) += Dt;
						if (bStopped)
						{
							(S.bInbound ? S.TaxiInStopped : S.TaxiOutStopped) += Dt;
							S.StoppedRun += Dt;
							S.MaxStoppedRun = FMath::Max(S.MaxStoppedRun, S.StoppedRun);
						}
						else
						{
							S.StoppedRun = 0.0;
							if (Agent->Phase == EAgentPhase::Taxiing) { MovingSpeedSum += Speed; MovingSpeedN += 1.0; }
						}
						if (Strategy != EStrategy::Baseline)
						{
							SyncLocksToPlan(*Agent);
							ReleaseBehind(*Agent);
						}
					}
				}
				MaxTaxiing = FMath::Max(MaxTaxiing, Taxiing);
			}
		}

		static double Pct(TArray<double> V, double P)
		{
			if (V.Num() == 0) { return 0.0; }
			V.Sort();
			return V[FMath::Clamp(FMath::FloorToInt32(P * (V.Num() - 1)), 0, V.Num() - 1)];
		}
		static double Mean(const TArray<double>& V)
		{
			double Sum = 0.0; for (double X : V) { Sum += X; } return V.Num() ? Sum / V.Num() : 0.0;
		}

		void Report(const FDeadlockSpy& Spy, const TCHAR* Label) const
		{
			int32 Arrived = 0, Departed = 0, Stuck300 = 0, StillOnGround = 0;
			TArray<double> In, InStop, Out, OutStop, DepWait;
			for (const TPair<int32, FAgentStat>& Pair : Stats)
			{
				const FAgentStat& S = Pair.Value;
				if (S.FirstParked >= 0.0) { ++Arrived; In.Add(S.TaxiIn); InStop.Add(S.TaxiInStopped); }
				if (S.DepartingAt >= 0.0) { ++Departed; Out.Add(S.TaxiOut); OutStop.Add(S.TaxiOutStopped); }
				if (S.DepartAdmitted >= 0.0 && S.DepartOrderAt >= 0.0) { DepWait.Add(S.DepartAdmitted - S.DepartOrderAt); }
				if (S.MaxStoppedRun >= 300.0) { ++Stuck300; }
				if (!S.bGone) { ++StillOnGround; }
			}
			// Episodes: distinct member sets. Permanent: still logging "no member can turn" in the last 120 s.
			TSet<FString> Sets;
			for (const auto& P : Spy.NoTurnBySet) { Sets.Add(P.Key); }
			for (const auto& P : Spy.ResolvedBySet) { Sets.Add(P.Key); }
			int32 Permanent = 0, Resolved = 0, NoTurnLines = 0, ResolvedLines = 0;
			for (const FString& S : Sets)
			{
				const double* Last = Spy.LastSeen.Find(S);
				if (Spy.NoTurnBySet.Contains(S) && Last && *Last > Now - 120.0) { ++Permanent; }
				if (Spy.ResolvedBySet.Contains(S)) { ++Resolved; }
			}
			for (const auto& P : Spy.NoTurnBySet) { NoTurnLines += P.Value; }
			for (const auto& P : Spy.ResolvedBySet) { ResolvedLines += P.Value; }
			int32 ReservationLines = 0;
			for (const auto& P : Spy.ReservationBySet) { ReservationLines += P.Value; }

			UE_LOG(LogAirsideTests, Display,
				TEXT("Spike: [%s %s] admitted=%d arrived(parked)=%d departed=%d stillOnGround=%d pendingArrivals=%d | ")
				TEXT("deadlockSets=%d resolvedSets=%d permanentSets=%d noTurnLines=%d resolvedLines=%d reservationCycleLines=%d | stuck>=300s=%d | ")
				TEXT("taxiIn mean=%.0f p95=%.0f stopped mean=%.0f | taxiOut mean=%.0f p95=%.0f stopped mean=%.0f | depWait mean=%.0f p95=%.0f | arrDelay mean=%.0f | ")
				TEXT("maxTaxiing=%d | tick ms mean=%.3f p95=%.3f max=%.3f | arrRefusals=%d pushBlocked=%d depOther=%d admitWaitArr=%d admitWaitDep=%d lockViolations=%d | movingSpeed=%.0f arrLead=%.0f"),
				StrategyName(Strategy), Label, Stats.Num(), Arrived, Departed, StillOnGround, PendingArrivals.Num(),
				Sets.Num(), Resolved, Permanent, NoTurnLines, ResolvedLines, ReservationLines, Stuck300,
				Mean(In), Pct(In, 0.95), Mean(InStop), Mean(Out), Pct(Out, 0.95), Mean(OutStop), Mean(DepWait), Pct(DepWait, 0.95),
				ArrivalDelayN ? ArrivalDelaySum / ArrivalDelayN : 0.0,
				MaxTaxiing, Mean(TickMs), Pct(TickMs, 0.95), Pct(TickMs, 1.0),
				ArrivalRefusals, DepartRefusalsPushBlocked, DepartRefusalsOther, AdmissionWaitsArr, AdmissionWaitsDep, LockViolations,
				MovingSpeedN > 0.0 ? MovingSpeedSum / MovingSpeedN : 0.0, ArrLeadN ? ArrLeadSum / ArrLeadN : 0.0);
			for (const auto& P : Spy.FirstSeen)
			{
				UE_LOG(LogAirsideTests, Display, TEXT("Spike:   set [%s] first %.0f s last %.0f s noTurn=%d resolved=%d"), *P.Key, P.Value,
					Spy.LastSeen.FindRef(P.Key), Spy.NoTurnBySet.FindRef(P.Key), Spy.ResolvedBySet.FindRef(P.Key));
			}
			for (const TPair<int32, FAgentStat>& Pair : Stats)
			{
				const FAgentStat& S = Pair.Value;
				const FRoadAgent* A = Traffic->FindAgent(S.Id);
				if (A && S.MaxStoppedRun >= 300.0)
				{
					const FVector2D P = A->GroundPosition();
					UE_LOG(LogAirsideTests, Display, TEXT("Spike:   stuck agent %d %s phase=%d at (%.0f, %.0f) waitingOn=%d stoppedRun=%.0f since %.0f"),
						S.Id, S.bInbound ? TEXT("IN") : TEXT("OUT"), static_cast<int32>(A->Phase), P.X, P.Y, A->GetWaitingOn(), S.StoppedRun, Now - S.StoppedRun);
				}
			}
		}
	};

	/** The saved network actor in M_ScaleGatwick, or null. */
	ARoadNetworkActor* LoadSavedActor()
	{
		UPackage* Package = LoadPackage(nullptr, TEXT("/Game/Maps/M_ScaleGatwick"), LOAD_None);
		UWorld* World = Package ? UWorld::FindWorldInPackage(Package) : nullptr;
		if (!World || !World->PersistentLevel)
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

	bool RunOne(FAutomationTestBase& Test, EStrategy Strategy, double ArrivalInterval, const TCHAR* Label)
	{
		ARoadNetworkActor* Saved = LoadSavedActor();
		if (!Test.TestNotNull(TEXT("M_ScaleGatwick's road network actor loaded"), Saved) || !Saved->Network)
		{
			return false;
		}

		ARoadNetworkActor* Actor = NewObject<ARoadNetworkActor>(GetTransientPackage());
		Actor->PlaceNode(FVector2D(-2000000.0, -2000000.0));
		Actor->Network->RestoreFrom(*Saved->Network);
		Actor->TrafficRules = Saved->TrafficRules;
		Actor->RebuildMesh();
		URoadNetwork* Net = Actor->Network.Get();

		int32 Nodes = 0, Segments = 0, RunwaySegs = 0, Stands = 0;
		for (const FRoadNode& N : Net->GetNodes()) { Nodes += N.bAlive ? 1 : 0; }
		TMap<int32, TPair<FVector2D, int32>> RunwayMid; // chain head index -> (sum, count)
		for (int32 I = 0; I < Net->GetSegments().Num(); ++I)
		{
			const FRoadSegment& S = Net->GetSegments()[I];
			if (!S.bAlive) { continue; }
			++Segments;
			const FRoadSegmentId Id = Net->SegmentIdAt(I);
			if (Net->IsRunwaySegment(Id))
			{
				++RunwaySegs;
				const TArray<FRoadSegmentId> Chain = Net->RunwayChainOrSeed(Id);
				int32 Head = MAX_int32;
				for (const FRoadSegmentId& C : Chain) { Head = FMath::Min(Head, static_cast<int32>(C.Index)); }
				const FVector2D Mid = (Net->GetNode(S.A)->Position + Net->GetNode(S.B)->Position) * 0.5;
				TPair<FVector2D, int32>& Acc = RunwayMid.FindOrAdd(Head, TPair<FVector2D, int32>(FVector2D::ZeroVector, 0));
				Acc.Key += Mid; Acc.Value += 1;
			}
		}
		for (const FEntityInstance& E : Net->GetEntities()) { Stands += E.IsStandCandidate() ? 1 : 0; }
		UE_LOG(LogAirsideTests, Display, TEXT("Spike: [%s] map loaded: %d nodes, %d segments, %d runway segments in %d runways, %d stands, %d guideline edges"),
			Label, Nodes, Segments, RunwaySegs, RunwayMid.Num(), Stands, Net->GetGuidelineEdges().Num());

		FSim Sim;
		Sim.Strategy = Strategy;
		Sim.Net = Net;
		Sim.Traffic = Actor->GetTraffic()->GetModel();
		Sim.Traffic->Rules = Saved->TrafficRules;
		Sim.Airframe = TestAirframes::Piper();
		Sim.ArrivalInterval = ArrivalInterval;
		for (const auto& Pair : RunwayMid) { Sim.RunwayNear.Add(Pair.Value.Key / Pair.Value.Value); }
		if (!Test.TestTrue(TEXT("found runways"), Sim.RunwayNear.Num() > 0) || !Test.TestTrue(TEXT("found stands"), Stands > 0))
		{
			return false;
		}

		FDeadlockSpy Spy;
		GLog->AddOutputDevice(&Spy);
		Sim.Run(Spy);
		GLog->RemoveOutputDevice(&Spy);
		Sim.Report(Spy, Label);
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpikeTaxiBaseline40,
	"Airside.Spike.TaxiDeadlock.Baseline40",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSpikeTaxiBaseline40::RunTest(const FString& Parameters)
{
	return SpikeTaxi::RunOne(*this, SpikeTaxi::EStrategy::Baseline, 180.0, TEXT("40/h"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpikeTaxiDirLocks40,
	"Airside.Spike.TaxiDeadlock.DirLocks40",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSpikeTaxiDirLocks40::RunTest(const FString& Parameters)
{
	return SpikeTaxi::RunOne(*this, SpikeTaxi::EStrategy::DirLocks, 180.0, TEXT("40/h"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpikeTaxiSpaceTime40,
	"Airside.Spike.TaxiDeadlock.SpaceTime40",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSpikeTaxiSpaceTime40::RunTest(const FString& Parameters)
{
	return SpikeTaxi::RunOne(*this, SpikeTaxi::EStrategy::SpaceTime, 180.0, TEXT("40/h"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpikeTaxiBaseline80,
	"Airside.Spike.TaxiDeadlock.Baseline80",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSpikeTaxiBaseline80::RunTest(const FString& Parameters)
{
	return SpikeTaxi::RunOne(*this, SpikeTaxi::EStrategy::Baseline, 90.0, TEXT("80/h"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpikeTaxiDirLocks80,
	"Airside.Spike.TaxiDeadlock.DirLocks80",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSpikeTaxiDirLocks80::RunTest(const FString& Parameters)
{
	return SpikeTaxi::RunOne(*this, SpikeTaxi::EStrategy::DirLocks, 90.0, TEXT("80/h"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpikeTaxiSpaceTime80,
	"Airside.Spike.TaxiDeadlock.SpaceTime80",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSpikeTaxiSpaceTime80::RunTest(const FString& Parameters)
{
	return SpikeTaxi::RunOne(*this, SpikeTaxi::EStrategy::SpaceTime, 90.0, TEXT("80/h"));
}


#endif
