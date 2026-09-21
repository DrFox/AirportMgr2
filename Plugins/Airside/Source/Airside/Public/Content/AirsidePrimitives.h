#pragma once

#include "CoreMinimal.h"

/**
 * Engine-owned primitive asset paths, named once (issue #192 item 3).
 *
 * NOT AN AUTHORED ASSET, for the reason ARoadAgentActor's placeholder and
 * ARoadNetworkActor's plot boxes each defend at their own FObjectFinder: grey-box geometry
 * that shows only until real content arrives has no business owning a .uasset of its own.
 * The path itself was typed at three call sites with that same one-line defence copied
 * three times - the "engine primitive path typed three times" finding - which is what a
 * fourth site would have repeated a fourth time. Naming it here follows Content/'s own
 * rule: resolve a content default in exactly one function, so a path that ever needs to
 * change (an engine version renaming its own shapes) has exactly one call site to fix.
 */
namespace AirsidePrimitives
{
	/** "/Engine/BasicShapes/Cube.Cube" - see the namespace's own comment. */
	AIRSIDE_API const TCHAR* CubePath();
}
