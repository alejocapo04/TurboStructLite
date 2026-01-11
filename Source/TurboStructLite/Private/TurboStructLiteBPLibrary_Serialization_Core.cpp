#include "TurboStructLiteBPLibrary.h"
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
#if WITH_EDITOR
#include "Trace/Trace.inl"
#endif
#include "Async/ParallelFor.h"
#include "HAL/ThreadSafeBool.h"
#include "Runtime/Launch/Resources/Version.h"


void UTurboStructLiteBPLibrary::WriteFieldMeta(FArchive& Ar, const FTurboStructLiteFieldMeta& Meta)
{
	Ar << const_cast<FString&>(Meta.Name);
	Ar << const_cast<FString&>(Meta.Type);
	int32 SizeCopy = Meta.Size;
	Ar << SizeCopy;
	int32 ChildCount = Meta.Children.Num();
	Ar << ChildCount;
	for (const FTurboStructLiteFieldMeta& Child : Meta.Children)
	{
		WriteFieldMeta(Ar, Child);
	}
}bool UTurboStructLiteBPLibrary::ReadFieldMeta(FArchive& Ar, FTurboStructLiteFieldMeta& OutMeta)
{
	Ar << OutMeta.Name;
	Ar << OutMeta.Type;
	Ar << OutMeta.Size;
	int32 ChildCount = 0;
	Ar << ChildCount;
	if (ChildCount < 0)
	{
		return false;
	}
	OutMeta.Children.SetNum(ChildCount);
	for (int32 Index = 0; Index < ChildCount; ++Index)
	{
		if (!ReadFieldMeta(Ar, OutMeta.Children[Index]))
		{
			return false;
		}
	}
	return true;
}bool UTurboStructLiteBPLibrary::BuildDebugString(const TArray<FTurboStructLiteFieldMeta>& Fields, FString& Out)
{
	if (Fields.Num() == 1)
	{
		const FTurboStructLiteFieldMeta& Root = Fields[0];
		TArray<FString> ChildNames;
		for (const FTurboStructLiteFieldMeta& Child : Root.Children)
		{
			ChildNames.Add(Child.Name);
		}
		FString FieldList = FString::Join(ChildNames, TEXT(","));
		Out = FString::Printf(TEXT("Name=%s;IsArray=0;Type=%s"), *Root.Name, *Root.Type);
		if (!FieldList.IsEmpty())
		{
			Out += FString::Printf(TEXT(";Fields=%s"), *FieldList);
		}
		return true;
	}

	TArray<FString> Parts;
	for (const FTurboStructLiteFieldMeta& Field : Fields)
	{
		Parts.Add(FString::Printf(TEXT("%s(%s,%d)"), *Field.Name, *Field.Type, Field.Size));
	}
	Out = FString::Join(Parts, TEXT(", "));
	return true;
}bool UTurboStructLiteBPLibrary::CompressBuffer(ETurboStructLiteCompression Method, const TArray<uint8>& In, TArray<uint8>& Out, int32 MaxParallelThreads, int32 ChunkBatchSizeMB)
{
#if WITH_EDITOR
	TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("TurboStructLite_CompressBuffer"));
