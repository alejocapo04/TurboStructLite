/* Copyright (C) 2025 Alejo ALDREY - All Rights Reserved
 * This content is downloadable from Epic Games Fab.
 */

#pragma once

#include "CoreMinimal.h"

enum class ETurboStructLiteLogType : uint8
{
	Normal,
	Warning,
	Error
};

inline const TCHAR* TurboStructLiteDebugSettingsSection = TEXT("/Script/TurboStructLiteProjectSettings.TurboStructLiteProjectSettings");
inline const TCHAR* TurboStructLiteDebugPrintSettingKey = TEXT("bShowDebugPrintString");
