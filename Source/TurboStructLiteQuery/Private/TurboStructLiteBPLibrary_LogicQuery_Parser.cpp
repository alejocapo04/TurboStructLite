#include "TurboStructLiteQueryLibrary.h"
#include "TurboStructLiteBPLibrary.h"
#include "TurboStructLite.h"
#include "TurboStructLiteConstantsQuery.h"
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
#include "Templates/UnrealTemplate.h"
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
#include "UObject/UObjectIterator.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/Compression.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/Guid.h"
#include "TurboStructLiteDebugMacros.h"

bool UTurboStructLiteQueryLibrary::ResolveLogicSubSlot(const FString& SlotName, int32& OutSubSlotIndex, FString& OutErrorMessage)
{
	OutSubSlotIndex = 0;
	OutErrorMessage.Reset();
	if (SlotName.IsEmpty())
	{
		OutErrorMessage = TEXT("IO Error: SlotName is empty");
		return false;
	}
	TArray<int32> SubSlots;
	if (!UTurboStructLiteBPLibrary::ListSubSlotIndices(SlotName, SubSlots) || SubSlots.Num() == 0)
	{
		OutErrorMessage = TEXT("IO Error: Slot not found or empty");
		return false;
	}
	SubSlots.Sort();
	OutSubSlotIndex = SubSlots[0];
	return true;
}

UStruct* UTurboStructLiteQueryLibrary::FindStructByTypeName(const FString& TypeName)
{
	const FString NormalTarget = UTurboStructLiteBPLibrary::NormalizeTypeName(TypeName);
	if (NormalTarget.IsEmpty())
	{
		return nullptr;
	}
	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		UScriptStruct* Struct = *It;
		if (!Struct)
		{
			continue;
		}
		const FString NormalCpp = UTurboStructLiteBPLibrary::NormalizeTypeName(Struct->GetStructCPPName());
		if (NormalCpp == NormalTarget)
		{
			return Struct;
		}
		const FString NormalName = UTurboStructLiteBPLibrary::NormalizeTypeName(Struct->GetName());
		if (NormalName == NormalTarget)
		{
			return Struct;
		}
	}
	return nullptr;
}

bool UTurboStructLiteQueryLibrary::ResolveContextStructFromSlot(const FString& SlotName, int32 SubSlotIndex, const FString& EncryptionKey, ETurboStructLiteEncryption SelectedEncryption, UStruct*& OutStruct, FString& OutErrorMessage)
{
	OutStruct = nullptr;
	OutErrorMessage.Reset();
	int32 UseSubSlot = SubSlotIndex;
	if (UseSubSlot == -1)
	{
		if (!ResolveLogicSubSlot(SlotName, UseSubSlot, OutErrorMessage))
		{
			return false;
		}
	}
	TArray<uint8> RawBytes;
	if (!UTurboStructLiteBPLibrary::LoadEntry(SlotName, UseSubSlot, EncryptionKey, SelectedEncryption, RawBytes, false, FString()))
	{
		OutErrorMessage = TEXT("IO Error: Load failed");
		return false;
	}
	FString RootType;
	if (!UTurboStructLiteBPLibrary::GetRootMetaTypeFromBytes(RawBytes, RootType))
	{
		OutErrorMessage = TEXT("IO Error: Missing metadata type");
		return false;
	}
	UStruct* Struct = FindStructByTypeName(RootType);
	if (!Struct)
	{
		OutErrorMessage = FString::Printf(TEXT("Type Error: Struct '%s' not found"), *RootType);
		return false;
	}
	OutStruct = Struct;
	return true;
}

