/* Copyright (C) 2025 Alejo ALDREY - All Rights Reserved
 * This content is downloadable from Epic Games Fab.
 */

#pragma once

/*
 * Descripcion (ES):
 * Developer Settings de TurboStruct. Permite configurar compresion y cifrado por defecto para
 * todas las operaciones del plugin desde el editor o archivos config.
 *
 * Description (EN):
 * TurboStruct Developer Settings. Lets you configure default compression and encryption for all
 * plugin operations from the editor or config files.
 */

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "TurboStructLiteTypes.h"
#include "TurboStructLiteProjectSettings.generated.h"

UCLASS(config = Game, defaultconfig, meta = (DisplayName = "TurboStructLite"))
class TURBOSTRUCTLITEPROJECTSETTINGS_API UTurboStructLiteProjectSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UTurboStructLiteProjectSettings();

	UPROPERTY(EditAnywhere, config, Category = "Encryption")
	ETurboStructLiteEncryptionSettings DefaultEncryption = ETurboStructLiteEncryptionSettings::None;

	UPROPERTY(EditAnywhere, Transient, Category = "Encryption", meta = (EditCondition = "DefaultEncryption == ETurboStructLiteEncryptionSettings::AES", EditConditionHides))
	FString DefaultEncryptionKey;

	UPROPERTY(EditAnywhere, config, Category = "Compression")
	ETurboStructLiteCompressionSettings DefaultCompression = ETurboStructLiteCompressionSettings::Oodle;

	UPROPERTY(EditAnywhere, config, Category = "Compression")
	ETurboStructLiteBatching DefaultCompressionBatching = ETurboStructLiteBatching::Four;

	UPROPERTY(EditAnywhere, config, Category = "Serialization")
	TMap<FString, FString> LegacyRedirects;

	UPROPERTY(EditAnywhere, config, Category = "Logic Query")
	int32 MaxQueryRecursionDepth = 100;

	UPROPERTY(EditAnywhere, config, Category = "Debug")
	bool bShowDebugPrintString = false;

	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	virtual FText GetSectionText() const override;
};
