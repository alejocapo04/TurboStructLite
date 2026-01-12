#include "TurboStructLiteBPLibrary.h"
#include "TurboStructLite.h"

#include "Async/Async.h"
#include "Engine/Engine.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/AES.h"
#include "Misc/SecureHash.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Guid.h"
#if !TURBOSTRUCTLITE_USE_OPENSSL
#include "IPlatformCrypto.h"
#include "PlatformCryptoTypes.h"
#endif
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
#include "Hash/Blake3.h"
#endif
#include "Misc/ScopeLock.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#if WITH_EDITOR
#include "Trace/Trace.inl"
#endif
#if TURBOSTRUCTLITE_USE_OPENSSL
#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/err.h>
THIRD_PARTY_INCLUDES_END
#undef UI
#endif

void UTurboStructLiteBPLibrary::CalculateTurboHash(const uint8* Data, int32 Size, uint8 OutHash[32])
{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	FBlake3Hash H = FBlake3::HashBuffer(Data, Size);
	FMemory::Memcpy(OutHash, H.GetBytes(), 32);
#else
	FSHA1 Hasher;
	Hasher.Update(Data, Size);
	Hasher.Final();
	uint8 Digest[FSHA1::DigestSize];
	Hasher.GetHash(Digest);
	for (int32 i = 0; i < 32; ++i)
	{
		OutHash[i] = Digest[i % FSHA1::DigestSize];
	}
#endif
}

FCriticalSection UTurboStructLiteBPLibrary::SlotIndexMutex;
TMap<FString, FTurboStructLiteSlotIndex> UTurboStructLiteBPLibrary::CachedSlotIndexes;
FCriticalSection UTurboStructLiteBPLibrary::MemoryWarningMutex;
TSet<uint32> UTurboStructLiteBPLibrary::WarnedMemorySlots;
bool UTurboStructLiteBPLibrary::bMemoryWarningsCvarRegistered = false;
IConsoleVariable* UTurboStructLiteBPLibrary::MemoryWarningsOnScreenInShippingCvar = nullptr;

void UTurboStructLiteBPLibrary::EnsureMemoryWarningCvar()
{
#if UE_BUILD_SHIPPING
	{
		FScopeLock Lock(&MemoryWarningMutex);
		if (bMemoryWarningsCvarRegistered)
		{
			return;
		}
		bMemoryWarningsCvarRegistered = true;
	}
	MemoryWarningsOnScreenInShippingCvar = IConsoleManager::Get().RegisterConsoleVariable(TEXT("ts.MemoryWarningsOnScreenInShipping"), 1, TEXT("Enable TurboStructLite on-screen memory warnings in shipping builds."), ECVF_Default);
#endif
}

bool UTurboStructLiteBPLibrary::ShouldShowOnScreenWarnings()
{
#if UE_BUILD_SHIPPING
	EnsureMemoryWarningCvar();
	if (!MemoryWarningsOnScreenInShippingCvar)
	{
		return false;
	}
	return MemoryWarningsOnScreenInShippingCvar->GetInt() != 0;
#else
	return true;
#endif
}

uint32 UTurboStructLiteBPLibrary::BuildMemoryWarningKey(const FString& SlotName, int32 SubSlotIndex, bool bIsSave, bool bIsLogic)
{
	const uint32 SlotHash = GetTypeHash(SlotName);
	const uint32 SubHash = GetTypeHash(SubSlotIndex);
	const uint32 ModeHash = GetTypeHash(bIsSave);
	const uint32 LogicHash = GetTypeHash(bIsLogic);
	return HashCombine(HashCombine(SlotHash, SubHash), HashCombine(ModeHash, LogicHash));
}

void UTurboStructLiteBPLibrary::BeginMemoryOpMessage(const FString& SlotName, int32 SubSlotIndex, bool bIsSave, bool bIsLogic)
{
	if (SubSlotIndex < 0)
	{
		return;
	}
	BuildMemoryWarningKey(SlotName, SubSlotIndex, bIsSave, bIsLogic);
}

void UTurboStructLiteBPLibrary::UpdateMemoryPressureWarning(const FString& SlotName, int32 SubSlotIndex, int64 RawSizeBytes, bool bIsSave, bool bIsLogic)
{
	if (SubSlotIndex < 0)
	{
		return;
	}
	const int64 MemoryWarningThreshold = 64ll * 1024ll * 1024ll;
	if (RawSizeBytes < MemoryWarningThreshold)
	{
		return;
	}
	const uint32 WarningKey = BuildMemoryWarningKey(SlotName, SubSlotIndex, bIsSave, bIsLogic);
	const int64 EstimatedPeak = RawSizeBytes + (RawSizeBytes / 4);
	const double RawMB = static_cast<double>(RawSizeBytes) / (1024.0 * 1024.0);
	const double PeakMB = static_cast<double>(EstimatedPeak) / (1024.0 * 1024.0);
	const bool bIsMainThread = IsInGameThread();
	const FString Advice = bIsMainThread
		? (bIsSave ? TEXT("CRITICAL: Saving on GameThread! Use Async Save to prevent hitches.") : TEXT("CRITICAL: Loading on GameThread! Use Async Load to prevent hitches."))
		: TEXT("Info: Running Async (Good). Allocation risk remains.");
	FString BufferText;
	if (bIsSave)
	{
		const FString SavePath = BuildSavePath(SlotName);
		const int64 FileSize = IFileManager::Get().FileSize(*SavePath);
		const int32 BufferSize = CalcStreamingBufferSize(FileSize);
		const double BufferKB = static_cast<double>(BufferSize) / 1024.0;
		BufferText = FString::Printf(TEXT(" IO buffer: %.0f KB. "), BufferKB);
	}
	const TCHAR* ModeText = bIsSave ? TEXT("save") : TEXT("load");
	const TCHAR* LogicText = bIsLogic ? TEXT(" logic") : TEXT("");
	const FString Message = FString::Printf(TEXT("Warning: Large %s%s payload. Slot '%s' [%d] raw: %.2f MB. Estimated RAM peak >= %.2f MB.%s%s"), ModeText, LogicText, *SlotName, SubSlotIndex, RawMB, PeakMB, *BufferText, *Advice);
	bool bShouldLog = false;
	{
		FScopeLock Lock(&MemoryWarningMutex);
		if (!WarnedMemorySlots.Contains(WarningKey))
		{
			WarnedMemorySlots.Add(WarningKey);
			bShouldLog = true;
		}
	}
	if (bShouldLog)
	{
		UE_LOG(LogTurboStructLite, Warning, TEXT("%s"), *Message);
	}
	if (!ShouldShowOnScreenWarnings() || !GEngine)
	{
		return;
	}
	const int32 ScreenKey = static_cast<int32>(WarningKey);
	if (bIsMainThread)
	{
		GEngine->AddOnScreenDebugMessage(ScreenKey, 3600.0f, FColor::Yellow, Message);
	}
	else
	{
		AsyncTask(ENamedThreads::GameThread, [ScreenKey, Message]()
		{
			if (GEngine)
			{
				GEngine->AddOnScreenDebugMessage(ScreenKey, 3600.0f, FColor::Yellow, Message);
			}
		});
	}
}

void UTurboStructLiteBPLibrary::EndMemoryOpMessage(const FString& SlotName, int32 SubSlotIndex, bool bIsSave, bool bIsLogic)
{
	if (SubSlotIndex < 0)
	{
		return;
	}
	if (!ShouldShowOnScreenWarnings())
	{
		return;
	}
	const uint32 WarningKey = BuildMemoryWarningKey(SlotName, SubSlotIndex, bIsSave, bIsLogic);
	const int32 ScreenKey = static_cast<int32>(WarningKey);
	if (IsInGameThread())
	{
		if (GEngine)
		{
			GEngine->RemoveOnScreenDebugMessage(ScreenKey);
		}
	}
	else
	{
		AsyncTask(ENamedThreads::GameThread, [ScreenKey]()
		{
			if (GEngine)
			{
				GEngine->RemoveOnScreenDebugMessage(ScreenKey);
			}
		});
	}
}

bool UTurboStructLiteBPLibrary::GetExpectedRawSize(const FString& SlotName, int32 SubSlotIndex, int64& OutSizeBytes)
{
	OutSizeBytes = 0;
	if (SubSlotIndex < 0)
	{
		return false;
	}
	FTurboStructLiteCachedEntry Cached;
	if (!GetCachedEntry(SlotName, SubSlotIndex, Cached))
	{
		return false;
	}
	OutSizeBytes = static_cast<int64>(Cached.UncompressedSize);
	return OutSizeBytes > 0;
}