#endif
	Out.Reset();
	if (In.Num() == 0)
	{
		return false;
	}
	if (Method == ETurboStructLiteCompression::None)
	{
		Out = In;
		return true;
	}
	const FName Name = GetCompressionName(Method);
	if (Name.IsNone())
	{
		return false;
	}

	const int32 ChunkSizeMB = ChunkBatchSizeMB > 0 ? ChunkBatchSizeMB : 4;
	const int32 ChunkSize = FMath::Max(1, ChunkSizeMB) * 1024 * 1024;
	const int32 TotalSize = In.Num();
	const int32 NumChunks = FMath::Max(1, FMath::DivideAndRoundUp(TotalSize, ChunkSize));
	const int32 MaxThreads = FMath::Clamp(MaxParallelThreads > 0 ? MaxParallelThreads : GetParallelThreadLimit(), 1, FPlatformMisc::NumberOfCoresIncludingHyperthreads());
	const int32 TaskCount = FMath::Min(MaxThreads, NumChunks);
	const int32 ChunksPerTask = FMath::DivideAndRoundUp(NumChunks, TaskCount);

	TArray<TArray<uint8>> CompressedChunks;
	CompressedChunks.SetNum(NumChunks);
	FThreadSafeBool bFailed(false);

	ParallelFor(TaskCount, [&](int32 TaskIndex)
	{
		const int32 StartChunk = TaskIndex * ChunksPerTask;
		const int32 EndChunk = FMath::Min(StartChunk + ChunksPerTask, NumChunks);
		for (int32 ChunkIndex = StartChunk; ChunkIndex < EndChunk; ++ChunkIndex)
		{
			const int32 Offset = ChunkIndex * ChunkSize;
			const int32 ThisSize = FMath::Min(ChunkSize, TotalSize - Offset);
			const int32 BoundSize = FCompression::CompressMemoryBound(Name, ThisSize);
			TArray<uint8> LocalCompressed;
			LocalCompressed.SetNum(BoundSize);
			int32 LocalSize = BoundSize;
			if (!FCompression::CompressMemory(Name, LocalCompressed.GetData(), LocalSize, In.GetData() + Offset, ThisSize))
			{
				bFailed = true;
				return;
			}
			LocalCompressed.SetNum(LocalSize);
			CompressedChunks[ChunkIndex] = MoveTemp(LocalCompressed);
		}
	}, EParallelForFlags::Unbalanced);

	if (bFailed)
	{
		return false;
	}

	int64 TotalCompressed = 0;
	for (const TArray<uint8>& Chunk : CompressedChunks)
	{
		TotalCompressed += Chunk.Num();
	}

	const int32 HeaderInts = 3 + NumChunks;
	const int64 HeaderBytes = static_cast<int64>(HeaderInts) * sizeof(int32);
	Out.Reserve(HeaderBytes + TotalCompressed);

	const int32 UncompressedSize = In.Num();
	Out.Append(reinterpret_cast<const uint8*>(&UncompressedSize), sizeof(int32));
	const int32 StoredChunkSize = ChunkSize;
	Out.Append(reinterpret_cast<const uint8*>(&StoredChunkSize), sizeof(int32));
	const int32 StoredNumChunks = NumChunks;
	Out.Append(reinterpret_cast<const uint8*>(&StoredNumChunks), sizeof(int32));

	for (const TArray<uint8>& Chunk : CompressedChunks)
	{
		const int32 Size = Chunk.Num();
		Out.Append(reinterpret_cast<const uint8*>(&Size), sizeof(int32));
	}

	for (const TArray<uint8>& Chunk : CompressedChunks)
	{
		Out.Append(Chunk);
	}

	return true;
}bool UTurboStructLiteBPLibrary::DecompressBuffer(ETurboStructLiteCompression Method, const TArray<uint8>& In, TArray<uint8>& Out)
{
#if WITH_EDITOR
	TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("TurboStructLite_DecompressBuffer"));
