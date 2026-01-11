#pragma once

#include "CoreMinimal.h"

// Minimum estimated bytes before enabling parallel query execution.
inline constexpr int32 TurboStructLiteQueryParallelMinBytes = 128 * 1024;
// Minimum allowed queue priority for async query tasks.
inline constexpr int32 TurboStructLiteQueryQueuePriorityMin = 0;
// Maximum allowed queue priority for async query tasks.
inline constexpr int32 TurboStructLiteQueryQueuePriorityMax = 100;
// Default recursion depth when config read fails.
inline constexpr int32 TurboStructLiteQueryDefaultMaxRecursionDepth = 100;
// Minimum allowed recursion depth clamp.
inline constexpr int32 TurboStructLiteQueryMinRecursionDepth = 1;
// SQL keyword: SELECT.
inline const FString TurboStructLiteQueryKey_Select = TEXT("SELECT");
// SQL keyword: FROM.
inline const FString TurboStructLiteQueryKey_From = TEXT("FROM");
// SQL keyword: WHERE.
inline const FString TurboStructLiteQueryKey_Where = TEXT("WHERE");
// SQL keyword: ORDER.
inline const FString TurboStructLiteQueryKey_Order = TEXT("ORDER");
// SQL keyword: BY.
inline const FString TurboStructLiteQueryKey_By = TEXT("BY");
// SQL keyword: LIMIT.
inline const FString TurboStructLiteQueryKey_Limit = TEXT("LIMIT");
// SQL keyword: OFFSET.
inline const FString TurboStructLiteQueryKey_Offset = TEXT("OFFSET");
// SQL keyword: ASC.
inline const FString TurboStructLiteQueryKey_Asc = TEXT("ASC");
// SQL keyword: DESC.
inline const FString TurboStructLiteQueryKey_Desc = TEXT("DESC");
