#include "TurboStructLiteBPLibrary.h"
#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/ConfigCacheIni.h"
#include "HAL/CriticalSection.h"
#include "UObject/Stack.h"
#include "UObject/UnrealType.h"
#include "HAL/FileManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "TurboStructLiteDebugMacros.h"
FTurboStructLiteKeyProviderDelegate UTurboStructLiteBPLibrary::GlobalKeyProvider;

void UTurboStructLiteBPLibrary::RegisterEncryptionKeyProvider(FTurboStructLiteKeyProviderDelegate NewProvider)
{
	GlobalKeyProvider = NewProvider;
	FScopeLock Lock(&EncryptionSettingsMutex);
	CachedProviderKey.Reset();
	bHasCachedProviderKey = false;
}