bool UTurboStructLiteQueryLibrary::ParseSelectQueryString(const FString& InQueryString, const TArray<FString>& InSelectFields, FString& OutQueryString, TArray<FString>& OutSelectFields, int32& OutLimit, int32& OutOffset, FString& OutOrderByField, bool& bOutOrderDescending, TArray<ETurboStructLiteAggregateOp>& OutAggregateOps, TArray<FString>& OutAggregateFields, TArray<FName>& OutAggregateColumns, FString& OutErrorMessage)
{
	OutErrorMessage.Reset();
	OutQueryString = InQueryString;
	OutSelectFields = InSelectFields;
	OutLimit = 0;
	OutOffset = 0;
	OutOrderByField.Reset();
	bOutOrderDescending = false;
	OutAggregateOps.Reset();
	OutAggregateFields.Reset();
	OutAggregateColumns.Reset();

	const int32 Length = InQueryString.Len();
	int32 StartIndex = 0;
	while (StartIndex < Length && FChar::IsWhitespace(InQueryString[StartIndex]))
	{
		StartIndex++;
	}
	if (StartIndex >= Length)
	{
		return true;
	}

	TArray<FTurboStructLiteQueryToken> Tokens;
	int32 TokenErrorPos = 0;
	if (!TokenizeLogicQuery(InQueryString, Tokens, OutErrorMessage, TokenErrorPos, true))
	{
		return false;
	}

	int32 Index = 0;
	while (Index < Tokens.Num() && Tokens[Index].Type == ETurboStructLiteQueryTokenType::EndOfInput)
	{
		Index++;
	}
	if (Index >= Tokens.Num())
	{
		return true;
	}

	auto GetTokenPos = [&](int32 TokenIndex) -> int32
	{
		if (!Tokens.IsValidIndex(TokenIndex))
		{
			return Length;
		}
		const int32 Pos = Tokens[TokenIndex].Position - 1;
		return FMath::Clamp(Pos, 0, Length);
	};

	auto IsClauseStartAt = [&](int32 TokenIndex, ETurboStructLiteQueryTokenType& OutClauseType) -> bool
	{
		if (!Tokens.IsValidIndex(TokenIndex))
		{
			return false;
		}
		const ETurboStructLiteQueryTokenType Type = Tokens[TokenIndex].Type;
		if (Type == ETurboStructLiteQueryTokenType::EndOfInput)
		{
			return false;
		}
		if (TokenIndex > 0 && Tokens[TokenIndex - 1].Type == ETurboStructLiteQueryTokenType::Dot)
		{
			return false;
		}
		if (Type == ETurboStructLiteQueryTokenType::Order)
		{
			const int32 NextIndex = TokenIndex + 1;
			if (Tokens.IsValidIndex(NextIndex) && Tokens[NextIndex].Type == ETurboStructLiteQueryTokenType::By)
			{
				OutClauseType = Type;
				return true;
			}
			return false;
		}
		if (Type == ETurboStructLiteQueryTokenType::Limit || Type == ETurboStructLiteQueryTokenType::Offset)
		{
			const int32 NextIndex = TokenIndex + 1;
			if (Tokens.IsValidIndex(NextIndex) && Tokens[NextIndex].Type == ETurboStructLiteQueryTokenType::Number && !Tokens[NextIndex].Text.StartsWith(TEXT("-")))
			{
				OutClauseType = Type;
				return true;
			}
			return false;
		}
		if (Type == ETurboStructLiteQueryTokenType::Where || Type == ETurboStructLiteQueryTokenType::From)
		{
			OutClauseType = Type;
			return true;
		}
		return false;
	};

	auto FindNextClause = [&](int32 SearchStart, bool bAllowWhere, bool bAllowFrom, bool bAllowOrder, bool bAllowLimit, bool bAllowOffset, int32& OutIndex, ETurboStructLiteQueryTokenType& OutClauseType) -> bool
	{
		OutIndex = INDEX_NONE;
		OutClauseType = ETurboStructLiteQueryTokenType::EndOfInput;
		int32 Depth = 0;
		for (int32 ScanIndex = SearchStart; ScanIndex < Tokens.Num(); ++ScanIndex)
		{
			const ETurboStructLiteQueryTokenType Type = Tokens[ScanIndex].Type;
			if (Type == ETurboStructLiteQueryTokenType::EndOfInput)
			{
				break;
			}
			if (Type == ETurboStructLiteQueryTokenType::LeftParen)
			{
				Depth++;
				continue;
			}
			if (Type == ETurboStructLiteQueryTokenType::RightParen)
			{
				if (Depth > 0)
				{
					Depth--;
				}
				continue;
			}
			if (Depth != 0)
			{
				continue;
			}
			ETurboStructLiteQueryTokenType ClauseType = ETurboStructLiteQueryTokenType::EndOfInput;
			if (!IsClauseStartAt(ScanIndex, ClauseType))
			{
				continue;
			}
			if ((ClauseType == ETurboStructLiteQueryTokenType::Where && bAllowWhere) ||
				(ClauseType == ETurboStructLiteQueryTokenType::From && bAllowFrom) ||
				(ClauseType == ETurboStructLiteQueryTokenType::Order && bAllowOrder) ||
				(ClauseType == ETurboStructLiteQueryTokenType::Limit && bAllowLimit) ||
				(ClauseType == ETurboStructLiteQueryTokenType::Offset && bAllowOffset))
			{
				OutIndex = ScanIndex;
				OutClauseType = ClauseType;
				return true;
			}
		}
		if (Depth != 0)
		{
			OutErrorMessage = TEXT("Query Error: Unterminated parenthesis");
			return false;
		}
		return true;
	};

	auto ParseUnsignedInt = [&](const FString& Text, int32& OutValue) -> bool
	{
		FString Trimmed = Text;
		Trimmed.TrimStartAndEndInline();
		if (Trimmed.IsEmpty())
		{
			return false;
		}
		for (int32 Index = 0; Index < Trimmed.Len(); ++Index)
		{
			if (!FChar::IsDigit(Trimmed[Index]))
			{
				return false;
			}
		}
		OutValue = FCString::Atoi(*Trimmed);
		return true;
	};

	auto NormalizeIdentifier = [&](FString Text) -> FString
	{
		Text.TrimStartAndEndInline();
		if (Text.Len() >= 2)
		{
			const TCHAR First = Text[0];
			const TCHAR Last = Text[Text.Len() - 1];
			if ((First == '"' && Last == '"') || (First == '\'' && Last == '\''))
			{
				Text = Text.Mid(1, Text.Len() - 2);
				Text.ReplaceInline(TEXT("\\\""), TEXT("\""));
				Text.ReplaceInline(TEXT("\\'"), TEXT("'"));
				Text.ReplaceInline(TEXT("\\\\"), TEXT("\\"));
			}
		}
		return Text;
	};

	auto ParseOrderBy = [&](const FString& Text) -> bool
	{
		FString Trimmed = Text;
		Trimmed.TrimStartAndEndInline();
		if (Trimmed.IsEmpty())
		{
			OutErrorMessage = TEXT("Query Error: ORDER BY requires a field");
			return false;
		}
		FString FieldToken;
		FString Tail;
		const TCHAR FirstChar = Trimmed[0];
		if (FirstChar == '"' || FirstChar == '\'')
		{
			const TCHAR Quote = FirstChar;
			bool bEscape = false;
			int32 EndIndex = INDEX_NONE;
			for (int32 CharIndex = 1; CharIndex < Trimmed.Len(); ++CharIndex)
			{
				const TCHAR Current = Trimmed[CharIndex];
				if (bEscape)
				{
					bEscape = false;
					continue;
				}
				if (Current == '\\')
				{
					bEscape = true;
					continue;
				}
				if (Current == Quote)
				{
					EndIndex = CharIndex;
					break;
				}
			}
			if (EndIndex == INDEX_NONE)
			{
				OutErrorMessage = TEXT("Query Error: ORDER BY requires a field");
				return false;
			}
			FieldToken = Trimmed.Left(EndIndex + 1);
			Tail = Trimmed.Mid(EndIndex + 1);
		}
		else
		{
			int32 SpaceIndex = INDEX_NONE;
			for (int32 CharIndex = 0; CharIndex < Trimmed.Len(); ++CharIndex)
			{
				if (FChar::IsWhitespace(Trimmed[CharIndex]))
				{
					SpaceIndex = CharIndex;
					break;
				}
			}
			if (SpaceIndex == INDEX_NONE)
			{
				FieldToken = Trimmed;
			}
			else
			{
				FieldToken = Trimmed.Left(SpaceIndex);
				Tail = Trimmed.Mid(SpaceIndex);
			}
		}
		OutOrderByField = NormalizeIdentifier(FieldToken);
		if (OutOrderByField.IsEmpty())
		{
			OutErrorMessage = TEXT("Query Error: ORDER BY has invalid syntax");
			return false;
		}
		Tail.TrimStartAndEndInline();
		if (!Tail.IsEmpty())
		{
			const FString Dir = Tail.ToUpper();
			if (Dir == TEXT("DESC"))
			{
				bOutOrderDescending = true;
			}
			else if (Dir == TEXT("ASC"))
			{
				bOutOrderDescending = false;
			}
			else
			{
				OutErrorMessage = TEXT("Query Error: ORDER BY direction must be ASC or DESC");
				return false;
			}
		}
		return true;
	};

	auto ParseAggregateToken = [&](const FString& Field, ETurboStructLiteAggregateOp& OutOp, FString& OutInner) -> bool
	{
		int32 ParenIndex = Field.Find(TEXT("("));
		if (ParenIndex == INDEX_NONE || !Field.EndsWith(TEXT(")")))
		{
			return false;
		}
		FString FuncName = Field.Left(ParenIndex);
		FuncName.TrimStartAndEndInline();
		if (FuncName.IsEmpty())
		{
			return false;
		}
		FString UpperFunc = FuncName.ToUpper();
		OutInner = Field.Mid(ParenIndex + 1, Field.Len() - ParenIndex - 2);
		OutInner.TrimStartAndEndInline();
		if (UpperFunc == TEXT("COUNT"))
		{
			OutOp = ETurboStructLiteAggregateOp::Count;
			return true;
		}
		if (UpperFunc == TEXT("SUM"))
		{
			OutOp = ETurboStructLiteAggregateOp::Sum;
			return true;
		}
		if (UpperFunc == TEXT("AVG"))
		{
			OutOp = ETurboStructLiteAggregateOp::Avg;
			return true;
		}
		return false;
	};

	auto ParseFieldsList = [&](const FString& FieldsPart) -> bool
	{
		TArray<FString> RawFields;
		FieldsPart.ParseIntoArray(RawFields, TEXT(","), false);
		bool bSelectAll = false;
		bool bHasPlain = false;
		bool bHasAggregate = false;
		OutSelectFields.Reset();
		OutAggregateOps.Reset();
		OutAggregateFields.Reset();
		OutAggregateColumns.Reset();

		for (FString Field : RawFields)
		{
			Field = NormalizeIdentifier(Field);
			if (Field.IsEmpty())
			{
				OutErrorMessage = TEXT("Query Error: SELECT has empty field name");
				return false;
			}
			if (Field == TEXT("*"))
			{
				bSelectAll = true;
				continue;
			}
			ETurboStructLiteAggregateOp Op = ETurboStructLiteAggregateOp::Count;
			FString Inner;
			if (ParseAggregateToken(Field, Op, Inner))
			{
				bHasAggregate = true;
				if (Op == ETurboStructLiteAggregateOp::Count)
				{
					if (!Inner.IsEmpty() && Inner != TEXT("*"))
					{
						OutErrorMessage = TEXT("Query Error: COUNT only supports *");
						return false;
					}
					OutAggregateOps.Add(Op);
					OutAggregateFields.Add(TEXT(""));
					OutAggregateColumns.Add(FName(TEXT("COUNT")));
				}
				else
				{
					if (Inner.IsEmpty())
					{
						OutErrorMessage = TEXT("Query Error: Aggregate field is required");
						return false;
					}
					OutAggregateOps.Add(Op);
					OutAggregateFields.Add(Inner);
					const TCHAR* OpName = Op == ETurboStructLiteAggregateOp::Sum ? TEXT("SUM") : TEXT("AVG");
					const FString Column = FString::Printf(TEXT("%s(%s)"), OpName, *Inner);
					OutAggregateColumns.Add(FName(*Column));
				}
				continue;
			}
			bHasPlain = true;
			OutSelectFields.AddUnique(Field);
		}

		if (bSelectAll && bHasAggregate)
		{
			OutErrorMessage = TEXT("Query Error: SELECT * cannot be combined with aggregates");
			return false;
		}
		if (bHasAggregate && bHasPlain)
		{
			OutErrorMessage = TEXT("Query Error: Aggregates cannot be mixed with fields");
			return false;
		}
		if (bSelectAll)
		{
			OutSelectFields.Reset();
		}
		if (!bHasAggregate && !bSelectAll && OutSelectFields.Num() == 0)
		{
			OutErrorMessage = TEXT("Query Error: SELECT requires a field list");
			return false;
		}
		return true;
	};

	auto ParseWhereClause = [&](int32 WhereTokenIndex, int32& OutNextIndex) -> bool
	{
		const int32 WhereStartIndex = WhereTokenIndex + 1;
		if (!Tokens.IsValidIndex(WhereStartIndex) || Tokens[WhereStartIndex].Type == ETurboStructLiteQueryTokenType::EndOfInput)
		{
			OutErrorMessage = TEXT("Query Error: WHERE clause is empty");
			return false;
		}
		int32 ClauseIndex = INDEX_NONE;
		ETurboStructLiteQueryTokenType ClauseType = ETurboStructLiteQueryTokenType::EndOfInput;
		if (!FindNextClause(WhereStartIndex, false, false, true, true, true, ClauseIndex, ClauseType))
		{
			return false;
		}
		const int32 StartPos = GetTokenPos(WhereStartIndex);
		const int32 EndPos = ClauseIndex == INDEX_NONE ? Length : GetTokenPos(ClauseIndex);
		OutQueryString = InQueryString.Mid(StartPos, EndPos - StartPos);
		OutQueryString.TrimStartAndEndInline();
		if (OutQueryString.IsEmpty())
		{
			OutErrorMessage = TEXT("Query Error: WHERE clause is empty");
			return false;
		}
		OutNextIndex = ClauseIndex == INDEX_NONE ? Tokens.Num() : ClauseIndex;
		return true;
	};

	auto ParseTailClauses = [&](int32 TailStartIndex) -> bool
	{
		bool bHasOrder = false;
		bool bHasLimit = false;
		bool bHasOffset = false;
		int32 LocalIndex = TailStartIndex;
		while (LocalIndex < Tokens.Num())
		{
			const ETurboStructLiteQueryTokenType Type = Tokens[LocalIndex].Type;
			if (Type == ETurboStructLiteQueryTokenType::EndOfInput)
			{
				return true;
			}
			if (Type == ETurboStructLiteQueryTokenType::Order)
			{
				if (bHasOrder)
				{
					OutErrorMessage = TEXT("Query Error: ORDER BY has invalid syntax");
					return false;
				}
				if (!Tokens.IsValidIndex(LocalIndex + 1) || Tokens[LocalIndex + 1].Type != ETurboStructLiteQueryTokenType::By)
				{
					OutErrorMessage = TEXT("Query Error: ORDER must be followed by BY");
					return false;
				}
				const int32 OrderExprStartIndex = LocalIndex + 2;
				int32 ClauseIndex = INDEX_NONE;
				ETurboStructLiteQueryTokenType ClauseType = ETurboStructLiteQueryTokenType::EndOfInput;
				if (!FindNextClause(OrderExprStartIndex, false, false, false, true, true, ClauseIndex, ClauseType))
				{
					return false;
				}
				const int32 StartPos = GetTokenPos(OrderExprStartIndex);
				const int32 EndPos = ClauseIndex == INDEX_NONE ? Length : GetTokenPos(ClauseIndex);
				FString OrderExpr = InQueryString.Mid(StartPos, EndPos - StartPos);
				OrderExpr.TrimStartAndEndInline();
				if (!ParseOrderBy(OrderExpr))
				{
					return false;
				}
				bHasOrder = true;
				if (ClauseIndex == INDEX_NONE)
				{
					return true;
				}
				LocalIndex = ClauseIndex;
				continue;
			}
			if (Type == ETurboStructLiteQueryTokenType::Limit)
			{
				if (bHasLimit)
				{
					OutErrorMessage = TEXT("Query Error: LIMIT has invalid syntax");
					return false;
				}
				const int32 ValueIndex = LocalIndex + 1;
				if (!Tokens.IsValidIndex(ValueIndex) || Tokens[ValueIndex].Type != ETurboStructLiteQueryTokenType::Number || Tokens[ValueIndex].Text.StartsWith(TEXT("-")) || !ParseUnsignedInt(Tokens[ValueIndex].Text, OutLimit))
				{
					OutErrorMessage = TEXT("Query Error: LIMIT requires a non-negative integer");
					return false;
				}
				bHasLimit = true;
				LocalIndex = ValueIndex + 1;
				continue;
			}
			if (Type == ETurboStructLiteQueryTokenType::Offset)
			{
				if (bHasOffset)
				{
					OutErrorMessage = TEXT("Query Error: OFFSET has invalid syntax");
					return false;
				}
				const int32 ValueIndex = LocalIndex + 1;
				if (!Tokens.IsValidIndex(ValueIndex) || Tokens[ValueIndex].Type != ETurboStructLiteQueryTokenType::Number || Tokens[ValueIndex].Text.StartsWith(TEXT("-")) || !ParseUnsignedInt(Tokens[ValueIndex].Text, OutOffset))
				{
					OutErrorMessage = TEXT("Query Error: OFFSET requires a non-negative integer");
					return false;
				}
				bHasOffset = true;
				LocalIndex = ValueIndex + 1;
				continue;
			}
			OutErrorMessage = TEXT("Query Error: Unexpected text after clause");
			return false;
		}
		return true;
	};

	bool bWhereMode = false;
	if (Tokens[Index].Type == ETurboStructLiteQueryTokenType::Select)
	{
		Index++;
	}
	else if (Tokens[Index].Type == ETurboStructLiteQueryTokenType::Where)
	{
		bWhereMode = true;
		Index++;
	}
	else
	{
		return true;
	}

	if (bWhereMode)
	{
		int32 NextIndex = INDEX_NONE;
		if (!ParseWhereClause(Index - 1, NextIndex))
		{
			return false;
		}
		if (NextIndex == INDEX_NONE || NextIndex >= Tokens.Num())
		{
			return true;
		}
		return ParseTailClauses(NextIndex);
	}

	const int32 FieldsStartIndex = Index;
	if (!Tokens.IsValidIndex(FieldsStartIndex) || Tokens[FieldsStartIndex].Type == ETurboStructLiteQueryTokenType::EndOfInput)
	{
		OutErrorMessage = TEXT("Query Error: SELECT requires a field list");
		return false;
	}
	int32 ClauseIndex = INDEX_NONE;
	ETurboStructLiteQueryTokenType ClauseType = ETurboStructLiteQueryTokenType::EndOfInput;
	if (!FindNextClause(FieldsStartIndex, true, true, true, true, true, ClauseIndex, ClauseType))
	{
		return false;
	}
	const int32 FieldsStartPos = GetTokenPos(FieldsStartIndex);
	const int32 FieldsEndPos = ClauseIndex == INDEX_NONE ? Length : GetTokenPos(ClauseIndex);
	FString FieldsPart = InQueryString.Mid(FieldsStartPos, FieldsEndPos - FieldsStartPos);
	FieldsPart.TrimStartAndEndInline();
	if (FieldsPart.IsEmpty())
	{
		OutErrorMessage = TEXT("Query Error: SELECT requires a field list");
		return false;
	}
	if (!ParseFieldsList(FieldsPart))
	{
		return false;
	}

	if (ClauseIndex == INDEX_NONE)
	{
		OutQueryString = TEXT("true");
		return true;
	}

	Index = ClauseIndex;
	if (Tokens.IsValidIndex(Index) && Tokens[Index].Type == ETurboStructLiteQueryTokenType::From)
	{
		const int32 NameStartIndex = Index + 1;
		if (!Tokens.IsValidIndex(NameStartIndex) || Tokens[NameStartIndex].Type == ETurboStructLiteQueryTokenType::EndOfInput)
		{
			OutErrorMessage = TEXT("Query Error: FROM requires a name");
			return false;
		}
		int32 NameClauseIndex = INDEX_NONE;
		ETurboStructLiteQueryTokenType NameClauseType = ETurboStructLiteQueryTokenType::EndOfInput;
		if (!FindNextClause(NameStartIndex, true, false, true, true, true, NameClauseIndex, NameClauseType))
		{
			return false;
		}
		const int32 NameStartPos = GetTokenPos(NameStartIndex);
		const int32 NameEndPos = NameClauseIndex == INDEX_NONE ? Length : GetTokenPos(NameClauseIndex);
		FString TableName = NormalizeIdentifier(InQueryString.Mid(NameStartPos, NameEndPos - NameStartPos));
		if (TableName.IsEmpty())
		{
			OutErrorMessage = TEXT("Query Error: FROM requires a name");
			return false;
		}
		Index = NameClauseIndex == INDEX_NONE ? Tokens.Num() : NameClauseIndex;
	}

	if (Tokens.IsValidIndex(Index) && Tokens[Index].Type == ETurboStructLiteQueryTokenType::Where)
	{
		int32 NextIndex = INDEX_NONE;
		if (!ParseWhereClause(Index, NextIndex))
		{
			return false;
		}
		if (NextIndex == INDEX_NONE || NextIndex >= Tokens.Num())
		{
			return true;
		}
		return ParseTailClauses(NextIndex);
	}

	OutQueryString = TEXT("true");
	if (Index >= Tokens.Num())
	{
		return true;
	}
	return ParseTailClauses(Index);
}

