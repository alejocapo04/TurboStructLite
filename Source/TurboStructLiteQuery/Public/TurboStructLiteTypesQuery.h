#pragma once

#include "CoreMinimal.h"
#include "TurboStructLiteTypes.h"
#include "TurboStructLiteTypesQuery.generated.h"

struct FStructProperty;

UENUM(BlueprintType)
enum class ETurboStructLiteQueryExec : uint8
{
	OnSuccess UMETA(DisplayName = "OnSuccess"),
	OnFail UMETA(DisplayName = "OnFail")
};

enum class ETurboStructLiteQueryTokenType : uint8
{
	Identifier,
	Number,
	String,
	Boolean,
	Dot,
	Equal,
	NotEqual,
	Greater,
	Less,
	GreaterEqual,
	LessEqual,
	And,
	Or,
	Not,
	AndAlias,
	OrAlias,
	NotAlias,
	Contains,
	LeftParen,
	RightParen,
	EndOfInput,
	Comma,
	Asterisk,
	Select,
	From,
	Where,
	Order,
	By,
	Limit,
	Offset,
	Asc,
	Desc
};

enum class ETurboStructLiteQueryNodeType : uint8
{
	Comparison,
	And,
	Or,
	Not
};

enum class ETurboStructLiteQueryCompareOp : uint8
{
	Equal,
	NotEqual,
	Greater,
	Less,
	GreaterEqual,
	LessEqual,
	Contains
};

enum class ETurboStructLiteQueryLiteralType : uint8
{
	None,
	Boolean,
	Integer,
	Float,
	String,
	Name
};

enum class ETurboStructLiteQueryValueRoot : uint8
{
	Root,
	MapKey,
	MapValue
};

struct FTurboStructLiteQueryToken
{
	ETurboStructLiteQueryTokenType Type = ETurboStructLiteQueryTokenType::EndOfInput;
	FString Text;
	int32 Position = 0;
};

struct FTurboStructLiteQueryLiteral
{
	ETurboStructLiteQueryLiteralType Type = ETurboStructLiteQueryLiteralType::None;
	bool BoolValue = false;
	int64 IntValue = 0;
	double FloatValue = 0.0;
	FString StringValue;
	FName NameValue;
};

struct FTurboStructLiteQueryBoundProperty
{
	ETurboStructLiteQueryValueRoot Root = ETurboStructLiteQueryValueRoot::Root;
	TArray<FString> PathSegments;
	int32 PathPosition = 0;
	TArray<FProperty*> PropertyChain;
	FProperty* LeafProperty = nullptr;
	FProperty* ContainerElementProperty = nullptr;
	bool bIsContainer = false;
	bool bUseMapKey = false;
	bool bUseMapValue = false;
};

struct FTurboStructLiteQueryComparison
{
	FTurboStructLiteQueryBoundProperty Lhs;
	ETurboStructLiteQueryCompareOp Op = ETurboStructLiteQueryCompareOp::Equal;
	FTurboStructLiteQueryLiteral Rhs;
	int32 OperatorPosition = 0;
	int32 LiteralPosition = 0;
};

struct FTurboStructLiteQueryNode
{
	ETurboStructLiteQueryNodeType Type = ETurboStructLiteQueryNodeType::Comparison;
	FTurboStructLiteQueryComparison Comparison;
	TSharedPtr<FTurboStructLiteQueryNode> Left;
	TSharedPtr<FTurboStructLiteQueryNode> Right;
};

struct alignas(64) FTurboStructLiteThreadResultBucket
{
	TArray<int32> Indices;
};

struct FTurboStructLiteLogicQueryStats
{
	int32 Scanned = 0;
	int32 Matched = 0;
	double ElapsedMs = 0.0;
};

struct FTurboStructLiteLogicQueryContext
{
	UStruct* RootStruct = nullptr;
	FProperty* RootProperty = nullptr;
	FProperty* MapKeyProperty = nullptr;
	FProperty* MapValueProperty = nullptr;
	bool bAllowMapKeyValue = false;
};

enum class ETurboStructLiteAggregateOp : uint8
{
	Count,
	Sum,
	Avg
};

USTRUCT(BlueprintType)
struct TURBOSTRUCTLITEQUERY_API FTurboStructLiteRow
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TurboStructLite")
	TMap<FName, FTurboStructLiteVariant> Columns;
};

struct FTurboStructLiteSelectFieldInfo
{
	FName ColumnName;
	FName PathKey;
	TArray<FProperty*> PropertyChain;
	FProperty* LeafProperty = nullptr;
	bool bCountOnly = false;
};

struct FTurboStructLiteQueryExecutionContext
{
	const FString* SlotName = nullptr;
	int32 SubSlotIndex = -1;
	const FString* QueryString = nullptr;
	const FString* EncryptionKey = nullptr;
	ETurboStructLiteEncryption SelectedEncryption = ETurboStructLiteEncryption::ProjectDefault;
	int32 MaxParallelThreads = 1;
	bool bUseWriteAheadLog = false;
	const FString* WALPath = nullptr;
	UStruct* ContextStruct = nullptr;
	FStructProperty* ContextStructProp = nullptr;

	bool* bOutHasAggregates = nullptr;
	TArray<FTurboStructLiteRow>* OutRows = nullptr;
	FString* OutMetadata = nullptr;
	FDateTime* OutSaveDate = nullptr;
	FString* OutStatsText = nullptr;
	FString* OutErrorMessage = nullptr;

	FString ParsedQueryString;
	TArray<FString> ParsedSelectFields;
	int32 ParsedLimit = 0;
	int32 ParsedOffset = 0;
	FString ParsedOrderBy;
	bool bParsedOrderDesc = false;
	TArray<ETurboStructLiteAggregateOp> ParsedAggregateOps;
	TArray<FString> ParsedAggregateFields;
	TArray<FName> ParsedAggregateColumns;

	bool bHasAggregates = false;
	bool bHasOrderBy = false;
	bool bQueryIsTrue = false;
	bool bAggregateCountOnly = false;

	FTurboStructLiteLogicQueryContext QueryContext;
	TArray<FTurboStructLiteQueryToken> Tokens;
	int32 ErrorPos = 0;
	TSharedPtr<FTurboStructLiteQueryNode> Root;

	TArray<FTurboStructLiteSelectFieldInfo> SelectFieldInfos;
	FTurboStructLiteSelectFieldInfo OrderFieldInfo;
	TArray<FTurboStructLiteSelectFieldInfo> AggregateFieldInfos;
	TArray<int32> AggregateFieldIndices;

	int32 ClampedParallel = 1;
	double StartSeconds = 0.0;
	FTurboStructLiteLogicQueryStats Stats;
	TArray<FTurboStructLiteRow> Results;
	TArray<double> SortKeys;
	TArray<int64> AggregateCounts;
	TArray<double> AggregateSums;
	bool bHasLimit = false;
	bool bHasOffset = false;
	bool bForceSingleThread = false;
	bool bAllowEarlyExit = false;
	int32 MaxMatchIndex = 0;
	bool bOffsetAppliedInLoop = false;
	bool bLimitAppliedInLoop = false;
	int64 FastCountValue = 0;
	TArray<int32> SubSlots;
};