bool UTurboStructLiteBPLibrary::DeriveAESKeyFromString(const FString& Key, const TArray<uint8>& Salt, uint8 OutKey[64])
{
	FMemory::Memzero(OutKey, 64);
	if (Salt.Num() <= 0)
	{
		return false;
	}
	const FTCHARToUTF8 KeyUtf8(*Key);
	const int32 Iterations = 100000;
#if TURBOSTRUCTLITE_USE_OPENSSL
	if (PKCS5_PBKDF2_HMAC(KeyUtf8.Get(), KeyUtf8.Length(), Salt.GetData(), Salt.Num(), Iterations, EVP_sha256(), 64, OutKey) != 1)
	{
		FMemory::Memzero(OutKey, 64);
		return false;
	}
	return true;
#else
	const int32 KeyLen = KeyUtf8.Length();
	if (KeyLen <= 0)
	{
		return false;
	}
	TUniquePtr<FEncryptionContext> Context = IPlatformCrypto::Get().CreateContext();
	if (!Context)
	{
		return false;
	}
	auto HmacSha256 = [&Context](const uint8* HmacKey, int32 HmacKeyLen, const uint8* Data, int32 DataLen, uint8 OutHash[32]) -> bool
	{
		uint8 KeyBlock[64];
		if (HmacKeyLen > 64)
		{
			uint8 KeyHash[32];
			FSHA256Hasher KeyHasher = Context->CreateSHA256Hasher();
			if (KeyHasher.Init() != EPlatformCryptoResult::Success)
			{
				return false;
			}
			if (KeyHasher.Update(TArrayView<const uint8>(HmacKey, HmacKeyLen)) != EPlatformCryptoResult::Success)
			{
				return false;
			}
			if (KeyHasher.Finalize(TArrayView<uint8>(KeyHash, 32)) != EPlatformCryptoResult::Success)
			{
				return false;
			}
			FMemory::Memcpy(KeyBlock, KeyHash, 32);
			FMemory::Memzero(KeyBlock + 32, 32);
			FMemory::Memzero(KeyHash, sizeof(KeyHash));
		}
		else
		{
			if (HmacKeyLen > 0)
			{
				FMemory::Memcpy(KeyBlock, HmacKey, HmacKeyLen);
			}
			if (HmacKeyLen < 64)
			{
				FMemory::Memzero(KeyBlock + HmacKeyLen, 64 - HmacKeyLen);
			}
		}

		uint8 Ipad[64];
		uint8 Opad[64];
		for (int32 Index = 0; Index < 64; ++Index)
		{
			Ipad[Index] = KeyBlock[Index] ^ 0x36;
			Opad[Index] = KeyBlock[Index] ^ 0x5c;
		}

		uint8 InnerHash[32];
		FSHA256Hasher InnerHasher = Context->CreateSHA256Hasher();
		if (InnerHasher.Init() != EPlatformCryptoResult::Success)
		{
			return false;
		}
		if (InnerHasher.Update(TArrayView<const uint8>(Ipad, 64)) != EPlatformCryptoResult::Success)
		{
			return false;
		}
		if (DataLen > 0)
		{
			if (InnerHasher.Update(TArrayView<const uint8>(Data, DataLen)) != EPlatformCryptoResult::Success)
			{
				return false;
			}
		}
		if (InnerHasher.Finalize(TArrayView<uint8>(InnerHash, 32)) != EPlatformCryptoResult::Success)
		{
			return false;
		}

		FSHA256Hasher OuterHasher = Context->CreateSHA256Hasher();
		if (OuterHasher.Init() != EPlatformCryptoResult::Success)
		{
			return false;
		}
		if (OuterHasher.Update(TArrayView<const uint8>(Opad, 64)) != EPlatformCryptoResult::Success)
		{
			return false;
		}
		if (OuterHasher.Update(TArrayView<const uint8>(InnerHash, 32)) != EPlatformCryptoResult::Success)
		{
			return false;
		}
		if (OuterHasher.Finalize(TArrayView<uint8>(OutHash, 32)) != EPlatformCryptoResult::Success)
		{
			return false;
		}

		FMemory::Memzero(KeyBlock, sizeof(KeyBlock));
		FMemory::Memzero(Ipad, sizeof(Ipad));
		FMemory::Memzero(Opad, sizeof(Opad));
		FMemory::Memzero(InnerHash, sizeof(InnerHash));
		return true;
	};

	const int32 HashLen = 32;
	const int32 DerivedLen = 64;
	const int32 BlockCount = (DerivedLen + HashLen - 1) / HashLen;
	const uint8* KeyBytes = reinterpret_cast<const uint8*>(KeyUtf8.Get());
	for (int32 BlockIndex = 1; BlockIndex <= BlockCount; ++BlockIndex)
	{
		TArray<uint8> SaltBlock;
		SaltBlock.Reserve(Salt.Num() + 4);
		SaltBlock.Append(Salt);
		SaltBlock.Add(static_cast<uint8>((BlockIndex >> 24) & 0xFF));
		SaltBlock.Add(static_cast<uint8>((BlockIndex >> 16) & 0xFF));
		SaltBlock.Add(static_cast<uint8>((BlockIndex >> 8) & 0xFF));
		SaltBlock.Add(static_cast<uint8>(BlockIndex & 0xFF));

		uint8 U[32];
		uint8 T[32];
		if (!HmacSha256(KeyBytes, KeyLen, SaltBlock.GetData(), SaltBlock.Num(), U))
		{
			FMemory::Memzero(U, sizeof(U));
			FMemory::Memzero(T, sizeof(T));
			FMemory::Memzero(OutKey, 64);
			return false;
		}
		FMemory::Memcpy(T, U, 32);
		for (int32 Iter = 1; Iter < Iterations; ++Iter)
		{
			if (!HmacSha256(KeyBytes, KeyLen, U, 32, U))
			{
				FMemory::Memzero(U, sizeof(U));
				FMemory::Memzero(T, sizeof(T));
				FMemory::Memzero(OutKey, 64);
				return false;
			}
			for (int32 Index = 0; Index < 32; ++Index)
			{
				T[Index] ^= U[Index];
			}
		}
		const int32 Offset = (BlockIndex - 1) * HashLen;
		const int32 CopyLen = FMath::Min(HashLen, DerivedLen - Offset);
		FMemory::Memcpy(OutKey + Offset, T, CopyLen);
		FMemory::Memzero(U, sizeof(U));
		FMemory::Memzero(T, sizeof(T));
	}
	return true;
#endif
}

void UTurboStructLiteBPLibrary::EncryptAesBlock(const uint8* Key, const uint8 InBlock[16], uint8 OutBlock[16])
{
	uint8 Block[16];
	FMemory::Memcpy(Block, InBlock, 16);
	FAES::EncryptData(Block, FAES::AESBlockSize, Key, 32);
	FMemory::Memcpy(OutBlock, Block, 16);
	FMemory::Memzero(Block, 16);
}

void UTurboStructLiteBPLibrary::GcmIncrement32(uint8 Counter[16])
{
	for (int32 Index = 15; Index >= 12; --Index)
	{
		uint8 Value = Counter[Index];
		Counter[Index] = static_cast<uint8>(Value + 1);
		if (Counter[Index] != 0)
		{
			break;
		}
	}
}

void UTurboStructLiteBPLibrary::GcmRightShift(uint8 Block[16])
{
	uint8 Carry = 0;
	for (int32 Index = 0; Index < 16; ++Index)
	{
		const uint8 NewCarry = static_cast<uint8>(Block[Index] & 0x01);
		Block[Index] = static_cast<uint8>((Block[Index] >> 1) | (Carry ? 0x80 : 0x00));
		Carry = NewCarry;
	}
}

void UTurboStructLiteBPLibrary::GcmMultiply(const uint8 X[16], const uint8 Y[16], uint8 Out[16])
{
	uint8 Z[16];
	uint8 V[16];
	FMemory::Memzero(Z, 16);
	FMemory::Memcpy(V, Y, 16);

	for (int32 ByteIndex = 0; ByteIndex < 16; ++ByteIndex)
	{
		const uint8 XByte = X[ByteIndex];
		for (int32 Bit = 7; Bit >= 0; --Bit)
		{
			if (XByte & (1 << Bit))
			{
				for (int32 Index = 0; Index < 16; ++Index)
				{
					Z[Index] ^= V[Index];
				}
			}
			const bool bLsb = (V[15] & 0x01) != 0;
			GcmRightShift(V);
			if (bLsb)
			{
				V[0] ^= 0xe1;
			}
		}
	}

	FMemory::Memcpy(Out, Z, 16);
	FMemory::Memzero(Z, 16);
	FMemory::Memzero(V, 16);
}

void UTurboStructLiteBPLibrary::GcmUpdate(uint8 Xi[16], const uint8* Data, int32 DataLen, const uint8 H[16])
{
	int32 Offset = 0;
	while (Offset < DataLen)
	{
		uint8 Block[16];
		FMemory::Memzero(Block, 16);
		const int32 BlockSize = FMath::Min(16, DataLen - Offset);
		if (BlockSize > 0)
		{
			FMemory::Memcpy(Block, Data + Offset, BlockSize);
		}
		for (int32 Index = 0; Index < 16; ++Index)
		{
			Xi[Index] ^= Block[Index];
		}
		uint8 Product[16];
		GcmMultiply(Xi, H, Product);
		FMemory::Memcpy(Xi, Product, 16);
		FMemory::Memzero(Block, 16);
		FMemory::Memzero(Product, 16);
		Offset += BlockSize;
	}
}

void UTurboStructLiteBPLibrary::GcmFinalize(uint8 Xi[16], uint64 AadBits, uint64 CipherBits, const uint8 H[16])
{
	uint8 LenBlock[16];
	FMemory::Memzero(LenBlock, 16);
	for (int32 Index = 0; Index < 8; ++Index)
	{
		LenBlock[Index] = static_cast<uint8>((AadBits >> (56 - (Index * 8))) & 0xFF);
	}
	for (int32 Index = 0; Index < 8; ++Index)
	{
		LenBlock[8 + Index] = static_cast<uint8>((CipherBits >> (56 - (Index * 8))) & 0xFF);
	}
	for (int32 Index = 0; Index < 16; ++Index)
	{
		Xi[Index] ^= LenBlock[Index];
	}
	uint8 Product[16];
	GcmMultiply(Xi, H, Product);
	FMemory::Memcpy(Xi, Product, 16);
	FMemory::Memzero(LenBlock, 16);
	FMemory::Memzero(Product, 16);
}

void UTurboStructLiteBPLibrary::GcmBuildJ0(const uint8* IV, int32 IVLen, const uint8 H[16], uint8 OutJ0[16])
{
	FMemory::Memzero(OutJ0, 16);
	if (IV && IVLen == 12)
	{
		FMemory::Memcpy(OutJ0, IV, 12);
		OutJ0[15] = 1;
		return;
	}

	uint8 Xi[16];
	FMemory::Memzero(Xi, 16);
	if (IV && IVLen > 0)
	{
		GcmUpdate(Xi, IV, IVLen, H);
	}
	GcmFinalize(Xi, 0, static_cast<uint64>(IVLen) * 8, H);
	FMemory::Memcpy(OutJ0, Xi, 16);
	FMemory::Memzero(Xi, 16);
}

