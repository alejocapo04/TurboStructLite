#include "TurboStructLiteDatabaseParser.h"

#if WITH_EDITOR && TURBOSTRUCT_HAS_EDITOR_UI

#include "TurboStructLiteBPLibrary.h"
#include "UObject/UnrealType.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetData.h"
#include "Modules/ModuleManager.h"
#if (ENGINE_MAJOR_VERSION > 5) || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
#include "StructUtils/UserDefinedStruct.h"
#else
#include "Engine/UserDefinedStruct.h"
#endif
#include "Engine/UserDefinedEnum.h"

#if (ENGINE_MAJOR_VERSION > 5) || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
#define TURBOSTRUCT_PROPERTYBAG_HAS_UINT 1
#define TURBOSTRUCT_PROPERTYBAG_HAS_SETS 1
#else
#define TURBOSTRUCT_PROPERTYBAG_HAS_UINT 0
#define TURBOSTRUCT_PROPERTYBAG_HAS_SETS 0
#endif

FString FTurboStructLiteDatabaseParser::LastParseError;

void FTurboStructLiteDatabaseParser::SetLastParseError(const FString& Error)
{
	LastParseError = Error;
}

FString FTurboStructLiteDatabaseParser::GetLastParseError()
{
	return LastParseError;
}

UObject* FTurboStructLiteDatabaseParser::FindAssetByName(const FString& Name, const TArray<FTopLevelAssetPath>& ClassPaths)
{
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	FARFilter Filter;
	Filter.ClassPaths.Append(ClassPaths);
	Filter.bRecursiveClasses = true;
	Filter.PackagePaths.Add(FName(TEXT("/Game")));
	TArray<FAssetData> Assets;
	AssetRegistryModule.Get().GetAssets(Filter, Assets);
	for (const FAssetData& Asset : Assets)
	{
		if (Asset.AssetName.ToString() == Name)
		{
			return Asset.GetAsset();
		}
	}
	return nullptr;
}

bool FTurboStructLiteDatabaseParser::ResolveEnumType(const FString& InType, const UObject*& OutTypeObject)
{
	FString EnumName = InType;
	if (EnumName.StartsWith(TEXT("TEnumAsByte<")) && EnumName.EndsWith(TEXT(">")))
	{
		EnumName = EnumName.Mid(12, EnumName.Len() - 13);
	}
	if (EnumName.StartsWith(TEXT("enum ")))
	{
		EnumName = EnumName.Mid(5);
	}
	if (UEnum* FoundEnum = FindFirstObjectSafe<UEnum>(*EnumName))
	{
		OutTypeObject = FoundEnum;
		return true;
	}
	const TArray<FTopLevelAssetPath> EnumPaths = {
		UUserDefinedEnum::StaticClass()->GetClassPathName(),
		UEnum::StaticClass()->GetClassPathName()
	};
	if (UObject* Asset = FindAssetByName(EnumName, EnumPaths))
	{
		if (UEnum* AssetEnum = Cast<UEnum>(Asset))
		{
			OutTypeObject = AssetEnum;
			return true;
		}
	}
	return false;
}

bool FTurboStructLiteDatabaseParser::ResolveStructType(const FString& InType, const UObject*& OutTypeObject)
{
	if (UScriptStruct* FoundStruct = FindFirstObjectSafe<UScriptStruct>(*InType))
	{
		OutTypeObject = FoundStruct;
		return true;
	}
	if (InType.StartsWith(TEXT("F")) || InType.StartsWith(TEXT("BP_")))
	{
		const FString Trimmed = InType.StartsWith(TEXT("F")) ? InType.Mid(1) : InType;
		if (UScriptStruct* AltStruct = FindFirstObjectSafe<UScriptStruct>(*Trimmed))
		{
			OutTypeObject = AltStruct;
			return true;
		}
	}
	const TArray<FTopLevelAssetPath> StructPaths = {
		UUserDefinedStruct::StaticClass()->GetClassPathName(),
		UScriptStruct::StaticClass()->GetClassPathName()
	};
	if (UObject* Asset = FindAssetByName(InType, StructPaths))
	{
		if (UScriptStruct* AssetStruct = Cast<UScriptStruct>(Asset))
		{
			OutTypeObject = AssetStruct;
			return true;
		}
	}
	return false;
}