bool UTurboStructLiteQueryLibrary::TokenizeLogicQuery(const FString& QueryString, TArray<FTurboStructLiteQueryToken>& OutTokens, FString& OutErrorMessage, int32& OutErrorPos, bool bAllowSqlKeywords)
{
	OutTokens.Reset();
	OutErrorMessage.Reset();
	OutErrorPos = 0;
	const int32 Length = QueryString.Len();
	int32 Index = 0;

	auto AddToken = [&](ETurboStructLiteQueryTokenType Type, const FString& Text, int32 Position)
	{
		FTurboStructLiteQueryToken Token;
		Token.Type = Type;
		Token.Text = Text;
		Token.Position = Position;
		OutTokens.Add(Token);
	};

	while (Index < Length)
	{
		const TCHAR Ch = QueryString[Index];
		if (FChar::IsWhitespace(Ch))
		{
			Index++;
			continue;
		}
		const int32 Position = Index + 1;
		if (Ch == '(')
		{
			AddToken(ETurboStructLiteQueryTokenType::LeftParen, TEXT("("), Position);
			Index++;
			continue;
		}
		if (Ch == ')')
		{
			AddToken(ETurboStructLiteQueryTokenType::RightParen, TEXT(")"), Position);
			Index++;
			continue;
		}
		if (bAllowSqlKeywords && Ch == ',')
		{
			AddToken(ETurboStructLiteQueryTokenType::Comma, TEXT(","), Position);
			Index++;
			continue;
		}
		if (bAllowSqlKeywords && Ch == '*')
		{
			AddToken(ETurboStructLiteQueryTokenType::Asterisk, TEXT("*"), Position);
			Index++;
			continue;
		}
		if (Ch == '.')
		{
			AddToken(ETurboStructLiteQueryTokenType::Dot, TEXT("."), Position);
			Index++;
			continue;
		}
		if (Ch == '&' && Index + 1 < Length && QueryString[Index + 1] == '&')
		{
			AddToken(ETurboStructLiteQueryTokenType::And, TEXT("&&"), Position);
			Index += 2;
			continue;
		}
		if (Ch == '|' && Index + 1 < Length && QueryString[Index + 1] == '|')
		{
			AddToken(ETurboStructLiteQueryTokenType::Or, TEXT("||"), Position);
			Index += 2;
			continue;
		}
		if (Ch == '=' && Index + 1 < Length && QueryString[Index + 1] == '=')
		{
			AddToken(ETurboStructLiteQueryTokenType::Equal, TEXT("=="), Position);
			Index += 2;
			continue;
		}
		if (Ch == '!' && Index + 1 < Length && QueryString[Index + 1] == '=')
		{
			AddToken(ETurboStructLiteQueryTokenType::NotEqual, TEXT("!="), Position);
			Index += 2;
			continue;
		}
		if (Ch == '>' && Index + 1 < Length && QueryString[Index + 1] == '=')
		{
			AddToken(ETurboStructLiteQueryTokenType::GreaterEqual, TEXT(">="), Position);
			Index += 2;
			continue;
		}
		if (Ch == '<' && Index + 1 < Length && QueryString[Index + 1] == '=')
		{
			AddToken(ETurboStructLiteQueryTokenType::LessEqual, TEXT("<="), Position);
			Index += 2;
			continue;
		}
		if (Ch == '>')
		{
			AddToken(ETurboStructLiteQueryTokenType::Greater, TEXT(">"), Position);
			Index++;
			continue;
		}
		if (Ch == '<')
		{
			AddToken(ETurboStructLiteQueryTokenType::Less, TEXT("<"), Position);
			Index++;
			continue;
		}
		if (Ch == '!')
		{
			AddToken(ETurboStructLiteQueryTokenType::Not, TEXT("!"), Position);
			Index++;
			continue;
		}
		if (Ch == '\'' || Ch == '"')
		{
			const TCHAR Quote = Ch;
			Index++;
			FString Value;
			bool bClosed = false;
			while (Index < Length)
			{
				const TCHAR Current = QueryString[Index];
				if (Current == '\\' && Index + 1 < Length)
				{
					const TCHAR Next = QueryString[Index + 1];
					Value.AppendChar(Next);
					Index += 2;
					continue;
				}
				if (Current == Quote)
				{
					bClosed = true;
					Index++;
					break;
				}
				Value.AppendChar(Current);
				Index++;
			}
			if (!bClosed)
			{
				OutErrorPos = Position;
				OutErrorMessage = FString::Printf(TEXT("Syntax Error (col=%d): Unterminated string literal"), OutErrorPos);
				return false;
			}
			AddToken(ETurboStructLiteQueryTokenType::String, Value, Position);
			continue;
		}
		if (FChar::IsDigit(Ch) || (Ch == '-' && Index + 1 < Length && FChar::IsDigit(QueryString[Index + 1])))
		{
			const int32 Start = Index;
			bool bHasDot = false;
			if (Ch == '-')
			{
				Index++;
			}
			while (Index < Length)
			{
				const TCHAR Current = QueryString[Index];
				if (Current == '.')
				{
					if (bHasDot)
					{
						break;
					}
					bHasDot = true;
					Index++;
					continue;
				}
				if (!FChar::IsDigit(Current))
				{
					break;
				}
				Index++;
			}
			const FString NumberText = QueryString.Mid(Start, Index - Start);
			AddToken(ETurboStructLiteQueryTokenType::Number, NumberText, Position);
			continue;
		}
		if (FChar::IsAlpha(Ch) || Ch == '_')
		{
			const int32 Start = Index;
			Index++;
			while (Index < Length)
			{
				const TCHAR Current = QueryString[Index];
				if (!FChar::IsAlnum(Current) && Current != '_')
				{
					break;
				}
				Index++;
			}
			const FString Ident = QueryString.Mid(Start, Index - Start);
			const FString UpperIdent = Ident.ToUpper();
			if (UpperIdent == TEXT("AND"))
			{
				AddToken(ETurboStructLiteQueryTokenType::AndAlias, Ident, Position);
				continue;
			}
			if (UpperIdent == TEXT("OR"))
			{
				AddToken(ETurboStructLiteQueryTokenType::OrAlias, Ident, Position);
				continue;
			}
			if (UpperIdent == TEXT("NOT"))
			{
				AddToken(ETurboStructLiteQueryTokenType::NotAlias, Ident, Position);
				continue;
			}
			if (UpperIdent == TEXT("CONTAINS"))
			{
				AddToken(ETurboStructLiteQueryTokenType::Contains, Ident, Position);
				continue;
			}
			if (UpperIdent == TEXT("TRUE") || UpperIdent == TEXT("FALSE"))
			{
				AddToken(ETurboStructLiteQueryTokenType::Boolean, UpperIdent, Position);
				continue;
			}
			if (bAllowSqlKeywords)
			{
				if (UpperIdent == TurboStructLiteQueryKey_Select)
				{
					AddToken(ETurboStructLiteQueryTokenType::Select, Ident, Position);
					continue;
				}
				if (UpperIdent == TurboStructLiteQueryKey_From)
				{
					AddToken(ETurboStructLiteQueryTokenType::From, Ident, Position);
					continue;
				}
				if (UpperIdent == TurboStructLiteQueryKey_Where)
				{
					AddToken(ETurboStructLiteQueryTokenType::Where, Ident, Position);
					continue;
				}
				if (UpperIdent == TurboStructLiteQueryKey_Order)
				{
					AddToken(ETurboStructLiteQueryTokenType::Order, Ident, Position);
					continue;
				}
				if (UpperIdent == TurboStructLiteQueryKey_By)
				{
					AddToken(ETurboStructLiteQueryTokenType::By, Ident, Position);
					continue;
				}
				if (UpperIdent == TurboStructLiteQueryKey_Limit)
				{
					AddToken(ETurboStructLiteQueryTokenType::Limit, Ident, Position);
					continue;
				}
				if (UpperIdent == TurboStructLiteQueryKey_Offset)
				{
					AddToken(ETurboStructLiteQueryTokenType::Offset, Ident, Position);
					continue;
				}
				if (UpperIdent == TurboStructLiteQueryKey_Asc)
				{
					AddToken(ETurboStructLiteQueryTokenType::Asc, Ident, Position);
					continue;
				}
				if (UpperIdent == TurboStructLiteQueryKey_Desc)
				{
					AddToken(ETurboStructLiteQueryTokenType::Desc, Ident, Position);
					continue;
				}
			}
			AddToken(ETurboStructLiteQueryTokenType::Identifier, Ident, Position);
			continue;
		}
		OutErrorPos = Position;
		OutErrorMessage = FString::Printf(TEXT("Syntax Error (col=%d): Unexpected character '%c'"), OutErrorPos, Ch);
		return false;
	}
	AddToken(ETurboStructLiteQueryTokenType::EndOfInput, TEXT(""), Length + 1);
	return true;
}