void UTurboStructLiteBPLibrary::GcmCtrCrypt(const uint8* Key, const uint8 J0[16], const uint8* In, int32 InLen, TArray<uint8>& Out)
{
	Out.SetNum(InLen);
	if (InLen <= 0)
	{
		return;
	}

	uint8 Counter[16];
	FMemory::Memcpy(Counter, J0, 16);
	GcmIncrement32(Counter);

	int32 Offset = 0;
	while (Offset < InLen)
	{
		uint8 Stream[16];
		EncryptAesBlock(Key, Counter, Stream);
		const int32 BlockSize = FMath::Min(16, InLen - Offset);
		for (int32 Index = 0; Index < BlockSize; ++Index)
		{
			Out[Offset + Index] = In[Offset + Index] ^ Stream[Index];
		}
		GcmIncrement32(Counter);
		FMemory::Memzero(Stream, 16);
		Offset += BlockSize;
	}
	FMemory::Memzero(Counter, 16);
}

bool UTurboStructLiteBPLibrary::GcmConstantTimeEqual(const uint8* A, const uint8* B, int32 Len)
{
	uint8 Diff = 0;
	for (int32 Index = 0; Index < Len; ++Index)
	{
		Diff |= static_cast<uint8>(A[Index] ^ B[Index]);
	}
	return Diff == 0;
}

bool UTurboStructLiteBPLibrary::EncryptAesGcmFallback(const uint8* Key, const uint8* IV, int32 IVLen, const uint8* Plaintext, int32 PlaintextLen, TArray<uint8>& OutCiphertext, TArray<uint8>& OutTag)
{
	if (!Key || !IV || IVLen <= 0)
	{
		return false;
	}
	if (PlaintextLen < 0 || (PlaintextLen > 0 && !Plaintext))
	{
		return false;
	}

	uint8 Zero[16];
	FMemory::Memzero(Zero, 16);
	uint8 H[16];
	EncryptAesBlock(Key, Zero, H);

	uint8 J0[16];
	GcmBuildJ0(IV, IVLen, H, J0);

	GcmCtrCrypt(Key, J0, Plaintext, PlaintextLen, OutCiphertext);

	uint8 Xi[16];
	FMemory::Memzero(Xi, 16);
	if (OutCiphertext.Num() > 0)
	{
		GcmUpdate(Xi, OutCiphertext.GetData(), OutCiphertext.Num(), H);
	}
	GcmFinalize(Xi, 0, static_cast<uint64>(OutCiphertext.Num()) * 8, H);

	uint8 TagBlock[16];
	EncryptAesBlock(Key, J0, TagBlock);
	OutTag.SetNum(16);
	for (int32 Index = 0; Index < 16; ++Index)
	{
		OutTag[Index] = static_cast<uint8>(TagBlock[Index] ^ Xi[Index]);
	}

	FMemory::Memzero(Zero, 16);
	FMemory::Memzero(H, 16);
	FMemory::Memzero(J0, 16);
	FMemory::Memzero(Xi, 16);
	FMemory::Memzero(TagBlock, 16);
	return true;
}

bool UTurboStructLiteBPLibrary::DecryptAesGcmFallback(const uint8* Key, const uint8* IV, int32 IVLen, const uint8* Ciphertext, int32 CiphertextLen, const uint8* Tag, int32 TagLen, TArray<uint8>& OutPlaintext)
{
	if (!Key || !IV || !Tag || IVLen <= 0 || TagLen != 16)
	{
		return false;
	}
	if (CiphertextLen < 0 || (CiphertextLen > 0 && !Ciphertext))
	{
		return false;
	}

	uint8 Zero[16];
	FMemory::Memzero(Zero, 16);
	uint8 H[16];
	EncryptAesBlock(Key, Zero, H);

	uint8 J0[16];
	GcmBuildJ0(IV, IVLen, H, J0);

	uint8 Xi[16];
	FMemory::Memzero(Xi, 16);
	if (CiphertextLen > 0)
	{
		GcmUpdate(Xi, Ciphertext, CiphertextLen, H);
	}
	GcmFinalize(Xi, 0, static_cast<uint64>(CiphertextLen) * 8, H);

	uint8 TagBlock[16];
	uint8 Expected[16];
	EncryptAesBlock(Key, J0, TagBlock);
	for (int32 Index = 0; Index < 16; ++Index)
	{
		Expected[Index] = static_cast<uint8>(TagBlock[Index] ^ Xi[Index]);
	}

	const bool bMatch = GcmConstantTimeEqual(Tag, Expected, 16);
	if (!bMatch)
	{
		FMemory::Memzero(Zero, 16);
		FMemory::Memzero(H, 16);
		FMemory::Memzero(J0, 16);
		FMemory::Memzero(Xi, 16);
		FMemory::Memzero(TagBlock, 16);
		FMemory::Memzero(Expected, 16);
		return false;
	}

	GcmCtrCrypt(Key, J0, Ciphertext, CiphertextLen, OutPlaintext);

	FMemory::Memzero(Zero, 16);
	FMemory::Memzero(H, 16);
	FMemory::Memzero(J0, 16);
	FMemory::Memzero(Xi, 16);
	FMemory::Memzero(TagBlock, 16);
	FMemory::Memzero(Expected, 16);
	return true;
}

bool UTurboStructLiteBPLibrary::ReadHeaderAndEntryCount(FArchive& Reader, int32 ExpectedMagic, int32& OutVersion, int32& OutEntryCount)
{
	int32 Magic = 0;
	Reader << Magic;
	if (Magic != ExpectedMagic)
	{
		return false;
	}
	int32 Version = 0;
	Reader << Version;
	OutVersion = Version;
	if (Version != GetVersion() && Version != 1 && Version != 2 && Version != 3)
	{
		return false;
	}
	int64 Timestamp = 0;
	Reader << Timestamp;
	Reader << OutEntryCount;
	return OutEntryCount >= 0;
}

bool UTurboStructLiteBPLibrary::SkipData(FArchive& Reader, int32 DataSize)
{
	if (DataSize <= 0)
	{
		return DataSize == 0;
	}
	const int64 Target = Reader.Tell() + DataSize;
	if (Target > Reader.TotalSize())
	{
		return false;
	}
	Reader.Seek(Target);
	return true;
}

bool UTurboStructLiteBPLibrary::IsValidBufferSize(FArchive& Reader, int32 SizeToCheck)
{
	if (SizeToCheck < 0)
	{
		return false;
	}
	if (Reader.Tell() + SizeToCheck > Reader.TotalSize())
	{
		return false;
	}
	return true;
}

int32 UTurboStructLiteBPLibrary::CalcStreamingBufferSize(int64 FileSize)
{
	const int64 BaseSize = 128ll * 1024ll * 1024ll;
	if (FileSize <= 0)
	{
		return 64 * 1024;
	}
	const double Ratio = static_cast<double>(FileSize) / static_cast<double>(BaseSize);
	const int32 Exp = (Ratio <= 1.0) ? 6 : 7 + FMath::FloorToInt(FMath::Log2(Ratio));
	return (1 << Exp) * 1024;
}

void UTurboStructLiteBPLibrary::WriteEntry(FArchive& Writer, int32 SubSlot, const FTurboStructLiteEntry& Entry)
{
	uint8 CompressionByte = static_cast<uint8>(Entry.Compression);
	uint8 EncryptionByte = static_cast<uint8>(Entry.Encryption);
	int32 UncompressedSize = Entry.UncompressedSize;
	int32 DataSize = Entry.Data.Num();
	Writer << SubSlot;
	Writer << CompressionByte;
	Writer << EncryptionByte;
	Writer << UncompressedSize;
	Writer << DataSize;
	int32 MetaSize = 0;
	Writer << MetaSize;
	if (DataSize > 0)
	{
		Writer.Serialize(const_cast<uint8*>(Entry.Data.GetData()), DataSize);
	}
}