FString FTurboStructLiteDatabaseParser::StripWhitespace(const FString& In)
{
	FString Out = In;
	Out.ReplaceInline(TEXT(" "), TEXT(""));
	return Out;
}

bool FTurboStructLiteDatabaseParser::ParseContainerAndType(const FString& InType, FString& OutElementType, bool& bOutArray, bool& bOutSet, bool& bOutMap)
{
	FString Clean = StripWhitespace(InType);
	bOutArray = false;
	bOutSet = false;
	bOutMap = false;
	if (Clean.StartsWith(TEXT("TArray<")) && Clean.EndsWith(TEXT(">")))
	{
		OutElementType = Clean.Mid(7, Clean.Len() - 8);
		bOutArray = true;
		return true;
	}
	if (Clean.StartsWith(TEXT("TSet<")) && Clean.EndsWith(TEXT(">")))
	{
		OutElementType = Clean.Mid(5, Clean.Len() - 6);
		bOutSet = true;
		return true;
	}
	if (Clean.StartsWith(TEXT("TMap<")) && Clean.EndsWith(TEXT(">")))
	{
		bOutMap = true;
		return true;
	}
	OutElementType = Clean;
	return true;
}

bool FTurboStructLiteDatabaseParser::ResolveValueType(const FString& InType, EPropertyBagPropertyType& OutType, const UObject*& OutTypeObject)
{
	OutTypeObject = nullptr;
	const FString Lower = InType.ToLower();
	if (Lower == TEXT("bool"))
	{
		OutType = EPropertyBagPropertyType::Bool;
		return true;
	}
	if (Lower == TEXT("uint8") || Lower == TEXT("byte"))
	{
		OutType = EPropertyBagPropertyType::Byte;
		return true;
	}
	if (Lower == TEXT("int32"))
	{
		OutType = EPropertyBagPropertyType::Int32;
		return true;
	}
#if TURBOSTRUCT_PROPERTYBAG_HAS_UINT
	if (Lower == TEXT("uint32"))
	{
		OutType = EPropertyBagPropertyType::UInt32;
		return true;
	}
#else
	if (Lower == TEXT("uint32"))
	{
		OutType = EPropertyBagPropertyType::Int32;
		return true;
	}
#endif
	if (Lower == TEXT("int64"))
	{
		OutType = EPropertyBagPropertyType::Int64;
		return true;
	}
#if TURBOSTRUCT_PROPERTYBAG_HAS_UINT
	if (Lower == TEXT("uint64"))
	{
		OutType = EPropertyBagPropertyType::UInt64;
		return true;
	}
#else
	if (Lower == TEXT("uint64"))
	{
		OutType = EPropertyBagPropertyType::Int64;
		return true;
	}
#endif
	if (Lower == TEXT("float"))
	{
		OutType = EPropertyBagPropertyType::Float;
		return true;
	}
	if (Lower == TEXT("double"))
	{
		OutType = EPropertyBagPropertyType::Double;
		return true;
	}
	if (InType == TEXT("FName"))
	{
		OutType = EPropertyBagPropertyType::Name;
		return true;
	}
	if (InType == TEXT("FString"))
	{
		OutType = EPropertyBagPropertyType::String;
		return true;
	}
	if (InType == TEXT("FText"))
	{
		OutType = EPropertyBagPropertyType::Text;
		return true;
	}
	if (ResolveEnumType(InType, OutTypeObject))
	{
		OutType = EPropertyBagPropertyType::Enum;
		return true;
	}
	if (ResolveStructType(InType, OutTypeObject))
	{
		OutType = EPropertyBagPropertyType::Struct;
		return true;
	}
	return false;
}

