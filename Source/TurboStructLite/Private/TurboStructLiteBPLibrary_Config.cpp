#include "TurboStructLiteBPLibrary.h"
#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Parse.h"
#include "HAL/CriticalSection.h"
#include "UObject/Stack.h"
#include "UObject/UnrealType.h"
#include "HAL/FileManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "TurboStructLiteDebugMacros.h"

ETurboStructLiteEncryption UTurboStructLiteBPLibrary::GetActiveEncryptionMode()
{
	EnsureSettingsLoaded();
	FScopeLock Lock(&EncryptionSettingsMutex);
	return ActiveEncryptionMode;
}

FString UTurboStructLiteBPLibrary::GetActiveEncryptionKey()
{
	if (GlobalKeyProvider.IsBound())
	{
		if (IsInGameThread())
		{
			const FString ProvidedKey = GlobalKeyProvider.Execute();
			FScopeLock Lock(&EncryptionSettingsMutex);
			CachedProviderKey = ProvidedKey;
			bHasCachedProviderKey = true;
			return ProvidedKey;
		}
		FScopeLock Lock(&EncryptionSettingsMutex);
		if (bHasCachedProviderKey)
		{
			return CachedProviderKey;
		}
		return FString();
	}
	EnsureSettingsLoaded();
	FScopeLock Lock(&EncryptionSettingsMutex);
	return ActiveEncryptionKey;
}

void UTurboStructLiteBPLibrary::EnsureSettingsLoaded()
{
	if (bTurboStructLiteSettingsLoaded)
	{
		return;
	}
	FScopeLock Lock(&EncryptionSettingsMutex);
	if (bTurboStructLiteSettingsLoaded)
	{
		return;
	}
	if (GConfig)
	{
		FString EncryptionValue;
		if (GConfig->GetString(TurboStructLiteSettingsSection, TEXT("DefaultEncryption"), EncryptionValue, GGameIni))
		{
			EncryptionValue.TrimStartAndEndInline();
			int64 EnumValue = INDEX_NONE;
			if (const UEnum* Enum = StaticEnum<ETurboStructLiteEncryptionSettings>())
			{
				EnumValue = Enum->GetValueByNameString(EncryptionValue);
				if (EnumValue == INDEX_NONE)
				{
					const int32 ScopeIndex = EncryptionValue.Find(TEXT("::"));
					if (ScopeIndex != INDEX_NONE)
					{
						EnumValue = Enum->GetValueByNameString(EncryptionValue.Mid(ScopeIndex + 2));
					}
				}
			}
			if (EnumValue == INDEX_NONE && EncryptionValue.IsNumeric())
			{
				EnumValue = FCString::Atoi64(*EncryptionValue);
			}
			ConfigEncryptionMode = EnumValue == static_cast<int64>(ETurboStructLiteEncryptionSettings::AES) ? ETurboStructLiteEncryption::AES : ETurboStructLiteEncryption::None;
		}
		FString SavedKey;
		if (GConfig->GetString(TurboStructLiteSettingsSection, TEXT("DefaultEncryptionKey"), SavedKey, GGameIni))
		{
			ConfigEncryptionKey = SavedKey;
		}
		FString CompressionValue;
		if (GConfig->GetString(TurboStructLiteSettingsSection, TEXT("DefaultCompression"), CompressionValue, GGameIni))
		{
			CompressionValue.TrimStartAndEndInline();
			int64 EnumValue = INDEX_NONE;
			if (const UEnum* Enum = StaticEnum<ETurboStructLiteCompressionSettings>())
			{
				EnumValue = Enum->GetValueByNameString(CompressionValue);
				if (EnumValue == INDEX_NONE)
				{
					const int32 ScopeIndex = CompressionValue.Find(TEXT("::"));
					if (ScopeIndex != INDEX_NONE)
					{
						EnumValue = Enum->GetValueByNameString(CompressionValue.Mid(ScopeIndex + 2));
					}
				}
			}
			if (EnumValue == INDEX_NONE && CompressionValue.IsNumeric())
			{
				EnumValue = FCString::Atoi64(*CompressionValue);
			}
			if (EnumValue >= 0 && EnumValue <= static_cast<int64>(ETurboStructLiteCompressionSettings::Oodle))
			{
				ConfigDefaultCompression = static_cast<ETurboStructLiteCompression>(EnumValue + 1);
			}
		}
		FString BatchingValue;
		if (GConfig->GetString(TurboStructLiteSettingsSection, TEXT("DefaultCompressionBatching"), BatchingValue, GGameIni))
		{
			BatchingValue.TrimStartAndEndInline();
			int64 EnumValue = INDEX_NONE;
			if (const UEnum* Enum = StaticEnum<ETurboStructLiteBatching>())
			{
				EnumValue = Enum->GetValueByNameString(BatchingValue);
				if (EnumValue == INDEX_NONE)
				{
					const int32 ScopeIndex = BatchingValue.Find(TEXT("::"));
					if (ScopeIndex != INDEX_NONE)
					{
						EnumValue = Enum->GetValueByNameString(BatchingValue.Mid(ScopeIndex + 2));
					}
				}
			}
			if (EnumValue == INDEX_NONE && BatchingValue.IsNumeric())
			{
				EnumValue = FCString::Atoi64(*BatchingValue);
			}
			if (EnumValue != INDEX_NONE)
			{
				const int32 RawMB = (EnumValue == 0) ? 4 : static_cast<int32>(EnumValue);
				ConfigDefaultBatchingMB = FMath::Clamp(RawMB, 2, 32);
			}
		}
	}
	ActiveEncryptionMode = ConfigEncryptionMode;
	if (ActiveEncryptionKey.IsEmpty())
	{
		ActiveEncryptionKey = ConfigEncryptionKey;
	}
	ActiveDefaultCompression = ConfigDefaultCompression;
	ActiveDefaultBatchingMB = ConfigDefaultBatchingMB;
	bTurboStructLiteSettingsLoaded = true;
}

