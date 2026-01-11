#include "TurboStructLiteQueryLibrary.h"
#include "TurboStructLiteBPLibrary.h"
#include "TurboStructLite.h"
#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/ConfigCacheIni.h"
#include "Math/NumericLimits.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "Serialization/StructuredArchive.h"
#if __has_include("Serialization/StructuredArchiveAdapters.h")
#include "Serialization/StructuredArchiveAdapters.h"
#endif
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "HAL/ThreadSafeBool.h"
#include "Runtime/Launch/Resources/Version.h"
#include "UObject/Stack.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/Compression.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/Guid.h"
#if WITH_EDITOR
#include "Trace/Trace.inl"
#endif

bool UTurboStructLiteQueryLibrary::BuildLogicQueryContext(FProperty* DataProp, FTurboStructLiteLogicQueryContext& OutContext, FString& OutErrorMessage)
{
	OutContext = FTurboStructLiteLogicQueryContext();
	OutErrorMessage.Reset();
	if (!DataProp)
	{
		OutErrorMessage = TEXT("Type Error: Invalid context property");
		return false;
	}
	if (FStructProperty* StructProp = CastField<FStructProperty>(DataProp))
	{
		OutContext.RootStruct = StructProp->Struct;
		OutContext.RootProperty = DataProp;
		return true;
	}
	if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(DataProp))
	{
		if (FStructProperty* InnerStruct = CastField<FStructProperty>(ArrayProp->Inner))
		{
			OutContext.RootStruct = InnerStruct->Struct;
			return true;
		}
		OutErrorMessage = TEXT("Type Error: Array element must be a struct for logic queries");
		return false;
	}
	if (FSetProperty* SetProp = CastField<FSetProperty>(DataProp))
	{
		if (FStructProperty* InnerStruct = CastField<FStructProperty>(SetProp->ElementProp))
		{
			OutContext.RootStruct = InnerStruct->Struct;
			return true;
		}
		OutErrorMessage = TEXT("Type Error: Set element must be a struct for logic queries");
		return false;
	}
	if (FMapProperty* MapProp = CastField<FMapProperty>(DataProp))
	{
		OutContext.MapKeyProperty = MapProp->KeyProp;
		OutContext.MapValueProperty = MapProp->ValueProp;
		OutContext.bAllowMapKeyValue = true;
		if (FStructProperty* ValueStruct = CastField<FStructProperty>(MapProp->ValueProp))
		{
			OutContext.RootStruct = ValueStruct->Struct;
		}
		return true;
	}
	OutErrorMessage = TEXT("Type Error: Unsupported context type for logic queries");
	return false;
}

bool UTurboStructLiteQueryLibrary::BuildLogicQueryContextFromStruct(UStruct* RootStruct, FTurboStructLiteLogicQueryContext& OutContext, FString& OutErrorMessage)
{
	OutContext = FTurboStructLiteLogicQueryContext();
	OutErrorMessage.Reset();
	if (!RootStruct)
	{
		OutErrorMessage = TEXT("Type Error: Invalid struct context");
		return false;
	}
	OutContext.RootStruct = RootStruct;
	return true;
}

