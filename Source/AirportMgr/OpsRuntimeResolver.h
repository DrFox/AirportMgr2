#pragma once

#include "CoreMinimal.h"

class UOpsRuntime;
class UWorld;

/**
 * THE ONE DOOR TO THE OPS RUNTIME for the game module (#448): "which runtime does this world play?" is answered here. ENFORCED BY: Check-Architecture rule 52.
 *
 * It was answered in five places that did not see each other - the controller's GetOpsRuntime (with its own test override), a
 * handful of controller bypasses that skipped that override, FBuildActionContext's constructor, the bar's UseForTest runtime,
 * and nine widgets calling the game-instance subsystem directly. In a headless test the depot verbs saw the override while
 * the bar, the inspector and the context did not, so a test that handed one place a runtime silently left the rest with none.
 *
 * KEYED BY WORLD, because the subsystem is: the runtime is the game instance's, the game instance is the world's, and a widget
 * or a controller or a verb all know their world. A test world has no game instance, so SetOverrideForTest stands a runtime in
 * for it - for that one world, for every reader at once.
 *
 * ENFORCED BY: Check-Architecture rule 52 (UOpsRuntimeSubsystem::Get is called in OpsRuntimeResolver.cpp alone);
 * AirportMgr.Actions.OneResolverForTheOpsRuntime (an override reaches the context and the bar), AirportMgr.Inspector.DepotCardBuysThroughTheController
 * (and the inspector, whose panel finds the depot's runtime there and nowhere else)
 */
namespace OpsRuntimeResolver
{
	/** The runtime World plays: the test override if one was set for it, else the game instance's. Null for none - an editor
	 *  world, a headless test with no override, or a null World. */
	UOpsRuntime* Resolve(const UWorld* World);

	/**
	 * Stands Runtime in for World's game instance, for every reader of Resolve. Null removes it. WEAK on both sides: a world
	 * that is torn down takes its override with it, so a test that forgets to clear it cannot leak into the next.
	 */
	void SetOverrideForTest(const UWorld* World, UOpsRuntime* Runtime);
}
