#include "TurboStructLiteDebugLibrary.h"
#include "TurboStructLiteDebugMacros.h"

#include "Async/Async.h"
#include "Engine/Engine.h"
#include "Misc/ConfigCacheIni.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

DEFINE_LOG_CATEGORY(LogTurboStructLiteDebug);

void TurboStructLiteDebugLog(const FString& Text, ETurboStructLiteLogType Type)
{
	bool bShowDebug = false;
	if (GConfig)
	{
		GConfig->GetBool(TurboStructLiteDebugSettingsSection, TurboStructLiteDebugPrintSettingKey, bShowDebug, GGameIni);
	}

	if (Type == ETurboStructLiteLogType::Warning)
	{
		UE_LOG(LogTurboStructLiteDebug, Warning, TEXT("%s"), *Text);
	}
	if (Type == ETurboStructLiteLogType::Error)
	{
		UE_LOG(LogTurboStructLiteDebug, Error, TEXT("%s"), *Text);
	}
	if (Type != ETurboStructLiteLogType::Warning && Type != ETurboStructLiteLogType::Error)
	{
		UE_LOG(LogTurboStructLiteDebug, Log, TEXT("%s"), *Text);
	}

	if (!bShowDebug)
	{
		return;
	}

	const FColor Color = Type == ETurboStructLiteLogType::Error ? FColor::Red : (Type == ETurboStructLiteLogType::Warning ? FColor::Yellow : FColor::Green);
	if (IsInGameThread())
	{
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 2.0f, Color, Text);
		}
		return;
	}

	AsyncTask(ENamedThreads::GameThread, [Text, Color]()
	{
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 2.0f, Color, Text);
		}
	});
}

void TurboStructLiteDebugTraceScope(const TCHAR* Name)
{
	TURBOSTRUCTLITE_TRACE_SCOPE_TEXT(Name);
}
