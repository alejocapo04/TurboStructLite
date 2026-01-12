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
#include "TurboStructLiteDebugMacros.h"
#include "Async/ParallelFor.h"
#include "HAL/ThreadSafeBool.h"
#include "Runtime/Launch/Resources/Version.h"


bool UTurboStructLiteBPLibrary::StructMatchesFields(const UStruct* Struct, const TArray<FString>& FieldNames)
{
	if (!Struct || FieldNames.IsEmpty())
	{
		return true;
	}
	int32 Index = 0;
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		if (!FieldNames.IsValidIndex(Index))
		{
			return false;
		}
		if (It->GetName() != FieldNames[Index])
		{
			return false;
		}
		Index++;
	}
	return Index == FieldNames.Num();
}

FString UTurboStructLiteBPLibrary::NormalizeTypeName(const FString& InType)
{
	FString Out = InType;
	Out.TrimStartAndEndInline();
	if (Out.StartsWith(TEXT("struct ")))
	{
		Out = Out.Mid(7);
	}
	else if (Out.StartsWith(TEXT("class ")))
	{
		Out = Out.Mid(6);
	}
	Out.ReplaceInline(TEXT(" "), TEXT(""));
	return Out.ToLower();
}

FString UTurboStructLiteBPLibrary::NormalizeMetaFieldName(const FString& InName)
{
	FString Out = InName;
	Out.TrimStartAndEndInline();
	int32 LastUnderscore = INDEX_NONE;
	if (!Out.FindLastChar(TEXT('_'), LastUnderscore))
	{
		return Out;
	}
	const FString Suffix = Out.Mid(LastUnderscore + 1);
	if (Suffix.Len() != 32)
	{
		return Out;
	}
	bool bHex = true;
	for (int32 Index = 0; Index < Suffix.Len(); ++Index)
	{
		if (!FChar::IsHexDigit(Suffix[Index]))
		{
			bHex = false;
			break;
		}
	}
	if (!bHex)
	{
		return Out;
	}
	if (LastUnderscore <= 0)
	{
		return Out;
	}
	const FString Left = Out.Left(LastUnderscore);
	int32 SecondLastUnderscore = INDEX_NONE;
	if (!Left.FindLastChar(TEXT('_'), SecondLastUnderscore))
	{
		return Out;
	}
	const FString NumericSuffix = Out.Mid(SecondLastUnderscore + 1, LastUnderscore - SecondLastUnderscore - 1);
	if (NumericSuffix.IsEmpty())
	{
		return Out;
	}
	for (int32 Index = 0; Index < NumericSuffix.Len(); ++Index)
	{
		if (!FChar::IsDigit(NumericSuffix[Index]))
		{
			return Out;
		}
	}
	return Out.Left(SecondLastUnderscore);
}

bool UTurboStructLiteBPLibrary::NamesMatchForMigration(const FString& MetaName, const FProperty* Property)
{
	if (!Property)
	{
		return false;
	}
	const FString NormalMeta = NormalizeMetaFieldName(MetaName);
	const FString PropName = Property->GetName();
	const FString AuthoredName = Property->GetAuthoredName();
	if (NormalMeta.Equals(PropName, ESearchCase::IgnoreCase))
	{
		return true;
	}
	if (!AuthoredName.IsEmpty() && NormalMeta.Equals(AuthoredName, ESearchCase::IgnoreCase))
	{
		return true;
	}
	return false;
}

void UTurboStructLiteBPLibrary::CopyArchiveVersions(FArchive& TargetAr, const FArchive& SourceAr)
{
	TargetAr.SetUEVer(SourceAr.UEVer());
	TargetAr.SetLicenseeUEVer(SourceAr.LicenseeUEVer());
	TargetAr.SetEngineVer(SourceAr.EngineVer());
	TargetAr.SetCustomVersions(SourceAr.GetCustomVersions());
}

