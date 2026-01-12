/* Copyright (C) 2025 Alejo ALDREY - All Rights Reserved
 * This content is downloadable from Epic Games Fab.
 */

#pragma once

/*
 * Descripcion (ES):
 * Header del modulo TurboStructLiteDebug. Maneja el ciclo de vida de utilidades de debug.
 *
 * Description (EN):
 * TurboStructLiteDebug module header. Handles lifecycle for debug utilities.
 */

#include "Modules/ModuleManager.h"

class FTurboStructLiteDebugModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
