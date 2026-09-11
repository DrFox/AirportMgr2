#include "Entities/EntityDefinition.h"

#include "Model/RoadNetwork.h"

UEntityDefinition* UEntityDefinition::MakeStandTransient()
{
	// A stand with no design aircraft draws no envelope, offers no service positions and -
	// since the service loop is laid out around the aeroplane - gets no lane either, which in
	// a test reads as "the feature is broken" rather than "the fixture is thin".
	//
	// BUILT FIRST now, because the layout is measured FROM it: this used to be set after
	// BuildCodeCStand had already run, which was harmless only for as long as nothing in the
	// layout depended on it.
	UAircraftType* A320 = NewObject<UAircraftType>(GetTransientPackage());
	UAircraftType::BuildA320(A320);

	UEntityDefinition* Definition = NewObject<UEntityDefinition>(GetTransientPackage());
	BuildCodeCStand(Definition, A320);

	return Definition;
}

void UEntityDefinition::BuildCodeCStand(UEntityDefinition* Definition, UAircraftType* Aircraft)
{
	if (Definition == nullptr)
	{
		return;
	}

	Definition->Anchors.Reset();

	// SET HERE, not by the caller afterwards - see the header. The service loop below is
	// measured from this aeroplane, so the builder that lays the ground round it has to know
	// which aeroplane that is.
	Definition->DesignAircraft = Aircraft;

	// A Code C contact stand: the ground half of a turnaround.
	//
	// ORIGIN is the NOSE GEAR STOP POSITION - the mark painted on the apron that a docking
	// guidance system stops the aircraft at. An aircraft parked here shares this pose
	// exactly, which is why composing a stand with an aircraft needs no offset.
	//
	// WHAT IS HERE is ground-fixed only: plant dug into the concrete and boxes painted on
	// it. Where a service connects to the AIRCRAFT is on UAircraftType, because an A320 and
	// a 737-800 both park here and their doors are metres apart.
	//
	// +X faces the terminal, +Y is starboard of a parked aircraft. Units are uu, and a uu
	// here is a centimetre.
	auto AddFixture = [Definition](const TCHAR* Id, double X, double Y, double HeadingDegrees,
		EServiceRole Role)
	{
		FEntityAnchor Fixture;
		Fixture.Id = FName(Id);
		Fixture.LocalPosition = FVector2D(X, Y);
		Fixture.LocalHeading = FMath::DegreesToRadians(HeadingDegrees);
		Fixture.Role = Role;
		Definition->Anchors.Add(Fixture);
	};

	// Fuel hydrant pit, under where the starboard wing falls. A stand with one takes a
	// dispenser; a stand without needs a bowser, which is a different journey entirely.
	AddFixture(TEXT("HydrantPit"), -1200.0, 700.0, -90.0, EServiceRole::Fuel);

	// Fixed ground power at the bridge, off the port bow.
	AddFixture(TEXT("FixedGPU"), 300.0, -600.0, 90.0, EServiceRole::GPU);

	// Painted equipment boxes. These are on the concrete, so they belong to the stand even
	// though what parks in them serves the aircraft.
	AddFixture(TEXT("EquipmentFwd"), -300.0, 1100.0, -90.0, EServiceRole::Baggage);
	AddFixture(TEXT("EquipmentAft"), -2100.0, 1100.0, -90.0, EServiceRole::Baggage);

	// Where the tug waits before pushback, clear of the nose.
	AddFixture(TEXT("TugStand"), 1400.0, -600.0, 180.0, EServiceRole::Tug);

	// What this stand can provide at all. A contact stand does the lot.
	Definition->AvailableServices = {
		EServiceRole::Aircraft, EServiceRole::Fuel, EServiceRole::Baggage,
		EServiceRole::Tug, EServiceRole::GPU, EServiceRole::Passenger, EServiceRole::Crew };

	// EVERY FIELD THIS BUILDER OWNS, SET, including the three that happen to want the
	// constructor default. Not decoration: build_stand_asset.py re-authors an EXISTING asset
	// in place (it must - deleting one that a level and another asset reference fails), so a
	// field the builder leaves alone keeps whatever was last saved into it. Stating them is
	// what makes "re-run the script" mean the same thing as "make it from scratch".
	Definition->PoseRole = EServiceRole::Aircraft;   // the nose gear stop mark
	Definition->FootprintExtent = FVector2D::ZeroVector;   // the stand's extent IS its aircraft's
	Definition->Trucks = 0;                          // nothing is based here; a depot has the fleet

	// THE SERVICE LOOP: the closed lane the ground vehicles use, derived from what it has to
	// enclose rather than typed. See UEntityDefinition::ServiceLoop for why it is computed
	// here and not authored beside the anchors.
	//
	// The clearance is a CONSTANT and not a UPROPERTY, deliberately: it is a fact about how
	// this stand type is laid out, decided at authoring time beside the anchors. A
	// level-authored version would be a knob that silently reshaped stands already placed.
	constexpr double LoopClearance = 300.0;

	// THE UNION of the design aircraft's footprint AND every anchor. A footprint-only box
	// leaves TugStand at +1400 outside it, because the tug waits nine metres ahead of a nose
	// that stops at +507 - and an anchor outside the lane is an anchor whose spur has to
	// cross it to get in.
	double MinX = TNumericLimits<double>::Max(), MaxX = -TNumericLimits<double>::Max();
	double MinY = TNumericLimits<double>::Max(), MaxY = -TNumericLimits<double>::Max();
	auto Cover = [&MinX, &MaxX, &MinY, &MaxY](const FVector2D& Point)
	{
		MinX = FMath::Min(MinX, Point.X); MaxX = FMath::Max(MaxX, Point.X);
		MinY = FMath::Min(MinY, Point.Y); MaxY = FMath::Max(MaxY, Point.Y);
	};

	if (Aircraft != nullptr && Aircraft->Footprint.IsSet())
	{
		const FEntityFootprint& Footprint = Aircraft->Footprint;
		const double HalfSpan = Footprint.Wingspan * 0.5;
		Cover(FVector2D(Footprint.NoseX,  HalfSpan));
		Cover(FVector2D(Footprint.NoseX, -HalfSpan));
		Cover(FVector2D(Footprint.TailX,  HalfSpan));
		Cover(FVector2D(Footprint.TailX, -HalfSpan));
	}
	for (const FEntityAnchor& Anchor : Definition->Anchors)
	{
		Cover(Anchor.LocalPosition);
	}

	Definition->ServiceLoop.Reset();
	if (MinX <= MaxX)
	{
		MinX -= LoopClearance; MaxX += LoopClearance;
		MinY -= LoopClearance; MaxY += LoopClearance;

		// FOUR-SIDED AND CLOSED. An open U round the nose was rejected: a closed loop gives
		// entry from any side, including from behind the tail, and costs nothing because the
		// lane is invisible. The first point is never repeated - the close is implicit.
		Definition->ServiceLoop = {
			FVector2D(MinX, MinY), FVector2D(MaxX, MinY),
			FVector2D(MaxX, MaxY), FVector2D(MinX, MaxY) };
	}
}