bool UTurboStructLiteBPLibrary::TryReadVariantFromMeta(const FTurboStructLiteFieldMeta& Meta, const uint8* DataPtr, int32 DataSize, bool bSaveOnlyMarked, const FArchive& VersionSource, FTurboStructLiteVariant& OutVariant, bool& bOutReaderError)
{
	OutVariant = FTurboStructLiteVariant();
	bOutReaderError = false;
	if (!DataPtr || DataSize <= 0)
	{
		return false;
	}
	if (Meta.Children.Num() > 0)
	{
		return false;
	}
	const FString NormalType = NormalizeTypeName(Meta.Type);
	if (NormalType.StartsWith(TEXT("tarray<")) || NormalType.StartsWith(TEXT("tset<")) || NormalType.StartsWith(TEXT("tmap<")))
	{
		return false;
	}
	FMemoryReaderView ReaderView(MakeArrayView(DataPtr, DataSize));
	FObjectAndNameAsStringProxyArchive Ar(ReaderView, true);
	CopyArchiveVersions(Ar, VersionSource);
	Ar.ArIsSaveGame = bSaveOnlyMarked;
	Ar.ArNoDelta = true;

	auto ReadIntBySize = [&](bool bUnsigned, int32 Size, int64& OutValue) -> bool
	{
		if (Size == 1)
		{
			if (bUnsigned)
			{
				uint8 Value = 0;
				Ar << Value;
				OutValue = static_cast<int64>(Value);
			}
			else
			{
				int8 Value = 0;
				Ar << Value;
				OutValue = static_cast<int64>(Value);
			}
			return true;
		}
		if (Size == 2)
		{
			if (bUnsigned)
			{
				uint16 Value = 0;
				Ar << Value;
				OutValue = static_cast<int64>(Value);
			}
			else
			{
				int16 Value = 0;
				Ar << Value;
				OutValue = static_cast<int64>(Value);
			}
			return true;
		}
		if (Size == 4)
		{
			if (bUnsigned)
			{
				uint32 Value = 0;
				Ar << Value;
				OutValue = static_cast<int64>(Value);
			}
			else
			{
				int32 Value = 0;
				Ar << Value;
				OutValue = static_cast<int64>(Value);
			}
			return true;
		}
		if (Size == 8)
		{
			if (bUnsigned)
			{
				uint64 Value = 0;
				Ar << Value;
				OutValue = static_cast<int64>(Value);
			}
			else
			{
				int64 Value = 0;
				Ar << Value;
				OutValue = Value;
			}
			return true;
		}
		return false;
	};

	if (NormalType == TEXT("bool"))
	{
		bool Value = false;
		if (Meta.Size >= static_cast<int32>(sizeof(uint32)))
		{
			uint32 RawValue = 0;
			Ar << RawValue;
			Value = RawValue != 0;
		}
		else
		{
			uint8 RawValue = 0;
			Ar << RawValue;
			Value = RawValue != 0;
		}
		if (ReaderView.IsError())
		{
			bOutReaderError = true;
			return false;
		}
		OutVariant.Type = ETurboStructLiteVariantType::Bool;
		OutVariant.BoolValue = Value;
		OutVariant.IntValue = Value ? 1 : 0;
		OutVariant.FloatValue = Value ? 1.0 : 0.0;
		OutVariant.StringValue = Value ? TEXT("true") : TEXT("false");
		return true;
	}
	if (NormalType == TEXT("float"))
	{
		float Value = 0.0f;
		Ar << Value;
		if (ReaderView.IsError())
		{
			bOutReaderError = true;
			return false;
		}
		OutVariant.Type = ETurboStructLiteVariantType::Float;
		OutVariant.FloatValue = static_cast<double>(Value);
		OutVariant.IntValue = static_cast<int64>(Value);
		OutVariant.StringValue = LexToString(Value);
		return true;
	}
	if (NormalType == TEXT("double"))
	{
		double Value = 0.0;
		Ar << Value;
		if (ReaderView.IsError())
		{
			bOutReaderError = true;
			return false;
		}
		OutVariant.Type = ETurboStructLiteVariantType::Float;
		OutVariant.FloatValue = Value;
		OutVariant.IntValue = static_cast<int64>(Value);
		OutVariant.StringValue = LexToString(Value);
		return true;
	}
	if (NormalType == TEXT("fstring"))
	{
		FString Value;
		Ar << Value;
		if (ReaderView.IsError())
		{
			bOutReaderError = true;
			return false;
		}
		OutVariant.Type = ETurboStructLiteVariantType::String;
		OutVariant.StringValue = Value;
		return true;
	}
	if (NormalType == TEXT("fname"))
	{
		FString NameString;
		Ar << NameString;
		if (ReaderView.IsError())
		{
			bOutReaderError = true;
			return false;
		}
		OutVariant.Type = ETurboStructLiteVariantType::Name;
		OutVariant.NameValue = FName(*NameString);
		OutVariant.StringValue = NameString;
		return true;
	}
	if (NormalType == TEXT("ftext"))
	{
		FText Value;
		static_cast<FArchiveProxy&>(Ar) << Value;
		if (ReaderView.IsError())
		{
			bOutReaderError = true;
			return false;
		}
		OutVariant.Type = ETurboStructLiteVariantType::String;
		OutVariant.StringValue = Value.ToString();
		return true;
	}

	bool bIsEnum = false;
	bool bEnumUnsigned = false;
	FString EnumType;
	if (NormalType.StartsWith(TEXT("tenumasbyte<")))
	{
		bIsEnum = true;
		bEnumUnsigned = true;
		const int32 Open = Meta.Type.Find(TEXT("<"));
		const int32 Close = Meta.Type.Find(TEXT(">"), ESearchCase::IgnoreCase, ESearchDir::FromStart, Open + 1);
		if (Open != INDEX_NONE && Close != INDEX_NONE && Close > Open + 1)
		{
			EnumType = Meta.Type.Mid(Open + 1, Close - Open - 1);
			EnumType.TrimStartAndEndInline();
		}
	}
	else
	{
		EnumType = Meta.Type;
		EnumType.TrimStartAndEndInline();
		if (EnumType.StartsWith(TEXT("enum class "), ESearchCase::IgnoreCase))
		{
			EnumType = EnumType.Mid(11);
			EnumType.TrimStartAndEndInline();
		}
		else if (EnumType.StartsWith(TEXT("enum "), ESearchCase::IgnoreCase))
		{
			EnumType = EnumType.Mid(5);
			EnumType.TrimStartAndEndInline();
		}
		else if (EnumType.StartsWith(TEXT("class "), ESearchCase::IgnoreCase))
		{
			EnumType = EnumType.Mid(6);
			EnumType.TrimStartAndEndInline();
		}
	}
	const UEnum* Enum = nullptr;
	if (!EnumType.IsEmpty())
	{
		Enum = FindFirstObject<UEnum>(*EnumType, EFindFirstObjectOptions::None);
		if (Enum)
		{
			bIsEnum = true;
		}
	}
	if (bIsEnum)
	{
		if (Enum)
		{
			FMemoryReaderView NameReaderView(MakeArrayView(DataPtr, DataSize));
			FObjectAndNameAsStringProxyArchive NameAr(NameReaderView, true);
			CopyArchiveVersions(NameAr, VersionSource);
			NameAr.ArIsSaveGame = bSaveOnlyMarked;
			NameAr.ArNoDelta = true;
			FString EnumNameString;
			NameAr << EnumNameString;
			if (!NameReaderView.IsError())
			{
				int64 EnumValue = INDEX_NONE;
				if (Enum->HasAnyEnumFlags(EEnumFlags::Flags))
				{
					EnumValue = Enum->GetValueOrBitfieldFromString(EnumNameString);
				}
				else
				{
					EnumValue = Enum->GetValueByNameString(EnumNameString);
				}
				if (EnumValue != INDEX_NONE)
				{
					OutVariant.Type = ETurboStructLiteVariantType::Int;
					OutVariant.IntValue = EnumValue;
					OutVariant.FloatValue = static_cast<double>(EnumValue);
					OutVariant.StringValue = LexToString(EnumValue);
					return true;
				}
			}
		}
		int64 Value = 0;
		if (!ReadIntBySize(bEnumUnsigned, Meta.Size, Value))
		{
			return false;
		}
		if (ReaderView.IsError())
		{
			bOutReaderError = true;
			return false;
		}
		OutVariant.Type = ETurboStructLiteVariantType::Int;
		OutVariant.IntValue = Value;
		OutVariant.FloatValue = static_cast<double>(Value);
		OutVariant.StringValue = LexToString(Value);
		return true;
	}

	bool bUnsigned = false;
	if (NormalType == TEXT("uint8") || NormalType == TEXT("uint16") || NormalType == TEXT("uint32") || NormalType == TEXT("uint64"))
	{
		bUnsigned = true;
	}
	if (NormalType == TEXT("int8") || NormalType == TEXT("int16") || NormalType == TEXT("int32") || NormalType == TEXT("int64")
		|| NormalType == TEXT("uint8") || NormalType == TEXT("uint16") || NormalType == TEXT("uint32") || NormalType == TEXT("uint64"))
	{
		int64 Value = 0;
		if (!ReadIntBySize(bUnsigned, Meta.Size, Value))
		{
			return false;
		}
		if (ReaderView.IsError())
		{
			bOutReaderError = true;
			return false;
		}
		OutVariant.Type = ETurboStructLiteVariantType::Int;
		OutVariant.IntValue = Value;
		OutVariant.FloatValue = static_cast<double>(Value);
		OutVariant.StringValue = LexToString(Value);
		return true;
	}

	return false;
}