FString UTurboStructLiteBPLibrary::SanitizeSlotName(const FString& InSlotName)
{
	FString Result = InSlotName;
	Result.ReplaceInline(TEXT("\\"), TEXT("/"));
	Result.ReplaceInline(TEXT(":"), TEXT("_"));

	TArray<FString> Parts;
	Result.ParseIntoArray(Parts, TEXT("/"), true);

	FString CleanPath;
	for (const FString& Part : Parts)
	{
		FString Segment = Part;
		Segment.TrimStartAndEndInline();
		if (Segment.IsEmpty() || Segment == TEXT(".") || Segment == TEXT(".."))
		{
			continue;
		}
		Segment = FPaths::MakeValidFileName(Segment, TEXT('_'));
		Segment.TrimStartAndEndInline();
		while (Segment.EndsWith(TEXT(".")) || Segment.EndsWith(TEXT(" ")))
		{
			Segment = Segment.LeftChop(1);
		}
		if (Segment.IsEmpty())
		{
			continue;
		}
		const FString Upper = Segment.ToUpper();
		const bool bReserved =
			Upper == TEXT("CON") || Upper.StartsWith(TEXT("CON.")) ||
			Upper == TEXT("PRN") || Upper.StartsWith(TEXT("PRN.")) ||
			Upper == TEXT("AUX") || Upper.StartsWith(TEXT("AUX.")) ||
			Upper == TEXT("NUL") || Upper.StartsWith(TEXT("NUL.")) ||
			Upper == TEXT("COM1") || Upper.StartsWith(TEXT("COM1.")) ||
			Upper == TEXT("COM2") || Upper.StartsWith(TEXT("COM2.")) ||
			Upper == TEXT("COM3") || Upper.StartsWith(TEXT("COM3.")) ||
			Upper == TEXT("COM4") || Upper.StartsWith(TEXT("COM4.")) ||
			Upper == TEXT("COM5") || Upper.StartsWith(TEXT("COM5.")) ||
			Upper == TEXT("COM6") || Upper.StartsWith(TEXT("COM6.")) ||
			Upper == TEXT("COM7") || Upper.StartsWith(TEXT("COM7.")) ||
			Upper == TEXT("COM8") || Upper.StartsWith(TEXT("COM8.")) ||
			Upper == TEXT("COM9") || Upper.StartsWith(TEXT("COM9.")) ||
			Upper == TEXT("LPT1") || Upper.StartsWith(TEXT("LPT1.")) ||
			Upper == TEXT("LPT2") || Upper.StartsWith(TEXT("LPT2.")) ||
			Upper == TEXT("LPT3") || Upper.StartsWith(TEXT("LPT3.")) ||
			Upper == TEXT("LPT4") || Upper.StartsWith(TEXT("LPT4.")) ||
			Upper == TEXT("LPT5") || Upper.StartsWith(TEXT("LPT5.")) ||
			Upper == TEXT("LPT6") || Upper.StartsWith(TEXT("LPT6.")) ||
			Upper == TEXT("LPT7") || Upper.StartsWith(TEXT("LPT7.")) ||
			Upper == TEXT("LPT8") || Upper.StartsWith(TEXT("LPT8.")) ||
			Upper == TEXT("LPT9") || Upper.StartsWith(TEXT("LPT9."));
		if (bReserved)
		{
			Segment = TEXT("_") + Segment;
		}
		if (!CleanPath.IsEmpty())
		{
			CleanPath += TEXT("/");
		}
		CleanPath += Segment;
	}

	if (CleanPath.IsEmpty())
	{
		CleanPath = TEXT("Unnamed_Slot");
	}

	const FString BaseSaveDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SaveGames"), TEXT("TurboStructLite"));
	const int32 MaxPathLen = 260;
	const int32 SafetyMargin = 32;
	const int32 ExtLen = 5;
	int32 Available = MaxPathLen - BaseSaveDir.Len() - ExtLen - SafetyMargin;
	if (Available < 1)
	{
		Available = 1;
	}
	if (CleanPath.Len() > Available)
	{
		CleanPath = CleanPath.Left(Available);
		while (CleanPath.EndsWith(TEXT("/")))
		{
			CleanPath = CleanPath.LeftChop(1);
		}
		if (CleanPath.IsEmpty())
		{
			CleanPath = TEXT("Unnamed_Slot");
		}
	}

	return CleanPath;
}

FString UTurboStructLiteBPLibrary::BuildSavePath(const FString& SlotName)
{
	const FString SanitizedName = SanitizeSlotName(SlotName);
	if (!SlotName.Equals(SanitizedName, ESearchCase::CaseSensitive))
	{
		UE_LOG(LogTurboStructLite, Warning, TEXT("SlotName '%s' sanitized to '%s'"), *SlotName, *SanitizedName);
	}
	FString Name = SanitizedName;
	const FString BaseSaveDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SaveGames"), TEXT("TurboStructLite"));
	if (!Name.EndsWith(TEXT(".ssfs")))
	{
		Name += TEXT(".ssfs");
	}
	const FString FullPath = FPaths::Combine(BaseSaveDir, Name);
	const FString TargetDir = FPaths::GetPath(FullPath);
	IFileManager::Get().MakeDirectory(*TargetDir, true);
	return FullPath;
}

FString UTurboStructLiteBPLibrary::GenerateWALPath(const FString& SlotName, int32 SubSlotIndex, const FString& OpLabel)
{
	const FString SavePath = BuildSavePath(SlotName);
	const FString SaveDir = FPaths::GetPath(SavePath);
	const FString BaseName = FPaths::GetBaseFilename(SavePath);
	const FString SafeOpLabel = FPaths::MakeValidFileName(OpLabel, TEXT('_'));
	const FDateTime Now = FDateTime::Now();
	const FString TimeStamp = FString::Printf(TEXT("%04d-%02d-%02d_%02d-%02d-%02d-%03d"), Now.GetYear(), Now.GetMonth(), Now.GetDay(), Now.GetHour(), Now.GetMinute(), Now.GetSecond(), Now.GetMillisecond());
	const FString FileName = FString::Printf(TEXT("%s_%d_%s_%s.log"), *BaseName, SubSlotIndex, *SafeOpLabel, *TimeStamp);
	return FPaths::Combine(SaveDir, FileName);
}

void UTurboStructLiteBPLibrary::WriteWALEntry(const FString& WalPath, const FString& Message)
{
	if (WalPath.IsEmpty())
	{
		return;
	}
	const FDateTime Now = FDateTime::Now();
	const FString Stamp = FString::Printf(TEXT("%02d/%02d/%04d %02d:%02d:%02d.%03d"), Now.GetDay(), Now.GetMonth(), Now.GetYear(), Now.GetHour(), Now.GetMinute(), Now.GetSecond(), Now.GetMillisecond());
	const FString Line = FString::Printf(TEXT("[%s] %s\n"), *Stamp, *Message);
	FFileHelper::SaveStringToFile(Line, *WalPath, FFileHelper::EEncodingOptions::ForceUTF8, &IFileManager::Get(), FILEWRITE_Append);
}

void UTurboStructLiteBPLibrary::DeleteWALFile(const FString& WalPath)
{
	if (WalPath.IsEmpty())
	{
		return;
	}
	if (!IFileManager::Get().FileExists(*WalPath))
	{
		return;
	}
	IFileManager::Get().Delete(*WalPath, false, true, true);
}

bool UTurboStructLiteBPLibrary::ReadTurboStructLiteFile(const FString& FilePath, TMap<int32, FTurboStructLiteEntry>& OutEntries)
{
	OutEntries.Reset();
	if (!FPaths::FileExists(FilePath))
	{
		return true;
	}
	TArray<uint8> FileBytes;
	if (!FFileHelper::LoadFileToArray(FileBytes, *FilePath))
	{
		return false;
	}
	FMemoryReader Reader(FileBytes, true);
	int32 Version = 1;
	int32 EntryCount = 0;
	if (!ReadHeaderAndEntryCount(Reader, GetMagic(), Version, EntryCount))
	{
		return false;
	}
	for (int32 Index = 0; Index < EntryCount; ++Index)
	{
		if (Reader.AtEnd())
		{
			return false;
		}
		int32 SubSlot = 0;
		Reader << SubSlot;
		uint8 CompressionByte = 0;
		Reader << CompressionByte;
		uint8 EncryptionByte = 0;
		if (Version >= 3)
		{
			Reader << EncryptionByte;
		}
		int32 UncompressedSize = 0;
		Reader << UncompressedSize;
		int32 DataSize = 0;
		Reader << DataSize;
		int32 MetaSize = 0;
		if (Version >= 2)
		{
			Reader << MetaSize;
			if (MetaSize < 0)
			{
				return false;
			}
		}
		if (DataSize < 0 || Reader.AtEnd())
		{
			return false;
		}
		TArray<uint8> Data;
		Data.SetNum(DataSize);
		if (DataSize > 0)
		{
			Reader.Serialize(Data.GetData(), DataSize);
		}
		if (Version >= 2 && MetaSize > 0)
		{
			if (!SkipData(Reader, MetaSize))
			{
				return false;
			}
		}
		FTurboStructLiteEntry Entry;
		Entry.Compression = static_cast<ETurboStructLiteCompression>(CompressionByte);
		Entry.Encryption = static_cast<ETurboStructLiteEncryption>(EncryptionByte);
		Entry.UncompressedSize = UncompressedSize;
		Entry.Data = MoveTemp(Data);
		OutEntries.Add(SubSlot, MoveTemp(Entry));
	}
	return true;
}

bool UTurboStructLiteBPLibrary::WriteTurboStructLiteFile(const FString& FilePath, const TMap<int32, FTurboStructLiteEntry>& Entries)
{
	TArray<uint8> FileBytes;
	FMemoryWriter Writer(FileBytes, true);
	int32 Magic = GetMagic();
	Writer << Magic;
	int32 Version = GetVersion();
	Writer << Version;
	int64 Timestamp = FDateTime::UtcNow().ToUnixTimestamp();
	Writer << Timestamp;
	int32 EntryCount = Entries.Num();
	Writer << EntryCount;
	for (const auto& Pair : Entries)
	{
		int32 SubSlot = Pair.Key;
		Writer << SubSlot;
		uint8 CompressionByte = static_cast<uint8>(Pair.Value.Compression);
		Writer << CompressionByte;
		uint8 EncryptionByte = static_cast<uint8>(Pair.Value.Encryption);
		Writer << EncryptionByte;
		int32 UncompressedSize = Pair.Value.UncompressedSize;
		Writer << UncompressedSize;
		int32 DataSize = Pair.Value.Data.Num();
		Writer << DataSize;
		int32 MetaSize = 0;
		Writer << MetaSize;
		if (DataSize > 0)
		{
			Writer.Serialize(const_cast<uint8*>(Pair.Value.Data.GetData()), DataSize);
		}
	}

return FFileHelper::SaveArrayToFile(FileBytes, *FilePath);
}

void UTurboStructLiteBPLibrary::InvalidateSlotIndex(const FString& SlotName)
{
	FScopeLock Lock(&SlotIndexMutex);
	const FString SanitizedName = SanitizeSlotName(SlotName);
	CachedSlotIndexes.Remove(SanitizedName);
}

void UTurboStructLiteBPLibrary::InvalidateAllSlotIndexes()
{
	FScopeLock Lock(&SlotIndexMutex);
	CachedSlotIndexes.Reset();
}

bool UTurboStructLiteBPLibrary::BuildSlotIndex(const FString& SlotName, FTurboStructLiteSlotIndex& OutIndex)
{
#if WITH_EDITOR
	TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("TurboStructLite_BuildSlotIndex"));