bool UTurboStructLiteBPLibrary::LoadLegacyRedirects(TMap<FString, FString>& OutRedirects)
{
	OutRedirects.Reset();
	if (!GConfig)
	{
		return false;
	}
	const FConfigSection* Section = GConfig->GetSection(TurboStructLiteSettingsSection, false, GGameIni);
	if (!Section)
	{
		return false;
	}
	for (const TPair<FName, FConfigValue>& Pair : *Section)
	{
		if (Pair.Key != TEXT("LegacyRedirects"))
		{
			continue;
		}
		const FString Entry = Pair.Value.GetValue();
		FString Key;
		FString Value;
		if (FParse::Value(*Entry, TEXT("Key="), Key) && FParse::Value(*Entry, TEXT("Value="), Value))
		{
			Key.TrimQuotesInline();
			Value.TrimQuotesInline();
			if (!Key.IsEmpty())
			{
				OutRedirects.Add(Key, Value);
			}
		}
	}
	return OutRedirects.Num() > 0;
}

ETurboStructLiteCompression UTurboStructLiteBPLibrary::ResolveCompression(ETurboStructLiteCompression Method)
{
	if (Method == ETurboStructLiteCompression::ProjectDefault)
	{
		return GetDefaultCompression();
	}
	return Method;
}

ETurboStructLiteCompression UTurboStructLiteBPLibrary::GetDefaultCompression()
{
	EnsureSettingsLoaded();
	FScopeLock Lock(&EncryptionSettingsMutex);
	return ActiveDefaultCompression == ETurboStructLiteCompression::ProjectDefault ? ETurboStructLiteCompression::None : ActiveDefaultCompression;
}

int32 UTurboStructLiteBPLibrary::ResolveBatchingMB(ETurboStructLiteBatchingSetting Batching)
{
	EnsureSettingsLoaded();
	const int32 Value = static_cast<int32>(Batching);
	if (Batching == ETurboStructLiteBatchingSetting::ProjectDefault || Value <= 0)
	{
		return ActiveDefaultBatchingMB;
	}

return FMath::Clamp(Value, 2, 32);
}

EAsyncExecution UTurboStructLiteBPLibrary::ResolveAsyncExecution(ETurboStructLiteAsyncExecution Execution)
{
	switch (Execution)
	{
	case ETurboStructLiteAsyncExecution::TaskGraph:
		return EAsyncExecution::TaskGraph;
	case ETurboStructLiteAsyncExecution::TaskGraphMainThread:
		return EAsyncExecution::TaskGraphMainThread;
	case ETurboStructLiteAsyncExecution::Thread:
		return EAsyncExecution::Thread;
	case ETurboStructLiteAsyncExecution::ThreadIfForkSafe:
		return EAsyncExecution::ThreadIfForkSafe;
	case ETurboStructLiteAsyncExecution::LargeThreadPool:
#if WITH_EDITOR
		return EAsyncExecution::LargeThreadPool;
#else
		return EAsyncExecution::ThreadPool;
#endif
	case ETurboStructLiteAsyncExecution::ThreadPool:
	default:
		return EAsyncExecution::ThreadPool;
	}
}



