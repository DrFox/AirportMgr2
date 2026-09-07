#include "Tool/StandPreview.h"

#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
#include "Tool/RoadBuildTool.h"

void StandPreview::Describe(const UEntityDefinition* Definition, const FVector2D& At,
	double Heading, IToolPreviewSink& Sink)
{
	if (Definition == nullptr)
	{
		// NOT "no stand definition": this function now describes any installation, and naming
		// one of the two kinds would be wrong half the time.
		Sink.Marker(At, EPreviewStyle::Refused);
		Sink.Label(At, TEXT("nothing to place"), EPreviewStyle::Refused);
		return;
	}

	const double Cos = FMath::Cos(Heading);
	const double Sin = FMath::Sin(Heading);

	// The same local-to-world transform PlaceEntity uses.
	auto ToWorld = [&At, Cos, Sin](const FVector2D& Local)
	{
		return FVector2D(
			At.X + Local.X * Cos - Local.Y * Sin,
			At.Y + Local.X * Sin + Local.Y * Cos);
	};

	// The DESIGN AIRCRAFT first, so everything else reads against it. An aircraft parked
	// here shares the stand's pose exactly - both are measured from the nose gear.
	if (const UAircraftType* Design = Definition->DesignAircraft.Get())
	{
		TArray<FVector2D> Outline;
		UAircraftType::BuildFootprintLines(Design->Footprint, Outline);
		for (int32 Index = 0; Index + 1 < Outline.Num(); Index += 2)
		{
			Sink.Line(ToWorld(Outline[Index]), ToWorld(Outline[Index + 1]), EPreviewStyle::Snap);
		}

		// Where THIS type needs each service. A different type on the same stand puts these
		// somewhere else, which is the entire reason they live on the aircraft.
		for (const FEntityAnchor& Point : Design->ServicePoints)
		{
			Sink.Marker(ToWorld(Point.LocalPosition), EPreviewStyle::Pending);
		}
	}

	// THE INSTALLATION'S OWN BOX, for something that is not an aeroplane. A stand leaves
	// FootprintExtent at zero, because its extent IS its design aircraft's and a second
	// rectangle round it would be a second opinion about how big the thing is; a fuel depot
	// has no design aircraft and this is the only geometry it has.
	if (Definition->FootprintExtent.X > 0.0 && Definition->FootprintExtent.Y > 0.0)
	{
		const FVector2D& Extent = Definition->FootprintExtent;
		const FVector2D Corners[4] = {
			ToWorld(FVector2D(+Extent.X, +Extent.Y)),
			ToWorld(FVector2D(+Extent.X, -Extent.Y)),
			ToWorld(FVector2D(-Extent.X, -Extent.Y)),
			ToWorld(FVector2D(-Extent.X, +Extent.Y)) };
		for (int32 Corner = 0; Corner < 4; ++Corner)
		{
			Sink.Line(Corners[Corner], Corners[(Corner + 1) % 4], EPreviewStyle::Snap);
		}
	}

	// The stand's own fixtures: plant dug into the concrete, which stay put whatever parks
	// on them. Drawn with a line back to the stop mark so they read as belonging to it.
	for (const FEntityAnchor& Fixture : Definition->Anchors)
	{
		const FVector2D World = ToWorld(Fixture.LocalPosition);
		Sink.Marker(World, EPreviewStyle::Snap);
		Sink.Line(At, World, EPreviewStyle::Heal);
	}

	// The stop mark itself - the thing actually being positioned.
	Sink.Marker(At, EPreviewStyle::Pending);
}
