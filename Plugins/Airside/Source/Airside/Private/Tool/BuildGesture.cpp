#include "Tool/BuildGesture.h"

void FBuildGesture::Press(const FVector2D& Screen)
{
	bDown = true;
	bDragging = false;
	PressScreen = Screen;
}

EGestureStep FBuildGesture::Move(const FVector2D& Screen, double Threshold)
{
	if (!bDown)
	{
		return EGestureStep::None;
	}

	if (bDragging)
	{
		return EGestureStep::Dragging;
	}

	if (FVector2D::Distance(Screen, PressScreen) < Threshold)
	{
		return EGestureStep::None;
	}

	bDragging = true;
	return EGestureStep::DragBegan;
}

EGestureEnd FBuildGesture::Release()
{
	if (!bDown)
	{
		return EGestureEnd::Nothing;
	}

	const bool bWasDragging = bDragging;
	bDown = false;
	bDragging = false;
	return bWasDragging ? EGestureEnd::DragEnd : EGestureEnd::Click;
}

void FBuildGesture::Cancel()
{
	bDown = false;
	bDragging = false;
}
