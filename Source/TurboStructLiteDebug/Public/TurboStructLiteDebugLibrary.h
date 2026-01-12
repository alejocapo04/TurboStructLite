/* Copyright (C) 2025 Alejo ALDREY - All Rights Reserved
 * This content is downloadable from Epic Games Fab.
 */

#pragma once

#include "CoreMinimal.h"
#include "TurboStructLiteTypesDebug.h"

DECLARE_LOG_CATEGORY_EXTERN(LogTurboStructLiteDebug, Log, All);

// Category: Debug.
TURBOSTRUCTLITEDEBUG_API void TurboStructLiteDebugLog(const FString& Text, ETurboStructLiteLogType Type);
// Category: Debug.
TURBOSTRUCTLITEDEBUG_API void TurboStructLiteDebugTraceScope(const TCHAR* Name);