bool FTurboStructLiteDatabaseParser::IsLikelyHex(const FString& Text)
{
	if (Text.Len() < 6 || Text.Len() > 40)
	{
		return false;
	}
	for (TCHAR C : Text)
	{
		if (!FChar::IsHexDigit(C))
		{
			return false;
		}
	}
	return true;
}

bool FTurboStructLiteDatabaseParser::IsAllDigits(const FString& Text)
{
	if (Text.IsEmpty())
	{
		return false;
	}
	for (TCHAR C : Text)
	{
		if (!FChar::IsDigit(C))
		{
			return false;
		}
	}
	return true;
}

FString FTurboStructLiteDatabaseParser::SanitizeFieldName(const FString& InName)
{
	if (InName.IsEmpty())
	{
		return FString();
	}
	if (InName.Len() > 1024)
	{
		return TEXT("InvalidName_TooLong");
	}

	TArray<FString> Parts;
	InName.ParseIntoArray(Parts, TEXT("_"), true);
	while (Parts.Num() > 1)
	{
		const FString& Tail = Parts.Last();
		const bool bHexTail = IsLikelyHex(Tail);
		const bool bNumericTail = FTurboStructLiteDatabaseParser::IsAllDigits(Tail);
		if (bHexTail || bNumericTail)
		{
			Parts.Pop();
			continue;
		}
		break;
	}
	FString Clean = FString::Join(Parts, TEXT("_"));
	while (Clean.Len() > 0 && !FChar::IsAlnum(Clean[Clean.Len() - 1]))
	{
		Clean.LeftChopInline(1);
	}
	return Clean;
}

FString FTurboStructLiteDatabaseParser::DescribePropertyReadable(const FProperty* Prop, int32 Depth)
{
	if (!Prop || Depth > 3)
	{
		return TEXT("...");
	}

	const FString CleanName = SanitizeFieldName(Prop->GetName());

	if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
	{
		const FString Inner = DescribePropertyReadable(ArrayProp->Inner, Depth + 1);
		return FString::Printf(TEXT("%s [Array %s]"), *CleanName, *Inner);
	}

	if (const FSetProperty* SetProp = CastField<FSetProperty>(Prop))
	{
		const FString Inner = DescribePropertyReadable(SetProp->ElementProp, Depth + 1);
		return FString::Printf(TEXT("%s [Set %s]"), *CleanName, *Inner);
	}

	if (const FMapProperty* MapProp = CastField<FMapProperty>(Prop))
	{
		const FString KeyDesc = DescribePropertyReadable(MapProp->KeyProp, Depth + 1);
		const FString ValDesc = DescribePropertyReadable(MapProp->ValueProp, Depth + 1);
		return FString::Printf(TEXT("%s [Map K=%s V=%s]"), *CleanName, *KeyDesc, *ValDesc);
	}

	if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
	{
		const FString InnerFields = DescribeStructFields(StructProp->Struct, Depth + 1);
		if (!InnerFields.IsEmpty())
		{
			return FString::Printf(TEXT("%s {%s}"), *CleanName, *InnerFields);
		}
		return CleanName;
	}

	return CleanName;
}

FString FTurboStructLiteDatabaseParser::DescribeStructFields(const UStruct* Struct, int32 Depth)
{
	if (!Struct || !Struct->IsValidLowLevelFast() || Depth > 3)
	{
		return FString();
	}

	TArray<FString> Names;
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		Names.Add(DescribePropertyReadable(*It, Depth));
	}

return FString::Join(Names, TEXT(", "));
}

void FTurboStructLiteDatabaseParser::GetStructFieldNames(const UStruct* Struct, TArray<FString>& OutNames, int32 Depth)
{
	if (!Struct || !Struct->IsValidLowLevelFast() || Depth > 3)
	{
		return;
	}
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		const FProperty* Prop = *It;
		if (!Prop)
		{
			continue;
		}
		const FString PropName = Prop->GetName();
		if (PropName.StartsWith(TEXT("TRASH_")) || PropName.StartsWith(TEXT("REINST_")) || PropName.Contains(TEXT("PLACEHOLDER")))
		{
			continue;
		}

		const FString CleanName = SanitizeFieldName(PropName);
		OutNames.Add(CleanName);
		if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			if (StructProp->Struct && StructProp->Struct->IsValidLowLevelFast())
			{
				GetStructFieldNames(StructProp->Struct, OutNames, Depth + 1);
			}
		}
	}
}

