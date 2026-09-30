#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"

class URoadNetwork;

/**
 * WHAT THE PLAYER CALLS A THING, where the ops layer has to say it (#447). One function per thing, so the wording cannot be
 * decided in six places that then disagree - which the stand's name was: the stand card and the digits painted at its turn-off
 * said StandNumber, while the depot card's backlog and the JobUnserviceable alert printed the ENTITY INDEX ("No fuel for stand 0"
 * beside a sign reading 4).
 *
 * A VEHICLE'S NAME is FServiceFleet::NameOf (#430, #483) and needs no sibling here: the kind's name and the vehicle's id are the
 * catalogue's and the board's to say. A STAND's number lives on the network's entity, which the job board does not hold - so it
 * takes the network per call, the way the board's other readers do.
 */
namespace OpsNames
{
	/**
	 * The stand's number as the player reads it: FEntityInstance::StandNumber, 1..N in placement order, never reused, painted on the
	 * ground. NOT the entity index, which RoadSlot recycles into a different stand after a delete and which starts at 0.
	 *
	 * FALLS BACK TO THE INDEX, and says so once in the log, only when there is no number to give: no network (a world-free test
	 * of a board), an unset or dead handle, or a stand that reached here unnumbered - which PlaceEntity and PostLoad's backfill
	 * prevent, so the index there is the broken-invariant case InspectFacts::DestinationOf also names, not a normal answer.
	 * ENFORCED BY: AirportOps.Model.StandLabel.AlertBacklogAndCardSayTheSameNumber (the number, through the alert, the backlog and the
	 * card), Airside.Model.StandNumbers (placement and the backfill give every stand one)
	 */
	AIRPORTOPS_API FString StandLabel(const URoadNetwork* Network, FEntityInstanceId Stand);
}
