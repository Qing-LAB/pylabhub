#include "XOPStandardHeaders.h"

/**
 * @file WaveAccess.h
 * @brief Header file for the WaveAccess XOP, defining custom error codes and function prototypes.
 */

/*
        WaveAccess.h -- equates for WaveAccess XOP
*/

/* WaveAccess custom error codes */

/** @brief Error code indicating that the Igor Pro version is too old. */
#define OLD_IGOR 1 + FIRST_XOP_ERR
/** @brief Error code indicating that a specified wave does not exist. */
#define NON_EXISTENT_WAVE 2 + FIRST_XOP_ERR
/** @brief Error code indicating that a function requires a 3D wave but was given a wave of
 * different dimensionality. */
#define NEEDS_3D_WAVE 3 + FIRST_XOP_ERR

/* Prototypes */
/**
 * @brief The main entry point for the XOP, called by Igor Pro on loading.
 * @param ioRecHandle A handle to the IORec structure for communication with Igor.
 * @return An error code, or 0 on success.
 */
HOST_IMPORT int XOPMain(IORecHandle ioRecHandle);