bool UTurboStructLiteBPLibrary::TryApplyVariantToProperty(FProperty* Property, void* Address, const FTurboStructLiteVariant& Variant)
{
	if (!Property || !Address)
	{
		return false;
	}

	if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
	{
		bool Value = false;
		if (Variant.Type == ETurboStructLiteVariantType::Bool)
		{
			Value = Variant.BoolValue;
		}
		else if (Variant.Type == ETurboStructLiteVariantType::Int)
		{
			Value = Variant.IntValue != 0;
		}
		else if (Variant.Type == ETurboStructLiteVariantType::Float)
		{
			Value = !FMath::IsNearlyZero(Variant.FloatValue);
		}
		else
		{
			const FString TextValue = Variant.StringValue;
			if (!LexTryParseString(Value, *TextValue))
			{
				const FString Lower = TextValue.ToLower();
				if (Lower == TEXT("true") || Lower == TEXT("1") || Lower == TEXT("yes"))
				{
					Value = true;
				}
				else if (Lower == TEXT("false") || Lower == TEXT("0") || Lower == TEXT("no"))
				{
					Value = false;
				}
				else
				{
					return false;
				}
			}
		}
		BoolProp->SetPropertyValue(Address, Value);
		return true;
	}

	auto IsUnsignedNumericProperty = [](const FNumericProperty* NumProp) -> bool
	{
		return CastField<FByteProperty>(NumProp) || CastField<FUInt16Property>(NumProp) || CastField<FUInt32Property>(NumProp) || CastField<FUInt64Property>(NumProp);
	};

	auto GetNumericFromString = [&](const FString& TextValue, bool bFloat, double& OutFloat, int64& OutInt) -> bool
	{
		if (bFloat)
		{
			return LexTryParseString(OutFloat, *TextValue);
		}
		return LexTryParseString(OutInt, *TextValue);
	};

	if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
	{
		int64 Value = 0;
		bool bHasValue = false;
		if (Variant.Type == ETurboStructLiteVariantType::Int)
		{
			Value = Variant.IntValue;
			bHasValue = true;
		}
		else if (Variant.Type == ETurboStructLiteVariantType::Float)
		{
			Value = static_cast<int64>(Variant.FloatValue);
			bHasValue = true;
		}
		else
		{
			const UEnum* Enum = EnumProp->GetEnum();
			if (Enum)
			{
				const int64 EnumValue = Enum->GetValueByNameString(Variant.StringValue);
				if (EnumValue != INDEX_NONE)
				{
					Value = EnumValue;
					bHasValue = true;
				}
			}
			if (!bHasValue)
			{
				int64 Parsed = 0;
				if (LexTryParseString(Parsed, *Variant.StringValue))
				{
					Value = Parsed;
					bHasValue = true;
				}
			}
		}
		if (!bHasValue)
		{
			return false;
		}
		FNumericProperty* Underlying = EnumProp->GetUnderlyingProperty();
		if (!Underlying)
		{
			return false;
		}
		if (IsUnsignedNumericProperty(Underlying) && Value < 0)
		{
			Value = 0;
		}
		if (IsUnsignedNumericProperty(Underlying))
		{
			Underlying->SetIntPropertyValue(Address, static_cast<uint64>(Value));
		}
		else
		{
			Underlying->SetIntPropertyValue(Address, static_cast<int64>(Value));
		}
		return true;
	}

	if (FNumericProperty* NumProp = CastField<FNumericProperty>(Property))
	{
		if (NumProp->IsFloatingPoint())
		{
			double Value = 0.0;
			if (Variant.Type == ETurboStructLiteVariantType::Float)
			{
				Value = Variant.FloatValue;
			}
			else if (Variant.Type == ETurboStructLiteVariantType::Int)
			{
				Value = static_cast<double>(Variant.IntValue);
			}
			else if (Variant.Type == ETurboStructLiteVariantType::Bool)
			{
				Value = Variant.BoolValue ? 1.0 : 0.0;
			}
		else
		{
			int64 DummyInt = 0;
			if (!GetNumericFromString(Variant.StringValue, true, Value, DummyInt))
			{
				return false;
			}
		}
			NumProp->SetFloatingPointPropertyValue(Address, Value);
			return true;
		}
		int64 Value = 0;
		bool bHasValue = true;
		if (Variant.Type == ETurboStructLiteVariantType::Int)
		{
			Value = Variant.IntValue;
		}
		else if (Variant.Type == ETurboStructLiteVariantType::Float)
		{
			Value = static_cast<int64>(Variant.FloatValue);
		}
		else if (Variant.Type == ETurboStructLiteVariantType::Bool)
		{
			Value = Variant.BoolValue ? 1 : 0;
		}
		else
		{
			double DummyFloat = 0.0;
			bHasValue = GetNumericFromString(Variant.StringValue, false, DummyFloat, Value);
		}
		if (!bHasValue)
		{
			return false;
		}
		const bool bUnsigned = IsUnsignedNumericProperty(NumProp);
		if (bUnsigned && Value < 0)
		{
			Value = 0;
		}
		if (bUnsigned)
		{
			NumProp->SetIntPropertyValue(Address, static_cast<uint64>(Value));
		}
		else
		{
			NumProp->SetIntPropertyValue(Address, static_cast<int64>(Value));
		}
		return true;
	}

	if (FStrProperty* StrProp = CastField<FStrProperty>(Property))
	{
		if (Variant.Type == ETurboStructLiteVariantType::Name)
		{
			StrProp->SetPropertyValue(Address, Variant.NameValue.ToString());
		}
		else
		{
			StrProp->SetPropertyValue(Address, Variant.StringValue);
		}
		return true;
	}

	if (FNameProperty* NameProp = CastField<FNameProperty>(Property))
	{
		if (Variant.Type == ETurboStructLiteVariantType::Name)
		{
			NameProp->SetPropertyValue(Address, Variant.NameValue);
		}
		else
		{
			NameProp->SetPropertyValue(Address, FName(*Variant.StringValue));
		}
		return true;
	}

	if (FTextProperty* TextProp = CastField<FTextProperty>(Property))
	{
		FText Value = FText::FromString(Variant.StringValue);
		TextProp->SetPropertyValue(Address, Value);
		return true;
	}

	return false;
}

