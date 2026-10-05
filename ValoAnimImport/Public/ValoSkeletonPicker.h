#pragma once
#include "CoreMinimal.h"

class USkeleton;

/** Shared modal skeleton picker (prefers TP_*, never defaults to FP_*). */
namespace ValoSkeletonPicker
{
	/** Returns nullptr if cancelled. Safe to call from the game thread. */
	USkeleton* Pick(const FString& FileName);

	/** Auto-pick preferred skeleton (TP_*) without UI — for headless. */
	USkeleton* PickDefault();
}