#endif
	OutIndex = FTurboStructLiteSlotIndex();
	const FString FilePath = BuildSavePath(SlotName);
	TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*FilePath));
	if (!Reader)
	{
		return false;
	}
	int32 EntryCount = 0;
	int32 Version = 1;
	if (!ReadHeaderAndEntryCount(*Reader, GetMagic(), Version, EntryCount))
	{
		return false;
	}
	OutIndex.FileSizeBytes = IFileManager::Get().FileSize(*FilePath);
	OutIndex.Timestamp = IFileManager::Get().GetTimeStamp(*FilePath);
	OutIndex.EntryCount = EntryCount;
	for (int32 Index = 0; Index < EntryCount; ++Index)
	{
		if (Reader->AtEnd())
		{
			return false;
		}
		int32 FoundSubSlot = 0;
		uint8 CompressionByte = 0;
		uint8 EncryptionByte = 0;
		int32 UncompressedSize = 0;
		int32 DataSize = 0;
		*Reader << FoundSubSlot;
		*Reader << CompressionByte;
		if (Version >= 3)
		{
			*Reader << EncryptionByte;
		}
		*Reader << UncompressedSize;
		*Reader << DataSize;
		if (!IsValidBufferSize(*Reader, DataSize))
		{
			return false;
		}
		int32 MetaSize = 0;
		if (Version >= 2)
		{
			*Reader << MetaSize;
			if (!IsValidBufferSize(*Reader, MetaSize))
			{
				return false;
			}
		}
		FTurboStructLiteCachedEntry Entry;
		Entry.Compression = static_cast<ETurboStructLiteCompression>(CompressionByte);
		Entry.Encryption = static_cast<ETurboStructLiteEncryption>(EncryptionByte);
		Entry.UncompressedSize = UncompressedSize;
		Entry.DataSize = DataSize;
		Entry.MetaSize = MetaSize;
		Entry.DataOffset = Reader->Tell();
		if (!SkipData(*Reader, DataSize))
		{
			return false;
		}
		Entry.MetaOffset = Reader->Tell();
		if (Version >= 2 && MetaSize > 0)
		{
			if (!SkipData(*Reader, MetaSize))
			{
				return false;
			}
		}
		OutIndex.OrderedSubSlots.Add(FoundSubSlot);
		OutIndex.Entries.Add(FoundSubSlot, Entry);
	}
	return true;
}

bool UTurboStructLiteBPLibrary::GetSlotIndex(const FString& SlotName, FTurboStructLiteSlotIndex& OutIndex)
{
#if WITH_EDITOR
	TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("TurboStructLite_GetSlotIndex"));
#endif
	const FString SanitizedName = SanitizeSlotName(SlotName);
	const FString FilePath = BuildSavePath(SlotName);
	if (!FPaths::FileExists(FilePath))
	{
		return false;
	}
	const int64 CurrentSize = IFileManager::Get().FileSize(*FilePath);
	const FDateTime CurrentTime = IFileManager::Get().GetTimeStamp(*FilePath);
	{
		FScopeLock Lock(&SlotIndexMutex);
		if (FTurboStructLiteSlotIndex* Found = CachedSlotIndexes.Find(SanitizedName))
		{
			if (Found->FileSizeBytes == CurrentSize && Found->Timestamp == CurrentTime)
			{
				OutIndex = *Found;
				return true;
			}
		}
	}
	FTurboStructLiteSlotIndex BuiltIndex;
	if (!BuildSlotIndex(SlotName, BuiltIndex))
	{
		return false;
	}
	{
		FScopeLock Lock(&SlotIndexMutex);
		CachedSlotIndexes.Add(SanitizedName, BuiltIndex);
	}
	OutIndex = BuiltIndex;
	return true;
}

bool UTurboStructLiteBPLibrary::GetCachedEntry(const FString& SlotName, int32 SubSlotIndex, FTurboStructLiteCachedEntry& OutEntry)
{
#if WITH_EDITOR
	TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("TurboStructLite_Load_GetCachedEntry"));
#endif
	FTurboStructLiteSlotIndex Index;
	if (!GetSlotIndex(SlotName, Index))
	{
		return false;
	}
	if (const FTurboStructLiteCachedEntry* Found = Index.Entries.Find(SubSlotIndex))
	{
		OutEntry = *Found;
		return true;
	}
	return false;
}

bool UTurboStructLiteBPLibrary::EncryptDataBuffer(ETurboStructLiteEncryption Method, const FString& Key, TArray<uint8>& InOutData)
{
#if WITH_EDITOR
	TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("TurboStructLite_EncryptDataBuffer"));
#endif
	if (Method == ETurboStructLiteEncryption::None)
	{
		return true;
	}
	if (Method == ETurboStructLiteEncryption::ProjectDefault)
	{
		Method = UTurboStructLiteBPLibrary::GetActiveEncryptionMode();
	}
	if (Method != ETurboStructLiteEncryption::AES)
	{
		return true;
	}
	if (Key.IsEmpty())
	{
		return false;
	}
	const int32 SaltSize = 16;
#if !TURBOSTRUCTLITE_USE_OPENSSL
	TUniquePtr<FEncryptionContext> Context = IPlatformCrypto::Get().CreateContext();
	if (!Context)
	{
		return false;
	}
#endif
	TArray<uint8> Salt;
	Salt.SetNum(SaltSize);
#if TURBOSTRUCTLITE_USE_OPENSSL
	if (RAND_bytes(Salt.GetData(), SaltSize) != 1)
	{
		return false;
	}
#else
	if (Context->CreateRandomBytes(TArrayView<uint8>(Salt.GetData(), Salt.Num())) != EPlatformCryptoResult::Success)
	{
		return false;
	}
#endif
	uint8 Derived[64];
	if (!DeriveAESKeyFromString(Key, Salt, Derived))
	{
		return false;
	}
	bool bSuccess = false;
	const int32 IVSize = 12;
	TArray<uint8> IV;
	IV.SetNum(IVSize);
	TArray<uint8> Payload;

#if TURBOSTRUCTLITE_USE_OPENSSL
	EVP_CIPHER_CTX* Ctx = nullptr;
	TArray<uint8> Ciphertext;
	int32 Len = 0;
	int32 CiphertextLen = 0;
	const int32 TagSize = 16;
	TArray<uint8> Tag;

	if (RAND_bytes(IV.GetData(), IVSize) != 1)
	{
		goto Cleanup;
	}
	Ctx = EVP_CIPHER_CTX_new();
	if (!Ctx)
	{
		goto Cleanup;
	}
	if (1 != EVP_EncryptInit_ex(Ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr))
	{
		goto Cleanup;
	}
	if (1 != EVP_EncryptInit_ex(Ctx, nullptr, nullptr, Derived, IV.GetData()))
	{
		goto Cleanup;
	}
	Ciphertext.SetNum(InOutData.Num() + 16);
	if (1 != EVP_EncryptUpdate(Ctx, Ciphertext.GetData(), &Len, InOutData.GetData(), InOutData.Num()))
	{
		goto Cleanup;
	}
	CiphertextLen = Len;
	if (1 != EVP_EncryptFinal_ex(Ctx, Ciphertext.GetData() + Len, &Len))
	{
		goto Cleanup;
	}
	CiphertextLen += Len;
	Ciphertext.SetNum(CiphertextLen);
	Tag.SetNum(TagSize);
	if (1 != EVP_CIPHER_CTX_ctrl(Ctx, EVP_CTRL_GCM_GET_TAG, TagSize, Tag.GetData()))
	{
		goto Cleanup;
	}

	Payload.Reserve(SaltSize + IVSize + TagSize + Ciphertext.Num());
	Payload.Append(Salt);
	Payload.Append(IV);
	Payload.Append(Tag);
	Payload.Append(Ciphertext);
	InOutData = MoveTemp(Payload);
	bSuccess = true;

Cleanup:
	if (Ctx)
	{
		EVP_CIPHER_CTX_free(Ctx);
	}
	if (Salt.Num() > 0)
	{
		FMemory::Memzero(Salt.GetData(), Salt.Num());
	}
	if (IV.Num() > 0)
	{
		FMemory::Memzero(IV.GetData(), IV.Num());
	}
	if (Ciphertext.Num() > 0)
	{
		FMemory::Memzero(Ciphertext.GetData(), Ciphertext.Num());
	}
	if (Tag.Num() > 0)
	{
		FMemory::Memzero(Tag.GetData(), Tag.Num());
	}
	if (Payload.Num() > 0 && !bSuccess)
	{
		FMemory::Memzero(Payload.GetData(), Payload.Num());
	}
	FMemory::Memzero(Derived, sizeof(Derived));
	return bSuccess;
#else
	TArray<uint8> Tag;
	TArray<uint8> Ciphertext;

	if (Context->CreateRandomBytes(TArrayView<uint8>(IV.GetData(), IV.Num())) != EPlatformCryptoResult::Success)
	{
		goto Cleanup;
	}
	if (!EncryptAesGcmFallback(Derived, IV.GetData(), IV.Num(), InOutData.GetData(), InOutData.Num(), Ciphertext, Tag))
	{
		goto Cleanup;
	}

	Payload.Reserve(SaltSize + IVSize + Tag.Num() + Ciphertext.Num());
	Payload.Append(Salt);
	Payload.Append(IV);
	Payload.Append(Tag);
	Payload.Append(Ciphertext);
	InOutData = MoveTemp(Payload);
	bSuccess = true;

Cleanup:
	if (Salt.Num() > 0)
	{
		FMemory::Memzero(Salt.GetData(), Salt.Num());
	}
	if (IV.Num() > 0)
	{
		FMemory::Memzero(IV.GetData(), IV.Num());
	}
	if (Ciphertext.Num() > 0)
	{
		FMemory::Memzero(Ciphertext.GetData(), Ciphertext.Num());
	}
	if (Tag.Num() > 0)
	{
		FMemory::Memzero(Tag.GetData(), Tag.Num());
	}
	if (Payload.Num() > 0 && !bSuccess)
	{
		FMemory::Memzero(Payload.GetData(), Payload.Num());
	}
	FMemory::Memzero(Derived, sizeof(Derived));
	return bSuccess;
#endif
}