UEntityDefinition* UEntityDefinition::MakeFuelDepotTransient()
{
	UEntityDefinition* Definition = NewObject<UEntityDefinition>(GetTransientPackage());
	BuildFuelDepot(Definition);
	return Definition;
}

void UEntityDefinition::BuildFuelDepot(UEntityDefinition* Definition)
{
	if (Definition == nullptr)
	{
		return;
	}

	// NO ANCHORS. A depot has no plant an aircraft connects to - see the header for why the
	// pose alone is its road connection. Reset rather than left alone so re-authoring an
	// asset that once had some really does clear them.
	Definition->Anchors.Reset();

	// AND NO SERVICE LOOP. A lane encloses an aeroplane and the boxes round it; a depot has
	// one pose and nothing parked at it, and its pose IS its road connection. Reset for the
	// same reason Anchors is.
	Definition->ServiceLoop.Reset();

	// ORIGIN IS THE TRUCK BAY - where a truck stands when it is home, and the node it is
	// dispatched from and back to.
	//
	// +X FACES AWAY FROM THE ROAD, exactly as a stand's +X faces the terminal: the pose
	// lead-in leaves along heading PLUS 180 (see FAnchorLink), so it runs out of the BACK of
	// the installation to the movement area. A depot is therefore aimed away from the road it
	// serves, and the truck drives out behind it.
	//
	// Stated the wrong way round when this was first written ("+X faces the road"), which is
	// self-contradictory given the +180 in the same sentence - and it is the sentence a
	// player placing one would have followed.
	Definition->PoseRole = EServiceRole::Fuel;

	// HALF-extents: 12 m by 8 m overall. A tank, a pump, and room to turn a bowser round.
	// A placeholder box, and the only geometry the depot has this slice.
	Definition->FootprintExtent = FVector2D(600.0, 400.0);

	// ONE truck. The number UFuelService counts trucks-out against; M3's job board replaces
	// the counting, not the number.
	Definition->Trucks = 1;

	// What this installation can provide. Fuel and nothing else, which is the whole slice.
	Definition->AvailableServices = { EServiceRole::Fuel };

	// NO DesignAircraft, deliberately: nothing parks here, so there is no envelope to draw
	// and no ICAO code letter to size a lead-in sweep by. FAnchorLink falls back to Code C's
	// 2500 uu radius, which is generous for a van and costs nothing.
}