#endif
	Out.Reset();
	if (Method == ETurboStructLiteCompression::None)
	{
		Out = In;
		return In.Num() > 0;
	}
	if (In.Num() < static_cast<int32>(sizeof(int32) * 3))
	{
		return false;
	}

	int32 UncompressedSize = 0;
	int32 ChunkSize = 0;
	int32 NumChunks = 0;
	const uint8* Ptr = In.GetData();
	FMemory::Memcpy(&UncompressedSize, Ptr, sizeof(int32));
	Ptr += sizeof(int32);
	if (UncompressedSize <= 0)
	{
		return false;
	}
	FMemory::Memcpy(&ChunkSize, Ptr, sizeof(int32));
	Ptr += sizeof(int32);
	FMemory::Memcpy(&NumChunks, Ptr, sizeof(int32));
	Ptr += sizeof(int32);

	const FName Name = GetCompressionName(Method);
	if (Name.IsNone())
	{
		return false;
	}

	const auto LegacyDecompress = [&]() -> bool
	{
		const uint8* CompressedData = In.GetData() + sizeof(int32);
		const int32 CompressedSize = In.Num() - static_cast<int32>(sizeof(int32));
		Out.SetNum(UncompressedSize);
		if (!FCompression::UncompressMemory(Name, Out.GetData(), UncompressedSize, CompressedData, CompressedSize))
		{
			Out.Reset();
			return false;
		}
		return true;
	};

	const bool bLooksValid = ChunkSize > 0 && NumChunks > 0 && NumChunks < 1'000'000;
	const int64 TableBytes = static_cast<int64>(NumChunks) * sizeof(int32);
	const int64 HeaderBytes = sizeof(int32) * 3 + TableBytes;
	const bool bTableFits = HeaderBytes <= In.Num();

	if (!bLooksValid || !bTableFits)
	{
		return LegacyDecompress();
	}

	TArray<int32> ChunkSizes;
	ChunkSizes.SetNum(NumChunks);
	FMemory::Memcpy(ChunkSizes.GetData(), Ptr, TableBytes);
	int64 PayloadOffset = HeaderBytes;
	int64 PayloadSize = 0;
	for (int32 Size : ChunkSizes)
	{
		if (Size <= 0)
		{
			return LegacyDecompress();
		}
		PayloadSize += Size;
	}
	if (PayloadOffset + PayloadSize > In.Num())
	{
		return LegacyDecompress();
	}

	Out.SetNum(UncompressedSize);
	const int32 MaxThreads = FMath::Clamp(GetParallelThreadLimit(), 1, FPlatformMisc::NumberOfCoresIncludingHyperthreads());
	const int32 TaskCount = FMath::Min(MaxThreads, NumChunks);
	const int32 ChunksPerTask = FMath::DivideAndRoundUp(NumChunks, TaskCount);
	FThreadSafeBool bFailed(false);

	ParallelFor(TaskCount, [&](int32 TaskIndex)
	{
		const int32 StartChunk = TaskIndex * ChunksPerTask;
		const int32 EndChunk = FMath::Min(StartChunk + ChunksPerTask, NumChunks);
		int64 LocalOffset = PayloadOffset;
		for (int32 Index = 0; Index < StartChunk; ++Index)
		{
			LocalOffset += ChunkSizes[Index];
		}
		for (int32 ChunkIndex = StartChunk; ChunkIndex < EndChunk; ++ChunkIndex)
		{
			const int32 CompressedSize = ChunkSizes[ChunkIndex];
			const uint8* CompressedData = In.GetData() + LocalOffset;
			const int32 DestOffset = ChunkIndex * ChunkSize;
			const int32 DestSize = (ChunkIndex == NumChunks - 1) ? (UncompressedSize - DestOffset) : ChunkSize;
			if (!FCompression::UncompressMemory(Name, Out.GetData() + DestOffset, DestSize, CompressedData, CompressedSize))
			{
				bFailed = true;
				return;
			}
			LocalOffset += CompressedSize;
		}
	}, EParallelForFlags::Unbalanced);

	if (bFailed)
	{
		Out.Reset();
		return false;
	}

	return true;
}bool UTurboStructLiteBPLibrary::SerializeWildcard(FProperty* Property, void* Address, TArray<uint8>& OutBytes, bool bSaveOnlyMarked)
{
#if WITH_EDITOR
	TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("TurboStructLite_SerializeWildcard"));
#endif
	FString DebugMeta;
	return SerializePropertyWithMeta(Property, Address, OutBytes, DebugMeta, bSaveOnlyMarked);
}bool UTurboStructLiteBPLibrary::DeserializeWildcard(FProperty* Property, void* Address, const TArray<uint8>& InBytes, int32 OverrideMaxThreads, bool bSaveOnlyMarked)
{
	return DeserializePropertyWithMeta(Property, Address, InBytes, OverrideMaxThreads, bSaveOnlyMarked);
}bool UTurboStructLiteBPLibrary::SerializePropertyWithMeta(FProperty* Property, void* Address, TArray<uint8>& OutBytes, FString& OutDebugMeta, bool bSaveOnlyMarked)
{
#if WITH_EDITOR
	TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("TurboStructLite_SerializePropertyWithMeta"));
#endif
	if (!Property || !Address)
	{
		return false;
	}

	TArray<FTurboStructLiteFieldMeta> Fields;
	TArray<uint8> DataBuffer;

	FTurboStructLiteFieldMeta RootMeta;
	if (SerializePropertyRecursive(Property, Address, DataBuffer, RootMeta, bSaveOnlyMarked))
	{
		Fields.Add(MoveTemp(RootMeta));
	}
#if WITH_EDITOR
	TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("TurboStructLite_SerializeProperty_PostRecursive"));