bool UTurboStructLiteBPLibrary::DecryptDataBuffer(ETurboStructLiteEncryption Method, const FString& Key, TArray<uint8>& InOutData)
{
#if WITH_EDITOR
	TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("TurboStructLite_DecryptDataBuffer"));
#endif
	if (Method == ETurboStructLiteEncryption::None)
	{
		return true;
	}
	if (Method == ETurboStructLiteEncryption::ProjectDefault)
	{
		Method = UTurboStructLiteBPLibrary::GetActiveEncryptionMode();
	}
	if (Method != ETurboStructLiteEncryption::AES)
	{
		return true;
	}
	if (Key.IsEmpty())
	{
		return false;
	}
	const int32 Overhead = 16 + 12 + 16;
	if (InOutData.Num() <= Overhead)
	{
		return false;
	}
	const uint8* Ptr = InOutData.GetData();
	const uint8* SaltPtr = Ptr;
	Ptr += 16;
	const uint8* IVPtr = Ptr;
	Ptr += 12;
	const uint8* TagPtr = Ptr;
	Ptr += 16;
	const uint8* DataPtr = Ptr;
	const int32 DataLen = InOutData.Num() - Overhead;
	TArray<uint8> SaltCopy;
	SaltCopy.Append(SaltPtr, 16);
	uint8 Derived[64];
	if (!DeriveAESKeyFromString(Key, SaltCopy, Derived))
	{
		return false;
	}
	bool bSuccess = false;
#if TURBOSTRUCTLITE_USE_OPENSSL
	EVP_CIPHER_CTX* Ctx = nullptr;
	TArray<uint8> Plaintext;
	int32 Len = 0;
	int32 PlaintextLen = 0;
	int32 Ret = 0;

	Ctx = EVP_CIPHER_CTX_new();
	if (!Ctx)
	{
		goto Cleanup;
	}
	if (1 != EVP_DecryptInit_ex(Ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr))
	{
		goto Cleanup;
	}
	if (1 != EVP_DecryptInit_ex(Ctx, nullptr, nullptr, Derived, IVPtr))
	{
		goto Cleanup;
	}
	Plaintext.SetNum(DataLen + 16);
	if (1 != EVP_DecryptUpdate(Ctx, Plaintext.GetData(), &Len, DataPtr, DataLen))
	{
		goto Cleanup;
	}
	PlaintextLen = Len;
	if (1 != EVP_CIPHER_CTX_ctrl(Ctx, EVP_CTRL_GCM_SET_TAG, 16, (void*)TagPtr))
	{
		goto Cleanup;
	}
	Ret = EVP_DecryptFinal_ex(Ctx, Plaintext.GetData() + Len, &Len);
	if (Ret <= 0)
	{
		goto Cleanup;
	}
	PlaintextLen += Len;
	Plaintext.SetNum(PlaintextLen);
	InOutData = MoveTemp(Plaintext);
	bSuccess = true;

Cleanup:
	if (Ctx)
	{
		EVP_CIPHER_CTX_free(Ctx);
	}
	if (Plaintext.Num() > 0 && !bSuccess)
	{
		FMemory::Memzero(Plaintext.GetData(), Plaintext.Num());
	}
	if (SaltCopy.Num() > 0)
	{
		FMemory::Memzero(SaltCopy.GetData(), SaltCopy.Num());
	}
	FMemory::Memzero(Derived, sizeof(Derived));
	return bSuccess;
#else
	TArray<uint8> Plaintext;
	if (!DecryptAesGcmFallback(Derived, IVPtr, 12, DataPtr, DataLen, TagPtr, 16, Plaintext))
	{
		goto Cleanup;
	}
	InOutData = MoveTemp(Plaintext);
	bSuccess = true;

Cleanup:
	if (Plaintext.Num() > 0 && !bSuccess)
	{
		FMemory::Memzero(Plaintext.GetData(), Plaintext.Num());
	}
	if (SaltCopy.Num() > 0)
	{
		FMemory::Memzero(SaltCopy.GetData(), SaltCopy.Num());
	}
	FMemory::Memzero(Derived, sizeof(Derived));
	return bSuccess;
#endif
}

bool UTurboStructLiteBPLibrary::SaveEntry(const FString& SlotName, int32 SubSlotIndex, ETurboStructLiteCompression Compression, ETurboStructLiteEncryption Encryption, const FString& EncryptionKey, const TArray<uint8>& RawBytes, const FString& DebugMeta, int32 MaxParallelThreads, ETurboStructLiteBatchingSetting CompressionBatching, bool bUseWriteAheadLog, const FString& WALPath)
{
#if WITH_EDITOR
	TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("TurboStructLite_SaveEntry"));
#endif
	if (bUseWriteAheadLog)
	{
		WriteWALEntry(WALPath, FString::Printf(TEXT("Start SaveEntry Slot=%s SubSlot=%d Bytes=%d Compression=%d Encryption=%d"), *SlotName, SubSlotIndex, RawBytes.Num(), static_cast<int32>(Compression), static_cast<int32>(Encryption)));
	}
	TArray<uint8> CompressedBytes;
	const int32 ResolvedBatchMB = UTurboStructLiteBPLibrary::ResolveBatchingMB(CompressionBatching);
	if (bUseWriteAheadLog)
	{
		WriteWALEntry(WALPath, TEXT("Compress start"));
	}
	if (!CompressBuffer(Compression, RawBytes, CompressedBytes, MaxParallelThreads, ResolvedBatchMB))
	{
		if (bUseWriteAheadLog)
		{
			WriteWALEntry(WALPath, TEXT("Compress failed"));
		}
		return false;
	}
	if (bUseWriteAheadLog)
	{
		WriteWALEntry(WALPath, FString::Printf(TEXT("Compress success Size=%d"), CompressedBytes.Num()));
	}

	TArray<uint8> Payload = MoveTemp(CompressedBytes);
	if (Encryption != ETurboStructLiteEncryption::None)
	{
		if (bUseWriteAheadLog)
		{
			WriteWALEntry(WALPath, TEXT("Encrypt start"));
		}
		if (!EncryptDataBuffer(Encryption, EncryptionKey, Payload))
		{
			if (bUseWriteAheadLog)
			{
				WriteWALEntry(WALPath, TEXT("Encrypt failed"));
			}
			return false;
		}
		if (bUseWriteAheadLog)
		{
			WriteWALEntry(WALPath, FString::Printf(TEXT("Encrypt success Size=%d"), Payload.Num()));
		}
	}

	FTurboStructLiteEntry NewEntry;
	NewEntry.Compression = Compression;
	NewEntry.Encryption = Encryption;
	NewEntry.UncompressedSize = RawBytes.Num();
	NewEntry.Data = MoveTemp(Payload);

	const FString FilePath = BuildSavePath(SlotName);
	const FString TempPath = FilePath + TEXT(".tmp");

	TUniquePtr<FArchive> Reader;
	if (FPaths::FileExists(FilePath))
	{
		Reader.Reset(IFileManager::Get().CreateFileReader(*FilePath));
		if (!Reader)
		{
			if (bUseWriteAheadLog)
			{
				WriteWALEntry(WALPath, TEXT("Open reader failed"));
			}
			return false;
		}
	}

	TUniquePtr<FArchive> Writer(IFileManager::Get().CreateFileWriter(*TempPath));
	if (!Writer)
	{
		if (bUseWriteAheadLog)
		{
			WriteWALEntry(WALPath, TEXT("Open writer failed"));
		}
		return false;
	}
	if (bUseWriteAheadLog)
	{
		WriteWALEntry(WALPath, FString::Printf(TEXT("Write temp start %s"), *TempPath));
	}

	auto FailAndCleanup = [&]() -> bool
	{
		Writer.Reset();
		Reader.Reset();
		IFileManager::Get().Delete(*TempPath);
		if (bUseWriteAheadLog)
		{
			WriteWALEntry(WALPath, TEXT("Abort and cleanup"));
		}
		return false;
	};

	int32 Magic = GetMagic();
	*Writer << Magic;
	int32 Version = GetVersion();
	*Writer << Version;
	int64 Timestamp = FDateTime::UtcNow().ToUnixTimestamp();
	*Writer << Timestamp;

	const int64 EntryCountOffset = Writer->Tell();
	int32 EntryCount = 0;
	*Writer << EntryCount;

	if (Reader)
	{
		if (bUseWriteAheadLog)
		{
			WriteWALEntry(WALPath, TEXT("Copy existing entries"));
		}
		int32 ExistingEntryCount = 0;
		int32 ExistingVersion = 1;
		if (!ReadHeaderAndEntryCount(*Reader, GetMagic(), ExistingVersion, ExistingEntryCount))
		{
			return FailAndCleanup();
		}

		const int64 FileSize = Reader->TotalSize();
		const int32 BufferSize = CalcStreamingBufferSize(FileSize);
		TArray<uint8> Buffer;
		Buffer.SetNum(BufferSize);

		for (int32 Index = 0; Index < ExistingEntryCount; ++Index)
		{
			if (Reader->AtEnd())
			{
				return FailAndCleanup();
			}
			int32 FoundSubSlot = 0;
			uint8 CompressionByte = 0;
			uint8 EncryptionByte = 0;
			int32 UncompressedSize = 0;
			int32 DataSize = 0;
			int32 MetaSize = 0;
			*Reader << FoundSubSlot;
			*Reader << CompressionByte;
			if (ExistingVersion >= 3)
			{
				*Reader << EncryptionByte;
			}
			*Reader << UncompressedSize;
			*Reader << DataSize;

			if (DataSize < 0)
			{
				return FailAndCleanup();
			}

			if (ExistingVersion >= 2)
			{
				*Reader << MetaSize;
				if (MetaSize < 0)
				{
					return FailAndCleanup();
				}
			}

			if (FoundSubSlot == SubSlotIndex)
			{
				if (!SkipData(*Reader, DataSize))
				{
					return FailAndCleanup();
				}
				if (ExistingVersion >= 2 && MetaSize > 0)
				{
					if (!SkipData(*Reader, MetaSize))
					{
						return FailAndCleanup();
					}
				}
				continue;
			}

			*Writer << FoundSubSlot;
			*Writer << CompressionByte;
			uint8 EncryptionToWrite = (ExistingVersion >= 3) ? EncryptionByte : static_cast<uint8>(ETurboStructLiteEncryption::None);
			*Writer << EncryptionToWrite;
			*Writer << UncompressedSize;
			*Writer << DataSize;
			int32 MetaSizeToWrite = (ExistingVersion >= 2) ? MetaSize : 0;
			*Writer << MetaSizeToWrite;

			int32 Remaining = DataSize;
			while (Remaining > 0)
			{
				const int32 ChunkSize = FMath::Min(Remaining, Buffer.Num());
				Reader->Serialize(Buffer.GetData(), ChunkSize);
				if (Reader->IsError())
				{
					return FailAndCleanup();
				}
				Writer->Serialize(Buffer.GetData(), ChunkSize);
				if (Writer->IsError())
				{
					return FailAndCleanup();
				}
				Remaining -= ChunkSize;
			}

			if (MetaSizeToWrite > 0)
			{
				int32 RemainingMeta = MetaSizeToWrite;
				while (RemainingMeta > 0)
				{
					const int32 ChunkSize = FMath::Min(RemainingMeta, Buffer.Num());
					Reader->Serialize(Buffer.GetData(), ChunkSize);
					if (Reader->IsError())
					{
						return FailAndCleanup();
					}
					Writer->Serialize(Buffer.GetData(), ChunkSize);
					if (Writer->IsError())
					{
						return FailAndCleanup();
					}
					RemainingMeta -= ChunkSize;
				}
			}

			++EntryCount;
		}
	}

	const FTCHARToUTF8 MetaUtf8(*DebugMeta);
	int32 MetaSize = MetaUtf8.Length();

	int32 SubSlotToWrite = SubSlotIndex;
	*Writer << SubSlotToWrite;
	uint8 CompressionByte = static_cast<uint8>(Compression);
	*Writer << CompressionByte;
	uint8 EncryptionByte = static_cast<uint8>(Encryption);
	*Writer << EncryptionByte;
	int32 UncompressedSize = NewEntry.UncompressedSize;
	*Writer << UncompressedSize;
	int32 DataSize = NewEntry.Data.Num();
	*Writer << DataSize;
	TArray<uint8> MetaBytes;
	if (MetaSize > 0)
	{
		MetaBytes.Append(reinterpret_cast<const uint8*>(MetaUtf8.Get()), MetaSize);
		if (Encryption == ETurboStructLiteEncryption::AES)
		{
			TArray<uint8> EncryptedMeta = MetaBytes;
			if (EncryptDataBuffer(Encryption, EncryptionKey, EncryptedMeta))
			{
				MetaBytes = MoveTemp(EncryptedMeta);
			}
			else
			{
				MetaBytes.Reset();
			}
		}
		MetaSize = MetaBytes.Num();
	}
	*Writer << MetaSize;
	if (DataSize > 0)
	{
		Writer->Serialize(const_cast<uint8*>(NewEntry.Data.GetData()), DataSize);
	}
	if (MetaSize > 0)
	{
		Writer->Serialize(MetaBytes.GetData(), MetaSize);
	}
	++EntryCount;

	Writer->Seek(EntryCountOffset);
	*Writer << EntryCount;

	Writer.Reset();
	Reader.Reset();

	if (bUseWriteAheadLog)
	{
		WriteWALEntry(WALPath, TEXT("Move temp to final start"));
	}
	if (!IFileManager::Get().Move(*FilePath, *TempPath, true, true, false, true))
	{
		IFileManager::Get().Delete(*TempPath);
		if (bUseWriteAheadLog)
		{
			WriteWALEntry(WALPath, TEXT("Move temp to final failed"));
		}
		return false;
	}

	if (bUseWriteAheadLog)
	{
		WriteWALEntry(WALPath, TEXT("Move temp to final success"));
	}
	InvalidateSlotIndex(SlotName);
	if (bUseWriteAheadLog)
	{
		WriteWALEntry(WALPath, TEXT("SaveEntry completed"));
	}
	return true;
}

