#include "TurboStructLiteBPLibrary.h"
#include "TurboStructLiteConstants.h"
#include "TurboStructLite.h"
#include "Misc/Compression.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/Guid.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "Serialization/StructuredArchive.h"
#include "UObject/UObjectGlobals.h"
#if __has_include("Serialization/StructuredArchiveAdapters.h")
#include "Serialization/StructuredArchiveAdapters.h"
#endif
#include "TurboStructLiteDebugMacros.h"
#include "Async/ParallelFor.h"
#include "HAL/ThreadSafeBool.h"
#include "Runtime/Launch/Resources/Version.h"


thread_local int32 GTurboStructLiteParallelThreadLimit = 2;

FScopedParallelLimitLite::FScopedParallelLimitLite(int32 NewLimit)
{
	PrevLimit = UTurboStructLiteBPLibrary::SetParallelThreadLimit(NewLimit);
}

FScopedParallelLimitLite::~FScopedParallelLimitLite()
{
	UTurboStructLiteBPLibrary::SetParallelThreadLimit(PrevLimit);
}

int32 UTurboStructLiteBPLibrary::SetParallelThreadLimit(int32 MaxThreads)
{
	const int32 Prev = GTurboStructLiteParallelThreadLimit;
	GTurboStructLiteParallelThreadLimit = MaxThreads;
	return Prev;
}

int32 UTurboStructLiteBPLibrary::GetParallelThreadLimit()
{
	return GTurboStructLiteParallelThreadLimit;
}

bool UTurboStructLiteBPLibrary::IsPropertySafeForParallel(const FProperty* Property)
{
	if (!Property)
	{
		return false;
	}
	if (IsUnsupportedProperty(Property))
	{
		return false;
	}
	if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
	{
		return IsPropertySafeForParallel(ArrayProp->Inner);
	}
	if (const FSetProperty* SetProp = CastField<FSetProperty>(Property))
	{
		return IsPropertySafeForParallel(SetProp->ElementProp);
	}
	if (const FMapProperty* MapProp = CastField<FMapProperty>(Property))
	{
		return IsPropertySafeForParallel(MapProp->KeyProp) && IsPropertySafeForParallel(MapProp->ValueProp);
	}
	if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
	{
		for (TFieldIterator<FProperty> It(StructProp->Struct); It; ++It)
		{
			if (!IsPropertySafeForParallel(*It))
			{
				return false;
			}
		}
	}
	return true;
}

bool UTurboStructLiteBPLibrary::SerializeArrayParallel(FArrayProperty* ArrayProp, void* Address, TArray<uint8>& OutData, FTurboStructLiteFieldMeta& OutMeta, bool bSaveOnlyMarked)
{
	FScriptArrayHelper Helper(ArrayProp, Address);
	const int32 Num = Helper.Num();

	OutMeta.Name = ArrayProp->GetName();
	OutMeta.Type = ArrayProp->GetCPPType(nullptr, 0);

	const bool bIsComplexType = ArrayProp->Inner->IsA<FStructProperty>() || ArrayProp->Inner->IsA<FArrayProperty>() || ArrayProp->Inner->IsA<FMapProperty>() || ArrayProp->Inner->IsA<FSetProperty>();
	const int32 ParallelThreshold = bIsComplexType ? TurboStructLiteParallelThresholdComplex : TurboStructLiteParallelThresholdSimple;
	const bool bSafeInner = IsPropertySafeForParallel(ArrayProp->Inner);
	const int32 MaxAllowedThreads = FMath::Clamp(GetParallelThreadLimit(), 1, FPlatformMisc::NumberOfCoresIncludingHyperthreads());
	if (Num < ParallelThreshold || !bSafeInner || MaxAllowedThreads <= 1)
	{
		return false;
	}

	const int32 StartOffset = OutData.Num();
	const int32 HeaderIndex = OutData.Num();
	OutData.AddUninitialized(sizeof(int32));
	int32 NumElements = Num;
	checkSlow(OutData.GetData());
	checkSlow(HeaderIndex >= 0);
	checkSlow(HeaderIndex + static_cast<int32>(sizeof(int32)) <= OutData.Num());
	FMemory::Memcpy(OutData.GetData() + HeaderIndex, &NumElements, sizeof(int32));

	const int32 ElementSize = ArrayProp->Inner->GetSize();
	checkSlow(ElementSize > 0);
	int32 BatchSize = TurboStructLiteParallelBatchSizeDefault;
	if (bIsComplexType)
	{
		const int32 TargetChunks = MaxAllowedThreads;
		BatchSize = FMath::Max(1, FMath::DivideAndRoundUp(Num, TargetChunks));
	}
	else
	{
		const int32 MinBatchSize = TurboStructLiteParallelMinBatchSize;
		BatchSize = FMath::Max(MinBatchSize, TurboStructLiteParallelTargetBytesPerChunk / (ElementSize + 1));
		const int32 MaxChunks = MaxAllowedThreads * TurboStructLiteParallelMaxChunksPerThread;
		if (BatchSize > 0 && Num / BatchSize > MaxChunks)
		{
			BatchSize = FMath::Max(Num / MaxChunks, 1);
		}
	}
	const int32 NumChunks = FMath::Max(1, FMath::DivideAndRoundUp(Num, BatchSize));

	TArray<TArray<uint8>> ChunkBuffers;
	ChunkBuffers.SetNum(NumChunks);

	ParallelFor(NumChunks, [&](int32 ChunkIndex)
	{
		checkSlow(ChunkBuffers.IsValidIndex(ChunkIndex));
		const int32 StartIndex = ChunkIndex * BatchSize;
		const int32 EndIndex = FMath::Min(StartIndex + BatchSize, Num);
		TArray<uint8>& LocalBuffer = ChunkBuffers[ChunkIndex];
		LocalBuffer.Reserve((EndIndex - StartIndex) * ElementSize);
		FMemoryWriter LocalWriter(LocalBuffer, true);
		FObjectAndNameAsStringProxyArchive LocalAr(LocalWriter, true);
		LocalAr.ArIsSaveGame = bSaveOnlyMarked;
		LocalAr.ArNoDelta = true;
		for (int32 i = StartIndex; i < EndIndex; ++i)
		{
			FStructuredArchiveFromArchive LocalStructured(LocalAr);
			uint8* ElemPtr = Helper.GetRawPtr(i);
			checkSlow(ElemPtr);
			FStructuredArchive::FSlot Slot = LocalStructured.GetSlot();
			ArrayProp->Inner->SerializeItem(Slot, ElemPtr, nullptr);
		}
	}, EParallelForFlags::Unbalanced);

	TURBOSTRUCTLITE_TRACE_SCOPE(TEXT("TurboStructLite_SerializeArrayParallel_PostParallelFor"));
	int32 TotalAddedSize = sizeof(int32);
	for (const TArray<uint8>& Chunk : ChunkBuffers)
	{
		TotalAddedSize += Chunk.Num();
	}
	OutData.Reserve(StartOffset + TotalAddedSize);

	for (const TArray<uint8>& Chunk : ChunkBuffers)
	{
		OutData.Append(Chunk);
	}
	ChunkBuffers.Empty();

	OutMeta.Size = OutData.Num() - StartOffset;
	return true;
}




