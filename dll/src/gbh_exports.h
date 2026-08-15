// The complete gbh_* renderer interface gta2.exe resolves by name.
//
// gta2.exe LoadLibrary's whatever the `rendername` registry value points at and
// GetProcAddress's every one of these; a single missing export aborts startup
// with "Can't Find Function Called ...". Listed in the order the game resolves
// them so a failure is easy to locate.
#pragma once

// gbh_InitDLL is deliberately absent: the game always calls it first, so it is
// implemented directly as the place where the backend is bound. Everything else
// is a jump thunk over the table below.
#define GBH_EXPORT_LIST(X)  \
    X(gbh_CloseDLL)         \
    X(gbh_Init)             \
    X(gbh_DrawTile)         \
    X(gbh_DrawTilePart)     \
    X(gbh_DrawQuad)         \
    X(gbh_DrawQuadClipped)  \
    X(gbh_DrawTriangle)     \
    X(gbh_Plot)             \
    X(gbh_SetWindow)        \
    X(gbh_PrintBitmap)      \
    X(gbh_SetColourDepth)   \
    X(gbh_GetGlobals)       \
    X(gbh_ConvertColour)    \
    X(gbh_RegisterTexture)  \
    X(gbh_BeginScene)       \
    X(gbh_EndScene)         \
    X(gbh_BeginLevel)       \
    X(gbh_EndLevel)         \
    X(ConvertColourBank)    \
    X(DrawLine)             \
    X(MakeScreenTable)      \
    X(SetShadeTableA)       \
    X(gbh_UnlockTexture)    \
    X(gbh_RegisterPalette)  \
    X(gbh_FreePalette)      \
    X(gbh_FreeTexture)      \
    X(gbh_AssignPalette)    \
    X(gbh_LockTexture)      \
    X(gbh_GetUsedCache)     \
    X(gbh_SetCamera)        \
    X(gbh_ResetLights)      \
    X(gbh_AddLight)         \
    X(gbh_SetAmbient)       \
    X(gbh_InitImageTable)   \
    X(gbh_FreeImageTable)   \
    X(gbh_LoadImage)        \
    X(gbh_BlitImage)        \
    X(gbh_BlitBuffer)       \
    X(gbh_DrawFlatRect)     \
    X(gbh_CloseScreen)      \
    X(gbh_Convert16BitGraphic)
