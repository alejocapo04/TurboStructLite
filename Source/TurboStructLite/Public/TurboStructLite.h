/* Copyright (C) 2025 Alejo ALDREY - All Rights Reserved
 * This content is downloadable from Epic Games Fab.
 */

#pragma once

/*
 * Descripcion (ES):
 * Header del modulo runtime de TurboStructLite. Declara el modulo principal que maneja el ciclo
 * de vida Startup/Shutdown del plugin de guardado de structs.
 *
 * Description (EN):
 * Header for the TurboStructLite runtime module. Declares the primary module handling the
 * Startup/Shutdown lifecycle for the struct-saving plugin.
 */

#include "Modules/ModuleManager.h"

class FTurboStructLiteModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