bool FTurboStructLiteDatabaseParser::ParseMetadata(const FString& InMetadata, FTurboStructLiteParsedProperty& OutMeta, FString& OutError)
{
	OutMeta = FTurboStructLiteParsedProperty();
	OutError.Reset();
	if (InMetadata.IsEmpty())
	{
		OutError = TEXT("Missing metadata");
		return false;
	}
	TArray<FString> Parts;
	InMetadata.ParseIntoArray(Parts, TEXT(";"), true);
	FString NameValue;
	FString IsArrayValue;
	FString TypeValue;
	FString FieldsValue;
	for (const FString& Part : Parts)
	{
		FString Key;
		FString Value;
		if (!Part.Split(TEXT("="), &Key, &Value))
		{
			continue;
		}
		if (Key == TEXT("Name"))
		{
			NameValue = Value;
		}
		else if (Key == TEXT("IsArray"))
		{
			IsArrayValue = Value;
		}
		else if (Key == TEXT("Type"))
		{
			TypeValue = Value;
		}
		else if (Key == TEXT("Fields"))
		{
			FieldsValue = Value;
		}
	}
	if (NameValue.IsEmpty() || TypeValue.IsEmpty())
	{
		OutError = TEXT("Invalid metadata");
		return false;
	}
	OutMeta.PropertyName = NameValue;
	FString ElementType;
	bool bIsArray = false;
	bool bIsSet = false;
	bool bIsMap = false;
	const UObject* ResolvedTypeObject = nullptr;
	if (!ParseContainerAndType(TypeValue, ElementType, bIsArray, bIsSet, bIsMap))
	{
		OutError = TEXT("Unrecognized type");
		return false;
	}
	OutMeta.TypeName = ElementType;
	OutMeta.bIsArray = IsArrayValue == TEXT("1") || bIsArray;
	OutMeta.bIsSet = bIsSet;
	if (bIsMap)
	{
		OutError = TEXT("Map container not supported for preview");
		return false;
	}
	if (!ResolveValueType(ElementType, OutMeta.ValueType, ResolvedTypeObject))
	{
		OutError = FString::Printf(TEXT("Unsupported type %s"), *ElementType);
		return false;
	}
	OutMeta.TypeObject = ResolvedTypeObject;
	if (!FieldsValue.IsEmpty())
	{
		FieldsValue.ParseIntoArray(OutMeta.FieldNames, TEXT(","), true);
	}
	return true;
}

bool FTurboStructLiteDatabaseParser::BuildBag(const FTurboStructLiteParsedProperty& Meta, FInstancedPropertyBag& Bag)
{
	Bag.Reset();
	if (Meta.ValueType == EPropertyBagPropertyType::None)
	{
		return false;
	}
	const UObject* TypeObject = Meta.TypeObject.Get();
	UObject* MutableTypeObject = const_cast<UObject*>(TypeObject);
	if (Meta.bIsArray)
	{
		Bag.AddContainerProperty(*Meta.PropertyName, EPropertyBagContainerType::Array, Meta.ValueType, MutableTypeObject);
	}
	else if (Meta.bIsSet)
	{
#if TURBOSTRUCT_PROPERTYBAG_HAS_SETS
		Bag.AddContainerProperty(*Meta.PropertyName, EPropertyBagContainerType::Set, Meta.ValueType, MutableTypeObject);
#else
		Bag.AddContainerProperty(*Meta.PropertyName, EPropertyBagContainerType::Array, Meta.ValueType, MutableTypeObject);
#endif
	}
	else
	{
		Bag.AddProperty(*Meta.PropertyName, Meta.ValueType, MutableTypeObject);
	}
	return Bag.IsValid();
}