bool UTurboStructLiteBPLibrary::ListSubSlotIndices(const FString& SlotName, TArray<int32>& OutSubSlots)
{
	OutSubSlots.Reset();
	FTurboStructLiteSlotIndex Index;
	if (!GetSlotIndex(SlotName, Index))
	{
		return false;
	}
	OutSubSlots = Index.OrderedSubSlots;
	return true;
}

bool UTurboStructLiteBPLibrary::ListSubSlotInfos(const FString& SlotName, TArray<FTurboStructLiteSubSlotInfo>& OutInfos)
{
	OutInfos.Reset();
	FTurboStructLiteSlotIndex Index;
	if (!GetSlotIndex(SlotName, Index))
	{
		return false;
	}
	const FString FilePath = BuildSavePath(SlotName);
	TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*FilePath));
	if (!Reader)
	{
		return false;
	}
	for (int32 SubSlot : Index.OrderedSubSlots)
	{
		const FTurboStructLiteCachedEntry* Cached = Index.Entries.Find(SubSlot);
		if (!Cached)
		{
			continue;
		}
		FTurboStructLiteSubSlotInfo Info;
		Info.SubSlotIndex = SubSlot;
		Info.DataSizeBytes = Cached->DataSize;
		Info.UncompressedSizeBytes = Cached->UncompressedSize;
		Info.Compression = Cached->Compression;
		Info.Encryption = Cached->Encryption;
		if (Cached->MetaSize > 0)
		{
			Reader->Seek(Cached->MetaOffset);
			if (!IsValidBufferSize(*Reader, Cached->MetaSize))
			{
				return false;
			}
			TArray<uint8> MetaBytes;
			MetaBytes.SetNum(Cached->MetaSize);
			Reader->Serialize(MetaBytes.GetData(), Cached->MetaSize);
			if (Info.Encryption == ETurboStructLiteEncryption::AES)
			{
				FString Key = GetActiveEncryptionKey();
				if (!Key.IsEmpty())
				{
					TArray<uint8> MetaCopy = MetaBytes;
					if (DecryptDataBuffer(ETurboStructLiteEncryption::AES, Key, MetaCopy))
					{
						Info.DebugMetadata = FString(UTF8_TO_TCHAR(MetaCopy.GetData()));
					}
				}
			}
			else
			{
				Info.DebugMetadata = FString(UTF8_TO_TCHAR(MetaBytes.GetData()));
			}
		}
		OutInfos.Add(Info);
	}
	return true;
}

bool UTurboStructLiteBPLibrary::ReadSubSlotInfoInternal(const FString& SlotName, int32 SubSlotIndex, const FString& EncryptionKey, ETurboStructLiteEncryption Encryption, FTurboStructLiteSubSlotInfo& OutInfo)
{
	OutInfo = FTurboStructLiteSubSlotInfo();
	FTurboStructLiteCachedEntry Cached;
	if (!GetCachedEntry(SlotName, SubSlotIndex, Cached))
	{
		return false;
	}
	OutInfo.SubSlotIndex = SubSlotIndex;
	OutInfo.DataSizeBytes = Cached.DataSize;
	OutInfo.UncompressedSizeBytes = Cached.UncompressedSize;
	OutInfo.Compression = Cached.Compression;
	OutInfo.Encryption = Cached.Encryption;
	if (Cached.MetaSize <= 0)
	{
		return true;
	}
	const FString FilePath = BuildSavePath(SlotName);
	TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*FilePath));
	if (!Reader)
	{
		return false;
	}
	Reader->Seek(Cached.MetaOffset);
	if (!IsValidBufferSize(*Reader, Cached.MetaSize))
	{
		return false;
	}
	TArray<uint8> MetaBytes;
	MetaBytes.SetNum(Cached.MetaSize);
	Reader->Serialize(MetaBytes.GetData(), Cached.MetaSize);
	ETurboStructLiteEncryption MetaEncryption = OutInfo.Encryption;
	if (MetaEncryption == ETurboStructLiteEncryption::ProjectDefault)
	{
		MetaEncryption = Encryption == ETurboStructLiteEncryption::ProjectDefault ? GetActiveEncryptionMode() : Encryption;
	}
	if (MetaEncryption == ETurboStructLiteEncryption::AES)
	{
		FString KeyToUse = EncryptionKey;
		if (KeyToUse.IsEmpty())
		{
			KeyToUse = GetActiveEncryptionKey();
		}
		if (!DecryptDataBuffer(ETurboStructLiteEncryption::AES, KeyToUse, MetaBytes))
		{
			return false;
		}
	}
	OutInfo.DebugMetadata = FString(UTF8_TO_TCHAR(MetaBytes.GetData()));
	return true;
}

bool UTurboStructLiteBPLibrary::GetSlotInfoInternal(const FString& SlotName, FTurboStructLiteSlotInfo& OutInfo)
{
	OutInfo = FTurboStructLiteSlotInfo();
	const FString FilePath = BuildSavePath(SlotName);
	if (!FPaths::FileExists(FilePath))
	{
		return false;
	}
	const int64 Size = IFileManager::Get().FileSize(*FilePath);
	const FDateTime TimeStamp = IFileManager::Get().GetTimeStamp(*FilePath);

	TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*FilePath));
	if (!Reader)
	{
		return false;
	}
	int32 EntryCount = 0;
	int32 Version = 1;
	if (!ReadHeaderAndEntryCount(*Reader, GetMagic(), Version, EntryCount))
	{
		return false;
	}

	OutInfo.FileSizeBytes = Size;
	OutInfo.Timestamp = TimeStamp;
	OutInfo.EntryCount = EntryCount;
	return true;
}

bool UTurboStructLiteBPLibrary::LoadEntry(const FString& SlotName, int32 SubSlotIndex, const FString& EncryptionKey, ETurboStructLiteEncryption DefaultEncryption, TArray<uint8>& OutRawBytes, bool bUseWriteAheadLog, const FString& WALPath)
{
#if WITH_EDITOR
	TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("TurboStructLite_LoadEntry"));