#endif

	TArray<uint8> MetaBytes;
	FMemoryWriter MetaWriter(MetaBytes, true);
	int32 Count = Fields.Num();
	MetaWriter << Count;
	for (const FTurboStructLiteFieldMeta& FieldMeta : Fields)
	{
		WriteFieldMeta(MetaWriter, FieldMeta);
	}

	OutDebugMeta.Reset();
	BuildDebugString(Fields, OutDebugMeta);

	OutBytes.Reset();
	FMemoryWriter Writer(OutBytes, true);
	int32 FormatVersion = 1;
	Writer << FormatVersion;
	int32 MetaSize = MetaBytes.Num();
	Writer << MetaSize;
	if (MetaSize > 0)
	{
		Writer.Serialize(MetaBytes.GetData(), MetaSize);
	}
	if (DataBuffer.Num() > 0)
	{
		Writer.Serialize(DataBuffer.GetData(), DataBuffer.Num());
	}
	return true;
}bool UTurboStructLiteBPLibrary::BuildDebugMetadataFromBytes(const TArray<uint8>& InBytes, FString& OutDebugMeta)
{
	OutDebugMeta.Reset();
	if (InBytes.Num() < static_cast<int32>(sizeof(int32) * 2))
	{
		return false;
	}
	FMemoryReader Reader(InBytes, true);
	int32 FormatVersion = 0;
	Reader << FormatVersion;
	if (FormatVersion != 1)
	{
		return false;
	}
	int32 MetaSize = 0;
	Reader << MetaSize;
	if (MetaSize < 0 || Reader.TotalSize() < Reader.Tell() + MetaSize)
	{
		return false;
	}
	TArray<uint8> MetaBytes;
	MetaBytes.SetNum(MetaSize);
	if (MetaSize > 0)
	{
		Reader.Serialize(MetaBytes.GetData(), MetaSize);
	}
	FMemoryReader MetaReader(MetaBytes, true);
	int32 Count = 0;
	MetaReader << Count;
	if (Count < 0)
	{
		return false;
	}
	TArray<FTurboStructLiteFieldMeta> Fields;
	Fields.SetNum(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		if (!ReadFieldMeta(MetaReader, Fields[Index]))
		{
			return false;
		}
	}
	return BuildDebugString(Fields, OutDebugMeta);
}bool UTurboStructLiteBPLibrary::GetRootMetaTypeFromBytes(const TArray<uint8>& InBytes, FString& OutType)
{
	OutType.Reset();
	if (InBytes.Num() < static_cast<int32>(sizeof(int32) * 2))
	{
		return false;
	}
	FMemoryReader Reader(InBytes, true);
	int32 FormatVersion = 0;
	Reader << FormatVersion;
	if (FormatVersion != 1)
	{
		return false;
	}
	int32 MetaSize = 0;
	Reader << MetaSize;
	if (MetaSize <= 0 || Reader.TotalSize() < Reader.Tell() + MetaSize)
	{
		return false;
	}
	TArray<uint8> MetaBytes;
	MetaBytes.SetNum(MetaSize);
	Reader.Serialize(MetaBytes.GetData(), MetaSize);
	FMemoryReader MetaReader(MetaBytes, true);
	int32 Count = 0;
	MetaReader << Count;
	if (Count <= 0)
	{
		return false;
	}
	FTurboStructLiteFieldMeta RootMeta;
	if (!ReadFieldMeta(MetaReader, RootMeta))
	{
		return false;
	}
	OutType = RootMeta.Type;
	return !OutType.IsEmpty();
}bool UTurboStructLiteBPLibrary::DeserializePropertyWithMeta(FProperty* Property, void* Address, const TArray<uint8>& InBytes, int32 OverrideMaxThreads, bool bSaveOnlyMarked)
{
#if WITH_EDITOR
	TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("TurboStructLite_DeserializePropertyWithMeta"));
