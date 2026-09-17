#include "Build/StandLayoutBuild.h"

#include "AirsideLog.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"

namespace
{
	/**
	 * One edge of a stand layout - a straight run, or a bend that rounds a corner.
	 *
	 * One function for both because they differ in nothing but their control point, and a bend
	 * that disagreed with its two straights about which class may use it would be a layout a
	 * truck could reach and not leave.
	 */
	FGuidelineEdge MakeLayoutEdge(FGuidelineNodeId A, FGuidelineNodeId B,
		const FVector2D& Control, FEntityInstanceId Owner, bool bReverseLeg)
	{
		FGuidelineEdge Edge;
		Edge.A = A;
		Edge.B = B;
		Edge.Control = Control;

		// GroundVehicle, plus Emergency as every derived guideline carries. NOT Aircraft: an
		// aeroplane routed round a service layout would be driving round itself.
		Edge.AllowedTraffic = FTrafficMask::Only(ETraversalClass::GroundVehicle);
		Edge.AllowedTraffic.Add(ETraversalClass::Emergency);
		// A STAND'S FOUR LEGS ARE A ONE-WAY CYCLE, and until 2026-09-17 they were laid
		// Bidirectional. Arrive, serve, reverse and depart are a SEQUENCE; laid two-way they are
		// four curves that happen to touch, and RouteSearch - which costs by length and
		// congestion, with no heading term - found the obvious shortcut. Measured: the route OFF
		// the hydrant was 11 steps and 14015 uu with ZERO reverse legs in it. It retraced the
		// serve leg it had arrived on, which means turning the body through 180 degrees on the
		// spot beside a parked aeroplane. Reported from PIE: "it just flipped 180 degrees and went
		// out forwards".
		//
		// EGuidelineDir, NOT A FLAG OF ITS OWN. The first fix for this added bOneWayAtoB to
		// FGuidelineEdge and a test for it to RouteSearch - a second statement of a fact the edge
		// already carried, and URoadNetwork::GetOutgoingGuidelines has honoured Direction all
		// along, which is the one place the search asks. Two fields that must agree are one field.
		Edge.Direction = EGuidelineDir::AToB;
		Edge.Width = FStandLayoutBuild::LaneWidth();

		// 0 is UNLIMITED - see FProfileGuideline::MaxWingspan. A span limit on a line no wing
		// uses could never bind, and the class has already refused aircraft.
		Edge.MaxWingspan = 0.0;
		Edge.bDerived = true;
		Edge.StandGeometryOwner = Owner;
		Edge.bReverseLeg = bReverseLeg;

		// AND THE OWNER IS THE WHOLE MARK. Every edge this builder lays is part of a layout,
		// and nothing else in the graph carries this entity's id - the road link a declared
		// entry casts is deliberately unowned - so "owned by this stand" and "part of this
		// stand's layout" are the same statement.
		return Edge;
	}
}

namespace
{
	/**
	 * The live node within Tolerance of Where, or unset.
	 *
	 * ONLY THE IDEMPOTENT PATH USES IT. The laying path never asks the graph where a node is -
	 * it holds the handle it made - and that is deliberate: searching by position to find a
	 * node you just created is an invitation for two nodes a fraction apart to be treated as
	 * one. The recovery path has no handle to hold, so it has to ask.
	 */
	FGuidelineNodeId NodeNear(const URoadNetwork& Network, const FVector2D& Where, double Tolerance)
	{
		const TArray<FGuidelineNode>& Nodes = Network.GetGuidelineNodes();
		for (int32 At = 0; At < Nodes.Num(); ++At)
		{
			const FGuidelineNodeId Id = Network.GuidelineNodeIdAt(At);
			if (Id.IsSet() && FVector2D::Distance(Nodes[At].Position, Where) <= Tolerance)
			{
				return Id;
			}
		}
		return FGuidelineNodeId();
	}
}