FProperty* FTurboStructLiteDatabaseParser::ResolveBagProperty(const FInstancedPropertyBag& Bag, const FString& Name)
{
	const UPropertyBag* BagStruct = Bag.GetPropertyBagStruct();
	if (!BagStruct)
	{
		return nullptr;
	}
	if (const FPropertyBagPropertyDesc* Desc = BagStruct->FindPropertyDescByName(*Name))
	{
		if (Desc->CachedProperty)
		{
			return const_cast<FProperty*>(Desc->CachedProperty);
		}
	}
	return BagStruct->FindPropertyByName(*Name);
}

bool FTurboStructLiteDatabaseParser::DeserializeIntoBag(const FTurboStructLiteParsedProperty& Meta, FInstancedPropertyBag& Bag, const TArray<uint8>& Bytes, FString& OutError)
{
	OutError.Reset();
	if (!Bag.IsValid())
	{
		if (!BuildBag(Meta, Bag))
		{
			OutError = TEXT("Failed to build property bag");
			return false;
		}
	}
	FProperty* Property = ResolveBagProperty(Bag, Meta.PropertyName);
	if (!Property)
	{
		OutError = TEXT("Property not found");
		return false;
	}
	FStructView View = Bag.GetMutableValue();
	const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(View.GetMemory());
	if (!ValuePtr)
	{
		return false;
	}
	void* MutableValuePtr = const_cast<void*>(ValuePtr);
	if (!UTurboStructLiteBPLibrary::TurboStructLiteDeserializeProperty(Property, MutableValuePtr, Bytes))
	{
		OutError = TEXT("Deserialize failed");
		return false;
	}
	return true;
}

bool FTurboStructLiteDatabaseParser::SerializeFromBag(const FTurboStructLiteParsedProperty& Meta, const FInstancedPropertyBag& Bag, TArray<uint8>& OutBytes, FString& OutError)
{
	OutError.Reset();
	OutBytes.Reset();
	FProperty* Property = ResolveBagProperty(Bag, Meta.PropertyName);
	if (!Property)
	{
		OutError = TEXT("Property not found");
		return false;
	}
	FConstStructView View = Bag.GetValue();
	const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(View.GetMemory());
	if (!ValuePtr)
	{
		OutError = TEXT("Value pointer is null");
		return false;
	}
	void* WritePtr = const_cast<void*>(ValuePtr);
	if (!UTurboStructLiteBPLibrary::TurboStructLiteSerializeProperty(Property, WritePtr, OutBytes))
	{
		OutError = TEXT("Serialize failed");
		return false;
	}
	return true;
}

FString FTurboStructLiteDatabaseParser::DescribeCompression(ETurboStructLiteCompression Compression)
{
	switch (Compression)
	{
	case ETurboStructLiteCompression::None:
		return TEXT("None");
	case ETurboStructLiteCompression::LZ4:
		return TEXT("LZ4");
	case ETurboStructLiteCompression::Zlib:
		return TEXT("Zlib");
	case ETurboStructLiteCompression::Gzip:
		return TEXT("Gzip");
	case ETurboStructLiteCompression::Oodle:
		return TEXT("Oodle");
	default:
		return TEXT("ProjectDefault");
	}
}

FString FTurboStructLiteDatabaseParser::DescribeEncryption(ETurboStructLiteEncryption Encryption)
{
	switch (Encryption)
	{
	case ETurboStructLiteEncryption::None:
		return TEXT("None");
	case ETurboStructLiteEncryption::AES:
		return TEXT("AES");
	default:
		return TEXT("ProjectDefault");
	}
}

FString FTurboStructLiteDatabaseParser::BuildMetadataString(const FTurboStructLiteParsedProperty& Meta)
{
	FString Base = FString::Printf(TEXT("Name=%s;IsArray=%d;Type=%s"), *Meta.PropertyName, Meta.bIsArray ? 1 : 0, *Meta.TypeName);
	if (Meta.FieldNames.Num() > 0)
	{
		Base += TEXT(";Fields=") + FString::Join(Meta.FieldNames, TEXT(","));
	}
	return Base;
}