bool UEntityDefinition::HasUsableAnchorIds(const UEntityDefinition* Definition)
{
	if (Definition == nullptr)
	{
		return false;
	}

	TSet<FName> Seen;
	for (const FEntityAnchor& Anchor : Definition->Anchors)
	{
		if (Anchor.Id.IsNone() || Seen.Contains(Anchor.Id))
		{
			return false;
		}
		Seen.Add(Anchor.Id);
	}
	return true;
}

int32 UEntityDefinition::RefreshResolvedAnchors(URoadNetwork& Network)
{
	int32 ChangedCount = 0;

	// Gathered by index, like FAnchorLink::Build: nothing here adds or removes an entity,
	// so holding this reference across RefreshResolvedAnchor calls is safe, and the id
	// still needs building by hand from Index and Generation - the array elements have no
	// stable handle of their own to hand back.
	const TArray<FEntityInstance>& Entities = Network.GetEntities();
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		const FEntityInstance& Instance = Entities[Index];
		if (!Instance.bAlive || Instance.Definition == nullptr)
		{
			continue;
		}

		FEntityInstanceId EntityId;
		EntityId.Index = Index;
		EntityId.Generation = Instance.Generation;

		// THE INSTANCE'S OWN POSE ROLE, for the same reason the anchors' snapshots are
		// refreshed here: an entity placed and saved before FEntityInstance::PoseRole existed
		// loads with the UPROPERTY default (Aircraft) and nothing else ever corrects it.
		// Harmless for every entity that COULD have been saved then - they were all stands -
		// and wrong for a depot whose definition is re-authored after placement, which would
		// otherwise be stuck casting its lead-in at a taxiway for ever.
		if (Instance.PoseRole != Instance.Definition->PoseRole
			&& Network.SetEntityPoseRole(EntityId, Instance.Definition->PoseRole))
		{
			++ChangedCount;
		}

		for (const FResolvedAnchor& Resolved : Instance.ResolvedAnchors)
		{
			// The role lives on the definition, addressed by id - never by position in the
			// array, which is the invariant FResolvedAnchor exists to remove.
			const FEntityAnchor* Declared = Instance.Definition->Anchors.FindByPredicate(
				[&Resolved](const FEntityAnchor& Candidate) { return Candidate.Id == Resolved.Id; });
			if (Declared == nullptr)
			{
				continue;
			}

			if (Declared->LocalHeading == Resolved.LocalHeading && Declared->Role == Resolved.Role)
			{
				continue;
			}

			if (Network.RefreshResolvedAnchor(EntityId, Resolved.Id, Declared->LocalHeading, Declared->Role))
			{
				++ChangedCount;
			}
		}
	}

	return ChangedCount;
}