bool UTurboStructLiteQueryLibrary::ParseLogicQuery(const TArray<FTurboStructLiteQueryToken>& Tokens, TSharedPtr<FTurboStructLiteQueryNode>& OutRoot, FString& OutErrorMessage, int32& OutErrorPos)
{
	OutRoot.Reset();
	OutErrorMessage.Reset();
	OutErrorPos = 0;
	int32 Index = 0;
	int32 MaxDepth = TurboStructLiteQueryDefaultMaxRecursionDepth;
	if (GConfig)
	{
		int32 ConfigDepth = MaxDepth;
		if (GConfig->GetInt(UTurboStructLiteBPLibrary::TurboStructLiteSettingsSection, TEXT("MaxQueryRecursionDepth"), ConfigDepth, GGameIni))
		{
			MaxDepth = ConfigDepth;
		}
	}
	if (MaxDepth < TurboStructLiteQueryMinRecursionDepth)
	{
		MaxDepth = TurboStructLiteQueryMinRecursionDepth;
	}
	int32 CurrentDepth = 0;

	auto Peek = [&]() -> const FTurboStructLiteQueryToken&
	{
		if (Tokens.IsValidIndex(Index))
		{
			return Tokens[Index];
		}
		return Tokens.Last();
	};

	auto Advance = [&]() -> const FTurboStructLiteQueryToken&
	{
		const FTurboStructLiteQueryToken& Token = Peek();
		if (Index < Tokens.Num())
		{
			Index++;
		}
		return Token;
	};

	auto Match = [&](ETurboStructLiteQueryTokenType Type) -> bool
	{
		if (Peek().Type == Type)
		{
			Advance();
			return true;
		}
		return false;
	};

	TFunction<TSharedPtr<FTurboStructLiteQueryNode>()> ParseExpression;
	TFunction<TSharedPtr<FTurboStructLiteQueryNode>()> ParseOr;
	TFunction<TSharedPtr<FTurboStructLiteQueryNode>()> ParseAnd;
	TFunction<TSharedPtr<FTurboStructLiteQueryNode>()> ParseNot;
	TFunction<TSharedPtr<FTurboStructLiteQueryNode>()> ParsePrimary;
	TFunction<TSharedPtr<FTurboStructLiteQueryNode>()> ParseComparison;

	auto MakeError = [&](const FString& Message, int32 Position) -> TSharedPtr<FTurboStructLiteQueryNode>
	{
		OutErrorPos = Position;
		OutErrorMessage = FString::Printf(TEXT("Syntax Error (col=%d): %s"), OutErrorPos, *Message);
		return nullptr;
	};

	auto MakeCompileError = [&](const FString& Message, int32 Position) -> TSharedPtr<FTurboStructLiteQueryNode>
	{
		OutErrorPos = Position;
		OutErrorMessage = FString::Printf(TEXT("Compilation Error (col=%d): %s"), OutErrorPos, *Message);
		return nullptr;
	};

	ParseExpression = [&]() -> TSharedPtr<FTurboStructLiteQueryNode>
	{
		return ParseOr();
	};

	ParseOr = [&]() -> TSharedPtr<FTurboStructLiteQueryNode>
	{
		TSharedPtr<FTurboStructLiteQueryNode> Left = ParseAnd();
		if (!Left)
		{
			return nullptr;
		}
		while (Peek().Type == ETurboStructLiteQueryTokenType::Or || Peek().Type == ETurboStructLiteQueryTokenType::OrAlias)
		{
			Advance();
			TSharedPtr<FTurboStructLiteQueryNode> Right = ParseAnd();
			if (!Right)
			{
				return nullptr;
			}
			TSharedPtr<FTurboStructLiteQueryNode> Node = MakeShared<FTurboStructLiteQueryNode>();
			Node->Type = ETurboStructLiteQueryNodeType::Or;
			Node->Left = Left;
			Node->Right = Right;
			Left = Node;
		}
		return Left;
	};

	ParseAnd = [&]() -> TSharedPtr<FTurboStructLiteQueryNode>
	{
		TSharedPtr<FTurboStructLiteQueryNode> Left = ParseNot();
		if (!Left)
		{
			return nullptr;
		}
		while (Peek().Type == ETurboStructLiteQueryTokenType::And || Peek().Type == ETurboStructLiteQueryTokenType::AndAlias)
		{
			Advance();
			TSharedPtr<FTurboStructLiteQueryNode> Right = ParseNot();
			if (!Right)
			{
				return nullptr;
			}
			TSharedPtr<FTurboStructLiteQueryNode> Node = MakeShared<FTurboStructLiteQueryNode>();
			Node->Type = ETurboStructLiteQueryNodeType::And;
			Node->Left = Left;
			Node->Right = Right;
			Left = Node;
		}
		return Left;
	};

	ParseNot = [&]() -> TSharedPtr<FTurboStructLiteQueryNode>
	{
		if (Peek().Type == ETurboStructLiteQueryTokenType::Not || Peek().Type == ETurboStructLiteQueryTokenType::NotAlias)
		{
			const int32 NotPos = Peek().Position;
			Advance();
			if (CurrentDepth >= MaxDepth)
			{
				return MakeCompileError(FString::Printf(TEXT("Query recursion depth exceeded (max=%d)"), MaxDepth), NotPos);
			}
			TGuardValue<int32> DepthGuard(CurrentDepth, CurrentDepth + 1);
			TSharedPtr<FTurboStructLiteQueryNode> Child = ParseNot();
			if (!Child)
			{
				return nullptr;
			}
			TSharedPtr<FTurboStructLiteQueryNode> Node = MakeShared<FTurboStructLiteQueryNode>();
			Node->Type = ETurboStructLiteQueryNodeType::Not;
			Node->Left = Child;
			Node->Right.Reset();
			return Node;
		}
		return ParsePrimary();
	};

	ParsePrimary = [&]() -> TSharedPtr<FTurboStructLiteQueryNode>
	{
		if (Peek().Type == ETurboStructLiteQueryTokenType::LeftParen)
		{
			const int32 ParenPos = Peek().Position;
			Advance();
			if (CurrentDepth >= MaxDepth)
			{
				return MakeCompileError(FString::Printf(TEXT("Query recursion depth exceeded (max=%d)"), MaxDepth), ParenPos);
			}
			TGuardValue<int32> DepthGuard(CurrentDepth, CurrentDepth + 1);
			TSharedPtr<FTurboStructLiteQueryNode> Node = ParseExpression();
			if (!Node)
			{
				return nullptr;
			}
			if (!Match(ETurboStructLiteQueryTokenType::RightParen))
			{
				return MakeError(TEXT("Expected ')'"), Peek().Position);
			}
			return Node;
		}
		return ParseComparison();
	};

	ParseComparison = [&]() -> TSharedPtr<FTurboStructLiteQueryNode>
	{
		const FTurboStructLiteQueryToken& First = Peek();
		if (First.Type != ETurboStructLiteQueryTokenType::Identifier)
		{
			return MakeError(TEXT("Expected property name"), First.Position);
		}
		FTurboStructLiteQueryComparison Comparison;
		Comparison.Lhs.PathSegments.Reset();
		Comparison.Lhs.PathSegments.Add(First.Text);
		Comparison.Lhs.PathPosition = First.Position;
		Advance();
		while (Match(ETurboStructLiteQueryTokenType::Dot))
		{
			const FTurboStructLiteQueryToken& Segment = Peek();
			if (Segment.Type != ETurboStructLiteQueryTokenType::Identifier)
			{
				return MakeError(TEXT("Expected property name after '.'"), Segment.Position);
			}
			Comparison.Lhs.PathSegments.Add(Segment.Text);
			Advance();
		}

		const FTurboStructLiteQueryToken& OpToken = Peek();
		Comparison.OperatorPosition = OpToken.Position;
		switch (OpToken.Type)
		{
			case ETurboStructLiteQueryTokenType::Equal:
				Comparison.Op = ETurboStructLiteQueryCompareOp::Equal;
				break;
			case ETurboStructLiteQueryTokenType::NotEqual:
				Comparison.Op = ETurboStructLiteQueryCompareOp::NotEqual;
				break;
			case ETurboStructLiteQueryTokenType::Greater:
				Comparison.Op = ETurboStructLiteQueryCompareOp::Greater;
				break;
			case ETurboStructLiteQueryTokenType::Less:
				Comparison.Op = ETurboStructLiteQueryCompareOp::Less;
				break;
			case ETurboStructLiteQueryTokenType::GreaterEqual:
				Comparison.Op = ETurboStructLiteQueryCompareOp::GreaterEqual;
				break;
			case ETurboStructLiteQueryTokenType::LessEqual:
				Comparison.Op = ETurboStructLiteQueryCompareOp::LessEqual;
				break;
			case ETurboStructLiteQueryTokenType::Contains:
				Comparison.Op = ETurboStructLiteQueryCompareOp::Contains;
				break;
			default:
				return MakeError(TEXT("Expected comparison operator"), OpToken.Position);
		}
		Advance();

		const FTurboStructLiteQueryToken& LiteralToken = Peek();
		Comparison.LiteralPosition = LiteralToken.Position;
		FTurboStructLiteQueryLiteral Literal;
		switch (LiteralToken.Type)
		{
			case ETurboStructLiteQueryTokenType::Boolean:
				Literal.Type = ETurboStructLiteQueryLiteralType::Boolean;
				Literal.BoolValue = LiteralToken.Text.Equals(TEXT("TRUE"), ESearchCase::IgnoreCase);
				break;
			case ETurboStructLiteQueryTokenType::Number:
			{
				Literal.Type = LiteralToken.Text.Contains(TEXT(".")) ? ETurboStructLiteQueryLiteralType::Float : ETurboStructLiteQueryLiteralType::Integer;
				if (Literal.Type == ETurboStructLiteQueryLiteralType::Float)
				{
					Literal.FloatValue = FCString::Atod(*LiteralToken.Text);
				}
				else
				{
					Literal.IntValue = FCString::Atoi64(*LiteralToken.Text);
				}
				break;
			}

case ETurboStructLiteQueryTokenType::String:
				Literal.Type = ETurboStructLiteQueryLiteralType::String;
				Literal.StringValue = LiteralToken.Text;
				break;
			case ETurboStructLiteQueryTokenType::Identifier:
				Literal.Type = ETurboStructLiteQueryLiteralType::String;
				Literal.StringValue = LiteralToken.Text;
				break;
			default:
				return MakeError(TEXT("Expected literal value"), LiteralToken.Position);
		}
		Advance();

		Comparison.Rhs = MoveTemp(Literal);
		TSharedPtr<FTurboStructLiteQueryNode> Node = MakeShared<FTurboStructLiteQueryNode>();
		Node->Type = ETurboStructLiteQueryNodeType::Comparison;
		Node->Comparison = MoveTemp(Comparison);
		return Node;
	};

	TSharedPtr<FTurboStructLiteQueryNode> Result = ParseExpression();
	if (!Result)
	{
		return false;
	}
	if (Peek().Type != ETurboStructLiteQueryTokenType::EndOfInput)
	{
		OutErrorPos = Peek().Position;
		OutErrorMessage = FString::Printf(TEXT("Syntax Error (col=%d): Unexpected token '%s'"), OutErrorPos, *Peek().Text);
		return false;
	}
	OutRoot = Result;
	return true;
}