FString FTurboStructLiteDatabaseParser::FormatMetadataForDisplay(const FString& RawMetadata, const FTurboStructLiteParsedProperty& Parsed)
{
	FTurboStructLiteParsedProperty Meta = Parsed;
	if (Meta.ValueType == EPropertyBagPropertyType::None && !RawMetadata.IsEmpty())
	{
		FString Dummy;
		ParseMetadata(RawMetadata, Meta, Dummy);
	}

	FString Name = Meta.PropertyName.IsEmpty() ? TEXT("Property") : Meta.PropertyName;
	FString Type = Meta.TypeName.IsEmpty() ? TEXT("Unknown") : Meta.TypeName;
	if (Meta.bIsArray)
	{
		Type = FString::Printf(TEXT("Array of %s"), *Type);
	}
	else if (Meta.bIsSet)
	{
		Type = FString::Printf(TEXT("Set of %s"), *Type);
	}

	FString Result = FString::Printf(TEXT("%s: %s"), *Name, *Type);

	if (const UStruct* Struct = Cast<UStruct>(Meta.TypeObject.Get()))
	{
		const FString Fields = DescribeStructFields(Struct, 0);
		if (!Fields.IsEmpty())
		{
			Result += TEXT(" | Fields: ") + Fields;
		}
	}
	else if (Meta.FieldNames.Num() > 0)
	{
		TArray<FString> Fields;
		Fields.Reserve(Meta.FieldNames.Num());
		for (const FString& Field : Meta.FieldNames)
		{
			Fields.Add(SanitizeFieldName(Field));
		}
		Result += TEXT(" | Fields: ") + FString::Join(Fields, TEXT(", "));
	}
	else if (Meta.ValueType == EPropertyBagPropertyType::None && !RawMetadata.IsEmpty())
	{
		Result = RawMetadata;
	}

	return Result;
}

bool FTurboStructLiteDatabaseParser::CompressBuffer(ETurboStructLiteCompression Method, const TArray<uint8>& In, TArray<uint8>& Out, int32 MaxParallelThreads, int32 ChunkBatchSizeMB)
{
	return UTurboStructLiteBPLibrary::CompressBuffer(Method, In, Out, MaxParallelThreads, ChunkBatchSizeMB);
}

bool FTurboStructLiteDatabaseParser::DecompressBuffer(ETurboStructLiteCompression Method, const TArray<uint8>& In, TArray<uint8>& Out)
{
	return UTurboStructLiteBPLibrary::DecompressBuffer(Method, In, Out);
}

bool FTurboStructLiteDatabaseParser::EncryptDataBuffer(ETurboStructLiteEncryption Method, const FString& Key, TArray<uint8>& InOutData)
{
	return UTurboStructLiteBPLibrary::EncryptDataBuffer(Method, Key, InOutData);
}

bool FTurboStructLiteDatabaseParser::DecryptDataBuffer(ETurboStructLiteEncryption Method, const FString& Key, TArray<uint8>& InOutData)
{
	return UTurboStructLiteBPLibrary::DecryptDataBuffer(Method, Key, InOutData);
}

bool FTurboStructLiteDatabaseParser::SerializePropertyWithMeta(FProperty* Property, void* Address, TArray<uint8>& OutBytes, FString& OutDebugMeta, bool bSaveOnlyMarked)
{
	return UTurboStructLiteBPLibrary::SerializePropertyWithMeta(Property, Address, OutBytes, OutDebugMeta, bSaveOnlyMarked);
}

bool FTurboStructLiteDatabaseParser::DeserializePropertyWithMeta(FProperty* Property, void* Address, const TArray<uint8>& InBytes, int32 OverrideMaxThreads, bool bSaveOnlyMarked)
{
	return UTurboStructLiteBPLibrary::DeserializePropertyWithMeta(Property, Address, InBytes, OverrideMaxThreads, bSaveOnlyMarked);
}

#endif