bool UTurboStructLiteBPLibrary::TryMigratePropertyValue(const FTurboStructLiteFieldMeta& Meta, FProperty* Property, void* Address, const uint8* DataPtr, int32 DataSize, bool bSaveOnlyMarked, const FArchive& VersionSource, bool& bOutReaderError)
{
	bOutReaderError = false;
	if (!Property || !Address || !DataPtr || DataSize <= 0)
	{
		return false;
	}
	if (IsUnsupportedProperty(Property))
	{
		return false;
	}
	if (Property->IsA<FArrayProperty>() || Property->IsA<FSetProperty>() || Property->IsA<FMapProperty>())
	{
		return false;
	}
	FTurboStructLiteVariant Variant;
	if (!TryReadVariantFromMeta(Meta, DataPtr, DataSize, bSaveOnlyMarked, VersionSource, Variant, bOutReaderError))
	{
		return false;
	}
	return TryApplyVariantToProperty(Property, Address, Variant);
}

bool UTurboStructLiteBPLibrary::BuildVariantFromProperty(const FProperty* Property, const void* ValuePtr, FTurboStructLiteVariant& OutVariant)
{
	OutVariant = FTurboStructLiteVariant();
	if (!Property || !ValuePtr)
	{
		return false;
	}

	if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
	{
		const bool Value = BoolProp->GetPropertyValue(ValuePtr);
		OutVariant.Type = ETurboStructLiteVariantType::Bool;
		OutVariant.BoolValue = Value;
		OutVariant.StringValue = Value ? TEXT("true") : TEXT("false");
		return true;
	}
	if (const FNumericProperty* NumProp = CastField<FNumericProperty>(Property))
	{
		if (NumProp->IsInteger())
		{
			const int64 Value = NumProp->GetSignedIntPropertyValue(ValuePtr);
			OutVariant.Type = ETurboStructLiteVariantType::Int;
			OutVariant.IntValue = Value;
			OutVariant.FloatValue = static_cast<double>(Value);
			OutVariant.StringValue = LexToString(Value);
			return true;
		}
		const double Value = NumProp->GetFloatingPointPropertyValue(ValuePtr);
		OutVariant.Type = ETurboStructLiteVariantType::Float;
		OutVariant.FloatValue = Value;
		OutVariant.IntValue = static_cast<int64>(Value);
		OutVariant.StringValue = LexToString(Value);
		return true;
	}
	if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
	{
		const int64 Value = EnumProp->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
		OutVariant.Type = ETurboStructLiteVariantType::Int;
		OutVariant.IntValue = Value;
		OutVariant.FloatValue = static_cast<double>(Value);
		const UEnum* Enum = EnumProp->GetEnum();
		if (Enum)
		{
			const FString NameString = Enum->GetNameStringByValue(Value);
			OutVariant.StringValue = NameString;
			OutVariant.NameValue = FName(*NameString);
		}
		else
		{
			OutVariant.StringValue = LexToString(Value);
		}
		return true;
	}
	if (const FByteProperty* ByteProp = CastField<FByteProperty>(Property))
	{
		const int64 Value = ByteProp->GetSignedIntPropertyValue(ValuePtr);
		OutVariant.Type = ETurboStructLiteVariantType::Int;
		OutVariant.IntValue = Value;
		OutVariant.FloatValue = static_cast<double>(Value);
		if (ByteProp->Enum)
		{
			const FString NameString = ByteProp->Enum->GetNameStringByValue(Value);
			OutVariant.StringValue = NameString;
			OutVariant.NameValue = FName(*NameString);
		}
		else
		{
			OutVariant.StringValue = LexToString(Value);
		}
		return true;
	}
	if (const FNameProperty* NameProp = CastField<FNameProperty>(Property))
	{
		const FName Value = NameProp->GetPropertyValue(ValuePtr);
		OutVariant.Type = ETurboStructLiteVariantType::Name;
		OutVariant.NameValue = Value;
		OutVariant.StringValue = Value.ToString();
		return true;
	}
	if (const FStrProperty* StrProp = CastField<FStrProperty>(Property))
	{
		const FString* Value = StrProp->GetPropertyValuePtr(ValuePtr);
		if (Value)
		{
			OutVariant.Type = ETurboStructLiteVariantType::String;
			OutVariant.StringValue = *Value;
			return true;
		}
	}
	if (const FTextProperty* TextProp = CastField<FTextProperty>(Property))
	{
		const FText Value = TextProp->GetPropertyValue(ValuePtr);
		OutVariant.Type = ETurboStructLiteVariantType::String;
		OutVariant.StringValue = Value.ToString();
		return true;
	}

	FString Exported;
	Property->ExportTextItem_Direct(Exported, ValuePtr, nullptr, nullptr, PPF_None);
	OutVariant.Type = ETurboStructLiteVariantType::Struct;
	OutVariant.StringValue = Exported;
	return true;
}

bool UTurboStructLiteBPLibrary::TurboStructLiteValidateStructLayout(const FProperty* Property, const TArray<FString>& FieldNames)
{
	if (FieldNames.IsEmpty() || !Property)
	{
		return true;
	}

	if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
	{
		return StructMatchesFields(StructProp->Struct, FieldNames);
	}

	if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
	{
		if (const FStructProperty* InnerStruct = CastField<FStructProperty>(ArrayProp->Inner))
		{
			return StructMatchesFields(InnerStruct->Struct, FieldNames);
		}
	}

	if (const FSetProperty* SetProp = CastField<FSetProperty>(Property))
	{
		if (const FStructProperty* InnerStruct = CastField<FStructProperty>(SetProp->ElementProp))
		{
			return StructMatchesFields(InnerStruct->Struct, FieldNames);
		}
	}

	return true;
}