#endif
	if (!Property || !Address || InBytes.Num() == 0)
	{
		return false;
	}
	if (InBytes.Num() < static_cast<int32>(sizeof(int32) * 2))
	{
		return false;
	}

	FMemoryReader Reader(InBytes, true);
	int32 FormatVersion = 0;
	Reader << FormatVersion;
	if (FormatVersion != 1)
	{
		Reader.Seek(0);
		FObjectAndNameAsStringProxyArchive Ar(Reader, true);
		Ar.ArIsSaveGame = bSaveOnlyMarked;
		Ar.ArNoDelta = true;
		FStructuredArchiveFromArchive Structured(Ar);
		FStructuredArchive::FSlot Slot = Structured.GetSlot();
		Property->SerializeItem(Slot, Address, nullptr);
		return true;
	}

	int32 MetaSize = 0;
	Reader << MetaSize;
	if (MetaSize < 0 || Reader.TotalSize() < Reader.Tell() + MetaSize)
	{
		return false;
	}

	TArray<uint8> MetaBytes;
	MetaBytes.SetNum(MetaSize);
	if (MetaSize > 0)
	{
		Reader.Serialize(MetaBytes.GetData(), MetaSize);
	}

	const int32 DataOffset = Reader.Tell();
	const int32 DataLen = InBytes.Num() - DataOffset;
	const uint8* DataPtr = InBytes.GetData() + DataOffset;

	FMemoryReader MetaReader(MetaBytes, true);
	int32 Count = 0;
	MetaReader << Count;
	if (Count < 0)
	{
		return false;
	}
	TArray<FTurboStructLiteFieldMeta> Fields;
	Fields.SetNum(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		if (!ReadFieldMeta(MetaReader, Fields[Index]))
		{
			return false;
		}
	}

	int32 Offset = 0;
	const int32 MaxThreads = FMath::Clamp((OverrideMaxThreads > 0) ? OverrideMaxThreads : GetParallelThreadLimit(), 1, FPlatformMisc::NumberOfCoresIncludingHyperthreads());
	if (FStructProperty* StructProp = CastField<FStructProperty>(Property))
	{
		uint8* StructPtr = static_cast<uint8*>(Address);
		const FString RootPath = StructProp->Struct ? StructProp->Struct->GetName() : FString();
		if (Fields.Num() == 1 && Fields[0].Children.Num() > 0)
		{
			return ApplyMetaToStruct(Fields[0].Children, StructProp->Struct, StructPtr, DataPtr, DataLen, Offset, MaxThreads, bSaveOnlyMarked, RootPath, Reader);
		}
		return ApplyMetaToStruct(Fields, StructProp->Struct, StructPtr, DataPtr, DataLen, Offset, MaxThreads, bSaveOnlyMarked, RootPath, Reader);
	}

	if (Fields.Num() == 0)
	{
		return true;
	}

	const FTurboStructLiteFieldMeta& Meta = Fields[0];
	if (Meta.Size < 0 || Meta.Size > DataLen)
	{
		return false;
	}
	const FString MetaType = NormalizeTypeName(Meta.Type);
	const FString PropType = NormalizeTypeName(Property->GetCPPType(nullptr, 0));
	if (MetaType == PropType)
	{
		return DeserializePropertyFromSlice(Property, Address, DataPtr, Meta.Size, bSaveOnlyMarked, Reader);
	}
	bool bReaderError = false;
	TryMigratePropertyValue(Meta, Property, Address, DataPtr, Meta.Size, bSaveOnlyMarked, Reader, bReaderError);
	if (bReaderError)
	{
		return false;
	}
	return true;
}bool UTurboStructLiteBPLibrary::TurboStructLiteSerializeProperty(FProperty* Property, void* Address, TArray<uint8>& OutBytes)
{
	FString DebugMeta;
	return SerializePropertyWithMeta(Property, Address, OutBytes, DebugMeta);
}bool UTurboStructLiteBPLibrary::TurboStructLiteDeserializeProperty(FProperty* Property, void* Address, const TArray<uint8>& InBytes)
{
	return DeserializePropertyWithMeta(Property, Address, InBytes);
}int32 UTurboStructLiteBPLibrary::GetMagic()
{
	return GetDefault<UTurboStructLiteBPLibrary>()->TurboStructLiteMagic;
}int32 UTurboStructLiteBPLibrary::GetVersion()
{
	return GetDefault<UTurboStructLiteBPLibrary>()->TurboStructLiteVersion;
}FString UTurboStructLiteBPLibrary::BuildStructFieldList(const UStruct* Struct)
{
	TArray<FString> Names;
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		Names.Add(It->GetName());
	}
	return FString::Join(Names, TEXT(","));
}FString UTurboStructLiteBPLibrary::BuildDebugMetadata(FProperty* Property)
{
	if (!Property)
	{
		return FString();
	}

	bool bIsArray = false;
	const FString PropertyName = Property->GetName();
	FString TypeName;
	FString FieldList;

	FProperty* TargetProp = Property;
	if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
	{
		bIsArray = true;
		TargetProp = ArrayProp->Inner;
	}

	TypeName = TargetProp ? TargetProp->GetCPPType() : TEXT("Unknown");

	if (const FStructProperty* StructProp = CastField<FStructProperty>(TargetProp))
	{
		FieldList = BuildStructFieldList(StructProp->Struct);
	}

	FString Result = FString::Printf(TEXT("Name=%s;IsArray=%s;Type=%s"),
		*PropertyName,
		bIsArray ? TEXT("1") : TEXT("0"),
		*TypeName);
	if (!FieldList.IsEmpty())
	{
		Result += FString::Printf(TEXT(";Fields=%s"), *FieldList);
	}
	return Result;
}