#endif
	if (bUseWriteAheadLog)
	{
		WriteWALEntry(WALPath, FString::Printf(TEXT("Start LoadEntry Slot=%s SubSlot=%d"), *SlotName, SubSlotIndex));
	}
	OutRawBytes.Reset();
	UTurboStructLiteBPLibrary::EnsureSettingsLoaded();
	FTurboStructLiteCachedEntry Cached;
	if (!GetCachedEntry(SlotName, SubSlotIndex, Cached))
	{
		if (bUseWriteAheadLog)
		{
			WriteWALEntry(WALPath, TEXT("Cached entry not found"));
		}
		return false;
	}
#if WITH_EDITOR
	TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("TurboStructLite_Load_OpenFile"));
#endif
	const FString FilePath = BuildSavePath(SlotName);
	if (bUseWriteAheadLog)
	{
		WriteWALEntry(WALPath, FString::Printf(TEXT("Open file %s"), *FilePath));
	}
	TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*FilePath));
	if (!Reader)
	{
		if (bUseWriteAheadLog)
		{
			WriteWALEntry(WALPath, TEXT("Open reader failed"));
		}
		return false;
	}
	Reader->Seek(Cached.DataOffset);
	if (!IsValidBufferSize(*Reader, Cached.DataSize))
	{
		if (bUseWriteAheadLog)
		{
			WriteWALEntry(WALPath, TEXT("Invalid data size"));
		}
		return false;
	}
	FTurboStructLiteEntry Entry;
	Entry.Compression = Cached.Compression;
	Entry.Encryption = Cached.Encryption;
	Entry.UncompressedSize = Cached.UncompressedSize;
	Entry.Data.SetNum(Cached.DataSize);
	if (Cached.DataSize > 0)
	{
		if (bUseWriteAheadLog)
		{
			WriteWALEntry(WALPath, FString::Printf(TEXT("Read data Size=%d"), Cached.DataSize));
		}
#if WITH_EDITOR
		TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("TurboStructLite_Load_ReadData"));
#endif
		Reader->Serialize(Entry.Data.GetData(), Cached.DataSize);
	}
	if (Cached.MetaSize > 0)
	{
		Reader->Seek(Cached.MetaOffset);
		if (!IsValidBufferSize(*Reader, Cached.MetaSize))
		{
			return false;
		}
	}
	ETurboStructLiteEncryption EffectiveEncryption = Entry.Encryption;
	if (EffectiveEncryption == ETurboStructLiteEncryption::ProjectDefault)
	{
		EffectiveEncryption = DefaultEncryption == ETurboStructLiteEncryption::ProjectDefault ? GetActiveEncryptionMode() : DefaultEncryption;
	}
	if (EffectiveEncryption == ETurboStructLiteEncryption::AES)
	{
#if WITH_EDITOR
		TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("TurboStructLite_Load_Decrypt"));
#endif
		if (bUseWriteAheadLog)
		{
			WriteWALEntry(WALPath, TEXT("Decrypt start"));
		}
		FString KeyToUse = EncryptionKey;
		if (KeyToUse.IsEmpty())
		{
			KeyToUse = GetActiveEncryptionKey();
		}
		if (KeyToUse.IsEmpty())
		{
			if (bUseWriteAheadLog)
			{
				WriteWALEntry(WALPath, TEXT("Decrypt missing key"));
			}
			return false;
		}
		if (!DecryptDataBuffer(ETurboStructLiteEncryption::AES, KeyToUse, Entry.Data))
		{
			if (bUseWriteAheadLog)
			{
				WriteWALEntry(WALPath, TEXT("Decrypt failed"));
			}
			return false;
		}
		if (bUseWriteAheadLog)
		{
			WriteWALEntry(WALPath, FString::Printf(TEXT("Decrypt success Size=%d"), Entry.Data.Num()));
		}
	}
#if WITH_EDITOR
	TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("TurboStructLite_Load_Decompress"));
#endif
	if (bUseWriteAheadLog)
	{
		WriteWALEntry(WALPath, TEXT("Decompress start"));
	}
	const bool bDecompressed = DecompressBuffer(Entry.Compression, Entry.Data, OutRawBytes);
	if (bUseWriteAheadLog)
	{
		WriteWALEntry(WALPath, bDecompressed ? FString::Printf(TEXT("Decompress success Size=%d"), OutRawBytes.Num()) : TEXT("Decompress failed"));
	}
	return bDecompressed;
}

bool UTurboStructLiteBPLibrary::ExistsEntry(const FString& SlotName, int32 SubSlotIndex)
{
	FTurboStructLiteCachedEntry Cached;
	return GetCachedEntry(SlotName, SubSlotIndex, Cached);
}

bool UTurboStructLiteBPLibrary::RemoveEntry(const FString& SlotName, int32 SubSlotIndex)
{
	const FString FilePath = BuildSavePath(SlotName);
	if (!FPaths::FileExists(FilePath))
	{
		return false;
	}

	const FString TempPath = FilePath + TEXT(".tmp");

	TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*FilePath));
	if (!Reader)
	{
		return false;
	}

	TUniquePtr<FArchive> Writer(IFileManager::Get().CreateFileWriter(*TempPath));
	if (!Writer)
	{
		return false;
	}

	auto FailAndCleanup = [&]() -> bool
	{
		Writer.Reset();
		Reader.Reset();
		IFileManager::Get().Delete(*TempPath);
		return false;
	};

	int32 EntryCount = 0;
	int32 ExistingVersion = 1;
	if (!ReadHeaderAndEntryCount(*Reader, GetMagic(), ExistingVersion, EntryCount))
	{
		return FailAndCleanup();
	}

	int32 Magic = GetMagic();
	*Writer << Magic;
	int32 VersionToWrite = GetVersion();
	*Writer << VersionToWrite;
	int64 Timestamp = FDateTime::UtcNow().ToUnixTimestamp();
	*Writer << Timestamp;
	const int64 EntryCountOffset = Writer->Tell();
	int32 NewEntryCount = 0;
	*Writer << NewEntryCount;

	bool bRemoved = false;
	const int64 FileSize = Reader->TotalSize();
	const int32 BufferSize = CalcStreamingBufferSize(FileSize);
	TArray<uint8> Buffer;
	Buffer.SetNum(BufferSize);

	for (int32 Index = 0; Index < EntryCount; ++Index)
	{
		if (Reader->AtEnd())
		{
			return FailAndCleanup();
		}

		int32 FoundSubSlot = 0;
		uint8 CompressionByte = 0;
		uint8 EncryptionByte = 0;
		int32 UncompressedSize = 0;
		int32 DataSize = 0;
		int32 MetaSize = 0;
		*Reader << FoundSubSlot;
		*Reader << CompressionByte;
		if (ExistingVersion >= 3)
		{
			*Reader << EncryptionByte;
		}
		*Reader << UncompressedSize;
		*Reader << DataSize;

		if (DataSize < 0)
		{
			return FailAndCleanup();
		}

		if (ExistingVersion >= 2)
		{
			*Reader << MetaSize;
			if (MetaSize < 0)
			{
				return FailAndCleanup();
			}
		}

		if (FoundSubSlot == SubSlotIndex)
		{
			if (!SkipData(*Reader, DataSize))
			{
				return FailAndCleanup();
			}
			if (ExistingVersion >= 2 && MetaSize > 0)
			{
				if (!SkipData(*Reader, MetaSize))
				{
					return FailAndCleanup();
				}
			}
			bRemoved = true;
			continue;
		}

		*Writer << FoundSubSlot;
		*Writer << CompressionByte;
		uint8 EncryptionToWrite = (ExistingVersion >= 3) ? EncryptionByte : static_cast<uint8>(ETurboStructLiteEncryption::None);
		*Writer << EncryptionToWrite;
		*Writer << UncompressedSize;
		*Writer << DataSize;
		int32 MetaSizeToWrite = (ExistingVersion >= 2) ? MetaSize : 0;
		*Writer << MetaSizeToWrite;

		int32 Remaining = DataSize;
		while (Remaining > 0)
		{
			const int32 ChunkSize = FMath::Min(Remaining, Buffer.Num());
			Reader->Serialize(Buffer.GetData(), ChunkSize);
			if (Reader->IsError())
			{
				return FailAndCleanup();
			}
			Writer->Serialize(Buffer.GetData(), ChunkSize);
			if (Writer->IsError())
			{
				return FailAndCleanup();
			}
			Remaining -= ChunkSize;
		}

		if (MetaSizeToWrite > 0)
		{
			int32 RemainingMeta = MetaSizeToWrite;
			while (RemainingMeta > 0)
			{
				const int32 ChunkSize = FMath::Min(RemainingMeta, Buffer.Num());
				Reader->Serialize(Buffer.GetData(), ChunkSize);
				if (Reader->IsError())
				{
					return FailAndCleanup();
				}
				Writer->Serialize(Buffer.GetData(), ChunkSize);
				if (Writer->IsError())
				{
					return FailAndCleanup();
				}
				RemainingMeta -= ChunkSize;
			}
		}

		++NewEntryCount;
	}

	if (!bRemoved)
	{
		return FailAndCleanup();
	}

	Writer->Seek(EntryCountOffset);
	*Writer << NewEntryCount;

	Writer.Reset();
	Reader.Reset();
	if (NewEntryCount == 0)
	{
		IFileManager::Get().Delete(*TempPath);
		const bool bDeleted = IFileManager::Get().Delete(*FilePath, false, true);
		if (bDeleted)
		{
			InvalidateSlotIndex(SlotName);
		}
		return bDeleted;
	}

	if (!IFileManager::Get().Move(*FilePath, *TempPath, true, true, false, true))
	{
		IFileManager::Get().Delete(*TempPath);
		return false;
	}

	InvalidateSlotIndex(SlotName);
	return true;
}