FStandLayoutBuild::FResult FStandLayoutBuild::Build(URoadNetwork& Network)
{
	FResult Result;

	// EVERY LAYOUT ALREADY IN THE GRAPH, gathered first. Both the idempotent skip below and
	// FAnchorLink's exclusion set need it, and gathering once is what keeps the two answering
	// the same question.
	{
		const TArray<FGuidelineEdge>& Edges = Network.GetGuidelineEdges();
		for (int32 At = 0; At < Edges.Num(); ++At)
		{
			const FGuidelineEdgeId Id = Network.GuidelineEdgeIdAt(At);
			if (!Id.IsSet() || !Edges[At].StandGeometryOwner.IsSet())
			{
				continue;
			}
			Result.Layouts.FindOrAdd(Edges[At].StandGeometryOwner).Add(Id);
			Result.Nodes.Add(Edges[At].A);
			Result.Nodes.Add(Edges[At].B);
		}
	}

	// By index, like FAnchorLink::Build: nothing here adds or removes an ENTITY, so holding
	// this reference across the mutations below is safe, and the handle still has to be built
	// by hand from the slot - the array elements have no stable handle of their own.
	const TArray<FEntityInstance>& Entities = Network.GetEntities();
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		const FEntityInstance& Instance = Entities[Index];
		if (!Instance.bAlive || Instance.Definition == nullptr
			|| Instance.Definition->ServiceBays.Num() == 0)
		{
			continue;
		}

		FEntityInstanceId EntityId;
		EntityId.Index = Index;
		EntityId.Generation = Instance.Generation;

		const double Cosine = FMath::Cos(Instance.Heading);
		const double Sine = FMath::Sin(Instance.Heading);
		auto ToWorld = [&Instance, Cosine, Sine](const FVector2D& Point)
		{
			return Instance.Position
				+ FVector2D(Point.X * Cosine - Point.Y * Sine,
					Point.X * Sine + Point.Y * Cosine);
		};

		if (Result.Layouts.Contains(EntityId))
		{
			// ALREADY LAID, by a pass whose output nothing swept. The entries are RECOVERED
			// rather than skipped, for the reason the lane's own RecoverEntries gave: an
			// Entries filled only by the laying path makes this result INCONSISTENT on the
			// second of two passes, so a stand laid on one and linked on the next would never
			// be joined and no log would say why.
			//
			// AND RECOVERY IS A LOOKUP HERE, not a re-derivation. An entry is authored on a
			// STRAIGHT, so a node sits exactly at the transformed pose. The lane had to re-run
			// its whole corner measurement and match the answer to within a uu, which needed
			// two separate tolerances and a paragraph explaining why they could not be one.
			TArray<FGuidelineNodeId> Recovered;
			for (const FServiceBay& Bay : Instance.Definition->ServiceBays)
			{
				for (const FVector2D& Local : { Bay.EntryLocal, Bay.ExitLocal })
				{
					const FGuidelineNodeId Found = NodeNear(Network, ToWorld(Local), 1.0);
					if (Found.IsSet())
					{
						Recovered.AddUnique(Found);
					}
				}
			}
			if (Recovered.Num() > 0)
			{
				Result.Entries.Add(EntityId, MoveTemp(Recovered));
			}
			continue;
		}

		TArray<FGuidelineEdgeId>& Laid = Result.Layouts.FindOrAdd(EntityId);
		TArray<FGuidelineNodeId> Entries;

		auto MakeNode = [&Network, &Result, &ToWorld](const FVector2D& Local)
		{
			const FGuidelineNodeId Node = Network.AddGuidelineNode(ToWorld(Local), /*bDerived=*/true);
			Result.Nodes.Add(Node);
			return Node;
		};

		/**
		 * Lay one leg between two nodes the caller names, making the ones in between.
		 *
		 * BOTH ENDS ARE PASSED IN, and that is what makes the joins a statement rather than a
		 * coincidence of arithmetic. A bay's serve leg ENDS at the anchor and its reverse leg
		 * STARTS there; those have to be ONE node or a vehicle arrives at a service point it
		 * cannot leave. Welding afterwards by position would make it true only while two
		 * independently computed values agreed to a tolerance.
		 */
		auto LayLeg = [&Network, &Laid, &Result, EntityId, &ToWorld](
			const FStandLeg& Leg, FGuidelineNodeId Start, FGuidelineNodeId End, bool bReverse)
			-> FGuidelineNodeId
		{
			if (!Leg.IsSet() || !Start.IsSet())
			{
				return FGuidelineNodeId();
			}

			FGuidelineNodeId Previous = Start;
			for (int32 At = 0; At + 1 < Leg.Points.Num(); ++At)
			{
				const bool bLast = At + 2 == Leg.Points.Num();
				const FGuidelineNodeId Next = (bLast && End.IsSet())
					? End
					: Network.AddGuidelineNode(ToWorld(Leg.Points[At + 1]), /*bDerived=*/true);

				Laid.Add(Network.AddGuidelineEdge(MakeLayoutEdge(
					Previous, Next, ToWorld(Leg.Controls[At]), EntityId, bReverse)));
				Result.Nodes.Add(Next);
				Previous = Next;
			}
			return Previous;
		};

		// ONE NODE PER AUTHORED POSE, SHARED BY EVERY BAY THAT NAMES IT - and that covers
		// ENTRIES AND EXITS TOGETHER, not exits alone.
		//
		// IT WAS EXITS ONLY UNTIL 2026-09-17, which was correct exactly while entries were one
		// per bay. They are not any more: a side's road contact is a single square pose that
		// serves as both the way in and the way out (see BuildStandTemplate), so the same
		// position is now named up to three times on one side. Making a fresh node per naming
		// put three COINCIDENT nodes on the back edge, and FAnchorLink then laid a lead-in to
		// each - measured at 309 uu against a lock of 699, and one sweep at 1 uu.
		//
		// KEYED ON THE AUTHORED POSE, which is the same value at every naming rather than two
		// values that happen to agree to a tolerance. Welding by position afterwards would make
		// the join true only while the arithmetic agreed.
		TArray<TPair<FVector2D, FGuidelineNodeId>> PoseNodes;

		auto NodeAt = [&PoseNodes, &MakeNode](const FVector2D& Local)
		{
			for (const TPair<FVector2D, FGuidelineNodeId>& Made : PoseNodes)
			{
				if (Made.Key.Equals(Local, UE_DOUBLE_KINDA_SMALL_NUMBER))
				{
					return Made.Value;
				}
			}
			const FGuidelineNodeId Fresh = MakeNode(Local);
			PoseNodes.Emplace(Local, Fresh);
			return Fresh;
		};

		for (const FServiceBay& Bay : Instance.Definition->ServiceBays)
		{
			// THE ANCHOR'S OWN NODE, made at placement, and FuelService routes to THAT handle -
			// a layout that laid a fresh node at the same position would look right in the
			// overlay and route nothing.
			FGuidelineNodeId ServiceNode;
			for (const FResolvedAnchor& Resolved : Instance.ResolvedAnchors)
			{
				if (Resolved.Id == Bay.AnchorId)
				{
					ServiceNode = Resolved.Node;
					break;
				}
			}
			if (!ServiceNode.IsSet())
			{
				UE_LOG(LogAirside, Warning,
					TEXT("Stand layout names anchor '%s', which the instance does not have. "
					     "Skipping its bay; nothing will route to that service."),
					*Bay.AnchorId.ToString());
				continue;
			}

			const FGuidelineNodeId ExitNode = NodeAt(Bay.ExitLocal);
			const FGuidelineNodeId EntryNode = NodeAt(Bay.EntryLocal);

			// THE PARK POSE IS NOT SHARED, and is made rather than looked up. Two bays have
			// their own slots by construction, and a layout that ever authored two at one
			// position wants that seen as two nodes on top of each other rather than silently
			// merged into a bay serving two anchors.
			const FGuidelineNodeId ParkNode = MakeNode(Bay.ParkLocal);

			LayLeg(Bay.ArriveLeg, EntryNode, ParkNode, /*bReverse=*/false);
			LayLeg(Bay.ServeLeg, ParkNode, ServiceNode, /*bReverse=*/false);

			// THE REVERSE LEG IS MARKED, so a consumer can tell which edges are judged by which
			// limit. A reverse leg is legitimately tighter than the forward limit, and judging
			// it by the forward rule is a false refusal - see FGuidelineEdge::bReverseLeg.
			const FGuidelineNodeId Cleared =
				LayLeg(Bay.ReverseLeg, ServiceNode, FGuidelineNodeId(), /*bReverse=*/true);
			LayLeg(Bay.DepartLeg, Cleared, ExitNode, /*bReverse=*/false);

			Entries.AddUnique(EntryNode);
			Entries.AddUnique(ExitNode);
		}

		if (Entries.Num() > 0)
		{
			Result.Entries.Add(EntityId, MoveTemp(Entries));
		}
		++Result.LayoutsBuilt;
	}

	if (Result.LayoutsBuilt > 0)
	{
		// One census line, beside the guideline builder's and FAnchorLink's. Zero layouts is
		// the common idle rebuild and stays quiet.
		//
		// THE ENTRY COUNT IS IN IT because nothing in this pass reads Result.Entries, so the
		// log is the only evidence the declared entries reached the graph at all - and a layout
		// laid with none of them is a stand no road can be joined to.
		int32 EntryNodes = 0;
		for (const TPair<FEntityInstanceId, TArray<FGuidelineNodeId>>& Entry : Result.Entries)
		{
			EntryNodes += Entry.Value.Num();
		}
		UE_LOG(LogAirside, Log, TEXT("Stand layouts: %d laid, %d entry/exit node(s)"),
			Result.LayoutsBuilt, EntryNodes);
	}

	return Result;
}