DEFINE_FUNCTION(UTurboStructLiteQueryLibrary::execTurboStructLiteValidateQuery)
{
	P_GET_PROPERTY(FStrProperty, QueryString);

	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FProperty>(nullptr);
	FProperty* ContextProp = Stack.MostRecentProperty;
	void* ContextPtr = Stack.MostRecentPropertyAddress;

	P_GET_UBOOL_REF(IsValid);
	P_GET_PROPERTY_REF(FStrProperty, ErrorMessage);

	P_FINISH;

	IsValid = false;
	ErrorMessage.Reset();

	if (!ContextProp || !ContextPtr)
	{
		ErrorMessage = TEXT("Type Error (col=1): Invalid context");
		return;
	}

	FTurboStructLiteLogicQueryContext QueryContext;
	FString LocalError;
	if (!BuildLogicQueryContext(ContextProp, QueryContext, LocalError))
	{
		ErrorMessage = LocalError;
		return;
	}

	const TArray<FString> EmptySelectFields;
	FString ParsedQueryString;
	TArray<FString> ParsedSelectFields;
	int32 ParsedLimit = 0;
	int32 ParsedOffset = 0;
	FString ParsedOrderBy;
	bool bParsedOrderDesc = false;
	TArray<ETurboStructLiteAggregateOp> ParsedAggregateOps;
	TArray<FString> ParsedAggregateFields;
	TArray<FName> ParsedAggregateColumns;
	if (!ParseSelectQueryString(QueryString, EmptySelectFields, ParsedQueryString, ParsedSelectFields, ParsedLimit, ParsedOffset, ParsedOrderBy, bParsedOrderDesc, ParsedAggregateOps, ParsedAggregateFields, ParsedAggregateColumns, LocalError))
	{
		ErrorMessage = LocalError;
		return;
	}

	TArray<FTurboStructLiteQueryToken> Tokens;
	int32 ErrorPos = 0;
	if (!TokenizeLogicQuery(ParsedQueryString, Tokens, LocalError, ErrorPos))
	{
		ErrorMessage = LocalError;
		return;
	}

	TSharedPtr<FTurboStructLiteQueryNode> Root;
	if (!ParseLogicQuery(Tokens, Root, LocalError, ErrorPos))
	{
		ErrorMessage = LocalError;
		return;
	}

	if (!BindLogicQuery(Root, QueryContext, LocalError, ErrorPos))
	{
		ErrorMessage = LocalError;
		return;
	}

	IsValid = true;
	ErrorMessage.Reset();
}