bool UTurboStructLiteQueryLibrary::BindLogicQuery(TSharedPtr<FTurboStructLiteQueryNode>& Root, const FTurboStructLiteLogicQueryContext& Context, FString& OutErrorMessage, int32& OutErrorPos)
{
	if (!Root.IsValid())
	{
		OutErrorMessage = TEXT("Binder Error (col=1): Empty query");
		OutErrorPos = 1;
		return false;
	}

	auto MakeError = [&](const FString& Message, int32 Position) -> bool
	{
		OutErrorPos = Position;
		OutErrorMessage = FString::Printf(TEXT("Binder Error (col=%d): %s"), OutErrorPos, *Message);
		return false;
	};

	auto MakeTypeError = [&](const FString& Message, int32 Position) -> bool
	{
		OutErrorPos = Position;
		OutErrorMessage = FString::Printf(TEXT("Type Error (col=%d): %s"), OutErrorPos, *Message);
		return false;
	};

	auto ResolvePath = [&](FTurboStructLiteQueryComparison& Comparison) -> bool
	{
		FTurboStructLiteQueryBoundProperty& Bound = Comparison.Lhs;
		Bound.PropertyChain.Reset();
		Bound.LeafProperty = nullptr;
		Bound.ContainerElementProperty = nullptr;
		Bound.bIsContainer = false;
		Bound.bUseMapKey = false;
		Bound.bUseMapValue = false;

		TArray<FString> Segments = Bound.PathSegments;
		if (Segments.Num() == 0)
		{
			return MakeError(TEXT("Empty property path"), Comparison.Lhs.PathPosition);
		}

		FProperty* RootProperty = nullptr;
		if (Context.bAllowMapKeyValue && Segments.Num() > 0)
		{
			if (Segments[0].Equals(TEXT("Key"), ESearchCase::IgnoreCase))
			{
				Bound.Root = ETurboStructLiteQueryValueRoot::MapKey;
				Bound.bUseMapKey = true;
				RootProperty = Context.MapKeyProperty;
				Segments.RemoveAt(0);
			}
			else if (Segments[0].Equals(TEXT("Value"), ESearchCase::IgnoreCase))
			{
				Bound.Root = ETurboStructLiteQueryValueRoot::MapValue;
				Bound.bUseMapValue = true;
				RootProperty = Context.MapValueProperty;
				Segments.RemoveAt(0);
			}
		}

		auto FindPropertyByName = [&](UStruct* Struct, const FString& Segment, int32 Position) -> FProperty*
		{
			if (!Struct)
			{
				return nullptr;
			}
			FProperty* Found = FindFProperty<FProperty>(Struct, *Segment);
			if (Found)
			{
				return Found;
			}
			FProperty* Match = nullptr;
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				FProperty* Prop = *It;
				const FString DisplayName = Prop->GetDisplayNameText().ToString();
				if (DisplayName.Equals(Segment, ESearchCase::IgnoreCase))
				{
					if (Match)
					{
						MakeError(FString::Printf(TEXT("Ambiguous property '%s'"), *Segment), Position);
						return nullptr;
					}
					Match = Prop;
				}
			}
			return Match;
		};

		if (Bound.Root == ETurboStructLiteQueryValueRoot::MapKey || Bound.Root == ETurboStructLiteQueryValueRoot::MapValue)
		{
			if (!RootProperty)
			{
				return MakeError(TEXT("Map key/value is not available in this context"), Comparison.Lhs.PathPosition);
			}
			if (Segments.Num() == 0)
			{
				Bound.LeafProperty = RootProperty;
				return true;
			}
			if (FStructProperty* RootStructProp = CastField<FStructProperty>(RootProperty))
			{
				UStruct* CurrentStruct = RootStructProp->Struct;
				for (int32 Index = 0; Index < Segments.Num(); ++Index)
				{
					FProperty* Found = FindPropertyByName(CurrentStruct, Segments[Index], Comparison.Lhs.PathPosition);
					if (!Found)
					{
						return MakeError(FString::Printf(TEXT("Property '%s' not found"), *Segments[Index]), Comparison.Lhs.PathPosition);
					}
					Bound.PropertyChain.Add(Found);
					if (Index < Segments.Num() - 1)
					{
						FStructProperty* NextStruct = CastField<FStructProperty>(Found);
						if (!NextStruct)
						{
							return MakeError(FString::Printf(TEXT("Property '%s' is not a struct"), *Segments[Index]), Comparison.Lhs.PathPosition);
						}
						CurrentStruct = NextStruct->Struct;
					}
					else
					{
						Bound.LeafProperty = Found;
					}
				}
				return true;
			}
			return MakeError(TEXT("Map key/value is not a struct"), Comparison.Lhs.PathPosition);
		}

		if (!Context.RootStruct)
		{
			return MakeError(TEXT("Context struct is missing"), Comparison.Lhs.PathPosition);
		}

		UStruct* CurrentStruct = Context.RootStruct;
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			FProperty* Found = FindPropertyByName(CurrentStruct, Segments[Index], Comparison.Lhs.PathPosition);
			if (!Found)
			{
				return MakeError(FString::Printf(TEXT("Property '%s' not found"), *Segments[Index]), Comparison.Lhs.PathPosition);
			}
			Bound.PropertyChain.Add(Found);
			if (Index < Segments.Num() - 1)
			{
				FStructProperty* NextStruct = CastField<FStructProperty>(Found);
				if (!NextStruct)
				{
					return MakeError(FString::Printf(TEXT("Property '%s' is not a struct"), *Segments[Index]), Comparison.Lhs.PathPosition);
				}
				CurrentStruct = NextStruct->Struct;
			}
			else
			{
				Bound.LeafProperty = Found;
			}
		}
		return true;
	};

	auto ConvertLiteralForProperty = [&](const FProperty* Prop, const FTurboStructLiteQueryLiteral& InputLiteral, FTurboStructLiteQueryLiteral& OutLiteral, int32 Position, bool bAllowNumeric) -> bool
	{
		if (CastField<FBoolProperty>(Prop))
		{
			if (InputLiteral.Type != ETurboStructLiteQueryLiteralType::Boolean)
			{
				return MakeTypeError(TEXT("Expected boolean literal"), Position);
			}
			OutLiteral = InputLiteral;
			return true;
		}
		if (const FNumericProperty* NumProp = CastField<FNumericProperty>(Prop))
		{
			if (!bAllowNumeric)
			{
				return MakeTypeError(TEXT("Numeric operator not allowed for this type"), Position);
			}
			double Value = 0.0;
			if (InputLiteral.Type == ETurboStructLiteQueryLiteralType::Integer)
			{
				Value = static_cast<double>(InputLiteral.IntValue);
			}
			else if (InputLiteral.Type == ETurboStructLiteQueryLiteralType::Float)
			{
				Value = InputLiteral.FloatValue;
			}
			else if (InputLiteral.Type == ETurboStructLiteQueryLiteralType::String)
			{
				if (!FDefaultValueHelper::ParseDouble(InputLiteral.StringValue, Value))
				{
					return MakeTypeError(TEXT("Expected numeric literal"), Position);
				}
			}
			else
			{
				return MakeTypeError(TEXT("Expected numeric literal"), Position);
			}

			if (NumProp->IsInteger())
			{
				OutLiteral.Type = ETurboStructLiteQueryLiteralType::Integer;
				OutLiteral.IntValue = static_cast<int64>(Value);
				return true;
			}
			OutLiteral.Type = ETurboStructLiteQueryLiteralType::Float;
			OutLiteral.FloatValue = Value;
			return true;
		}
		if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			const UEnum* Enum = EnumProp->GetEnum();
			if (!Enum)
			{
				return MakeTypeError(TEXT("Enum property missing enum"), Position);
			}
			int64 EnumValue = 0;
			if (InputLiteral.Type == ETurboStructLiteQueryLiteralType::Integer)
			{
				EnumValue = InputLiteral.IntValue;
			}
			else if (InputLiteral.Type == ETurboStructLiteQueryLiteralType::String)
			{
				EnumValue = Enum->GetValueByNameString(InputLiteral.StringValue);
				if (EnumValue == INDEX_NONE)
				{
					return MakeTypeError(TEXT("Enum literal not found"), Position);
				}
			}
			else
			{
				return MakeTypeError(TEXT("Expected enum literal"), Position);
			}
			OutLiteral.Type = ETurboStructLiteQueryLiteralType::Integer;
			OutLiteral.IntValue = EnumValue;
			return true;
		}
		if (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
		{
			if (const UEnum* Enum = ByteProp->Enum)
			{
				int64 EnumValue = 0;
				if (InputLiteral.Type == ETurboStructLiteQueryLiteralType::Integer)
				{
					EnumValue = InputLiteral.IntValue;
				}
				else if (InputLiteral.Type == ETurboStructLiteQueryLiteralType::String)
				{
					EnumValue = Enum->GetValueByNameString(InputLiteral.StringValue);
					if (EnumValue == INDEX_NONE)
					{
						return MakeTypeError(TEXT("Enum literal not found"), Position);
					}
				}
				else
				{
					return MakeTypeError(TEXT("Expected enum literal"), Position);
				}
				OutLiteral.Type = ETurboStructLiteQueryLiteralType::Integer;
				OutLiteral.IntValue = EnumValue;
				return true;
			}
			if (InputLiteral.Type == ETurboStructLiteQueryLiteralType::Integer)
			{
				OutLiteral = InputLiteral;
				return true;
			}
			if (InputLiteral.Type == ETurboStructLiteQueryLiteralType::Float)
			{
				OutLiteral.Type = ETurboStructLiteQueryLiteralType::Integer;
				OutLiteral.IntValue = static_cast<int64>(InputLiteral.FloatValue);
				return true;
			}
			return MakeTypeError(TEXT("Expected numeric literal"), Position);
		}
		if (CastField<FStrProperty>(Prop))
		{
			if (InputLiteral.Type != ETurboStructLiteQueryLiteralType::String)
			{
				return MakeTypeError(TEXT("Expected string literal"), Position);
			}
			OutLiteral = InputLiteral;
			return true;
		}
		if (CastField<FNameProperty>(Prop))
		{
			if (InputLiteral.Type != ETurboStructLiteQueryLiteralType::String)
			{
				return MakeTypeError(TEXT("Expected name literal"), Position);
			}
			OutLiteral.Type = ETurboStructLiteQueryLiteralType::Name;
			OutLiteral.NameValue = FName(*InputLiteral.StringValue);
			return true;
		}
		return MakeTypeError(TEXT("Unsupported property type"), Position);
	};

	TFunction<bool(const TSharedPtr<FTurboStructLiteQueryNode>&)> BindNode;
	BindNode = [&](const TSharedPtr<FTurboStructLiteQueryNode>& Node) -> bool
	{
		if (!Node.IsValid())
		{
			return false;
		}
		if (Node->Type == ETurboStructLiteQueryNodeType::Comparison)
		{
			if (!ResolvePath(Node->Comparison))
			{
				return false;
			}
			FProperty* Leaf = Node->Comparison.Lhs.LeafProperty;
			if (!Leaf)
			{
				return MakeError(TEXT("Invalid property binding"), Node->Comparison.Lhs.PathPosition);
			}

			const bool bIsContains = Node->Comparison.Op == ETurboStructLiteQueryCompareOp::Contains;
			if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Leaf))
			{
				if (!bIsContains)
				{
					return MakeTypeError(TEXT("Operator not valid for array"), Node->Comparison.OperatorPosition);
				}
				Node->Comparison.Lhs.bIsContainer = true;
				Node->Comparison.Lhs.ContainerElementProperty = ArrayProp->Inner;
			}
			else if (FSetProperty* SetProp = CastField<FSetProperty>(Leaf))
			{
				if (!bIsContains)
				{
					return MakeTypeError(TEXT("Operator not valid for set"), Node->Comparison.OperatorPosition);
				}
				Node->Comparison.Lhs.bIsContainer = true;
				Node->Comparison.Lhs.ContainerElementProperty = SetProp->ElementProp;
			}
			else if (FMapProperty* MapProp = CastField<FMapProperty>(Leaf))
			{
				if (!bIsContains)
				{
					return MakeTypeError(TEXT("Operator not valid for map"), Node->Comparison.OperatorPosition);
				}
				Node->Comparison.Lhs.bIsContainer = true;
				Node->Comparison.Lhs.ContainerElementProperty = MapProp->ValueProp;
			}
			else if (bIsContains && !CastField<FStrProperty>(Leaf))
			{
				return MakeTypeError(TEXT("CONTAINS is only valid for String/Array/Set/Map"), Node->Comparison.OperatorPosition);
			}

			FTurboStructLiteQueryLiteral ConvertedLiteral;
			if (Node->Comparison.Lhs.bIsContainer)
			{
				if (!ConvertLiteralForProperty(Node->Comparison.Lhs.ContainerElementProperty, Node->Comparison.Rhs, ConvertedLiteral, Node->Comparison.LiteralPosition, true))
				{
					return false;
				}
			}
			else
			{
				const bool bAllowNumeric = Node->Comparison.Op != ETurboStructLiteQueryCompareOp::Contains;
				if (!ConvertLiteralForProperty(Leaf, Node->Comparison.Rhs, ConvertedLiteral, Node->Comparison.LiteralPosition, bAllowNumeric))
				{
					return false;
				}
				if (Node->Comparison.Op == ETurboStructLiteQueryCompareOp::Greater || Node->Comparison.Op == ETurboStructLiteQueryCompareOp::Less ||
					Node->Comparison.Op == ETurboStructLiteQueryCompareOp::GreaterEqual || Node->Comparison.Op == ETurboStructLiteQueryCompareOp::LessEqual)
				{
					if (!CastField<FNumericProperty>(Leaf))
					{
						return MakeTypeError(TEXT("Relational operator not valid for this type"), Node->Comparison.OperatorPosition);
					}
				}
			}
			Node->Comparison.Rhs = MoveTemp(ConvertedLiteral);
			return true;
		}
		if (Node->Left.IsValid())
		{
			if (!BindNode(Node->Left))
			{
				return false;
			}
		}
		if (Node->Right.IsValid())
		{
			if (!BindNode(Node->Right))
			{
				return false;
			}
		}
		return true;
	};

	return BindNode(Root);
}

void UTurboStructLiteQueryLibrary::CollectQueryBoundProperties(const TSharedPtr<FTurboStructLiteQueryNode>& Root, TArray<const FTurboStructLiteQueryBoundProperty*>& OutProperties)
{
	if (!Root.IsValid())
	{
		return;
	}
	if (Root->Type == ETurboStructLiteQueryNodeType::Comparison)
	{
		OutProperties.Add(&Root->Comparison.Lhs);
	}
	if (Root->Left.IsValid())
	{
		CollectQueryBoundProperties(Root->Left, OutProperties);
	}
	if (Root->Right.IsValid())
	{
		CollectQueryBoundProperties(Root->Right, OutProperties);
	}
}

bool UTurboStructLiteQueryLibrary::BuildSelectFieldInfos(const TArray<FString>& SelectFields, const UStruct* RootStruct, TArray<FTurboStructLiteSelectFieldInfo>& OutFields, FString& OutErrorMessage)
{
	OutFields.Reset();
	OutErrorMessage.Reset();

	if (!RootStruct)
	{
		OutErrorMessage = TEXT("Type Error: Invalid struct context");
		return false;
	}

	TSet<FName> SeenPaths;

	for (const FString& RawField : SelectFields)
	{
		FString FieldPath = RawField;
		FieldPath.TrimStartAndEndInline();
		if (FieldPath.IsEmpty())
		{
			continue;
		}
		TArray<FString> Segments;
		FieldPath.ParseIntoArray(Segments, TEXT("."), true);
		if (Segments.Num() == 0)
		{
			OutErrorMessage = TEXT("Type Error: Empty select field");
			return false;
		}

		bool bCountOnly = false;
		if (Segments.Num() > 1 && Segments.Last().Equals(TEXT("Num"), ESearchCase::IgnoreCase))
		{
			bCountOnly = true;
			Segments.Pop();
		}

		UStruct* CurrentStruct = const_cast<UStruct*>(RootStruct);
		TArray<FProperty*> PropertyChain;

		auto FindPropertyByName = [&](UStruct* Struct, const FString& Segment) -> FProperty*
		{
			if (!Struct)
			{
				return nullptr;
			}
			FProperty* Found = FindFProperty<FProperty>(Struct, *Segment);
			if (Found)
			{
				return Found;
			}
			FProperty* Match = nullptr;
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				FProperty* Prop = *It;
				const FString DisplayName = Prop->GetDisplayNameText().ToString();
				if (DisplayName.Equals(Segment, ESearchCase::IgnoreCase))
				{
					if (Match)
					{
						return nullptr;
					}
					Match = Prop;
				}
			}
			return Match;
		};

		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			FProperty* Found = FindPropertyByName(CurrentStruct, Segments[Index]);
			if (!Found)
			{
				OutErrorMessage = FString::Printf(TEXT("Type Error: Property '%s' not found"), *Segments[Index]);
				return false;
			}
			PropertyChain.Add(Found);
			if (Index < Segments.Num() - 1)
			{
				FStructProperty* NextStruct = CastField<FStructProperty>(Found);
				if (!NextStruct)
				{
					OutErrorMessage = FString::Printf(TEXT("Type Error: Property '%s' is not a struct"), *Segments[Index]);
					return false;
				}
				CurrentStruct = NextStruct->Struct;
			}
		}

		FProperty* LeafProperty = PropertyChain.Num() > 0 ? PropertyChain.Last() : nullptr;
		if (!LeafProperty)
		{
			OutErrorMessage = TEXT("Type Error: Invalid select field");
			return false;
		}
		if (bCountOnly && !CastField<FArrayProperty>(LeafProperty))
		{
			OutErrorMessage = TEXT("Type Error: .Num is only valid for arrays");
			return false;
		}

		FString PathKeyString;
		for (int32 Index = 0; Index < PropertyChain.Num(); ++Index)
		{
			if (Index > 0)
			{
				PathKeyString += TEXT(".");
			}
			PathKeyString += PropertyChain[Index]->GetName();
		}
		if (bCountOnly)
		{
			PathKeyString += TEXT(".Num");
		}
		const FName PathKey = FName(*PathKeyString);
		if (SeenPaths.Contains(PathKey))
		{
			continue;
		}
		SeenPaths.Add(PathKey);

		FTurboStructLiteSelectFieldInfo Info;
		Info.ColumnName = FName(*FieldPath);
		Info.PathKey = PathKey;
		Info.PropertyChain = MoveTemp(PropertyChain);
		Info.LeafProperty = LeafProperty;
		Info.bCountOnly = bCountOnly;
		OutFields.Add(MoveTemp(Info));
	}

	return true;
}




