/*	WaveAccess.c

        Igor Pro 3.0 extended wave data storage from 1 dimension to 4 dimensions.
        This sample XOP illustrates how to access waves of dimension 1 through 4.

        The function WAGetWaveInfo illustrates the use of the following calls:
                WaveName, WaveType, WaveUnits, WaveScaling
                MDGetWaveDimensions, MDGetWaveScaling, MDGetWaveUnits, MDGetDimensionLabels

        It will  with Igor Pro 2.0 or later.

        Invoke it from Igor's command line like this:
                Make/N=(5,4,3) wave3D		// wave with 5 rows, 4 columns and 3 layers
                Print WAGetWaveInfo(wave3D)

        The functions WAFill3DWaveDirectMethod, WAFill3DWavePointMethod and
        WAFill3DWaveStorageMethod each fill a 3D wave with values, using different
        wave access methods. They all require Igor Pro 3.0 or later.

        You can invoke these functions from Igor Pro 3.0 or later as follows:
                Edit wave3D
                WAFill3DWaveDirectMethod(wave3D)
        or	WAFill3DWavePointMethod(wave3D)
        or	WAFill3DWaveStorageMethod(wave3D)

        The function fills a 3 dimensional wave with values such that:
                w[p][q][r] = p + 1e3*q + 1e6*r
        where p is the row number, q is the column number and r is the layer number.
        This is the equivalent of executing the following in Igor Pro 3.0 or later:
                wave3D = p + 1e3*q + 1e6*r

        The function WAModifyTextWave shows how to read and write the contents of
        a text wave. Invoke it from Igor Pro 3.0 or later like this:
                Make/T/N=(4,4) textWave2D = "(" + num2str(p) + "," + num2str(q) + ")"
                Edit textWave2D
                WAModifyTextWave(textWave2D, "Row/col=", ".")

        HR, 091021
                Updated for 64-bit compatibility.

        HR, 2013-02-08
                Updated for Xcode 4 compatibility. Changed to use XOPMain instead of main.
                As a result the XOP now requires Igor Pro 6.20 or later.

        HR, 2018-05-04
                Recompiled with XOP Toolkit 8 which supports long object names.
                As a result the XOP now requires Igor Pro 8.00 or later.
*/
#include "WaveAccess.h"
#include "XOPStandardHeaders.h" // Include ANSI headers, Mac headers, IgorXOP.h, XOP.h and XOPSupport.h
#include "plh_platform.hpp"
#include "storage.hpp"
#include "storage.hpp"
#include "plh_service.hpp"
#include "logging.hpp"
#include <fmt/format.h>

// Global Variables
static int gCallSpinProcess = 1; // Set to 1 to all user abort (cmd dot) and background processing.

static int AddCStringToHandle( // Concatenates C string to handle.
    const char *theStr, Handle theHand)
{
    return WMPtrAndHand(theStr, theHand, strlen(theStr));
}

#pragma pack(2) // All structures passed to Igor are two-byte aligned
struct WAGetWaveInfoParams
{
    waveHndl w;
    Handle strH;
};
typedef struct WAGetWaveInfoParams WAGetWaveInfoParams;
#pragma pack() // Reset structure alignment to default.

extern "C" int
WAGetWaveInfo(WAGetWaveInfoParams *p) // See the top of the file for instructions on how to invoke
                                      // this function from Igor Pro 3.0 or later.
{
    char buf[256];
    char waveName[MAX_OBJ_NAME + 1];
    int waveType;
    int dimension, numDimensions;
    CountInt dimensionSizes[MAX_DIMENSIONS + 1];
    char dimensionUnits[MAX_DIMENSIONS][MAX_UNIT_CHARS + 1];
    IndexInt element;
    char dataUnits[MAX_UNIT_CHARS + 1];
    double dataFullScaleMax, dataFullScaleMin;
    double sfA[MAX_DIMENSIONS];
    double sfB[MAX_DIMENSIONS];
    char dimLabel[MAX_DIM_LABEL_BYTES + 1];
    int result;
    decltype(fmt::format_to_n(buf, 0, "")) result_buf_tuple;

    if (p->w == NULL)
    {
        p->strH = NULL;           // Tell Igor that function return value is undefined.
        return NON_EXISTENT_WAVE; // Make sure wave exists.
    }

    p->strH = WMNewHandle(0L);
    if (p->strH == NULL)
        return NOMEM;

    // Get wave name.
    WaveName(p->w, waveName);

    // Get wave data type.
    waveType = WaveType(p->w);

    // Get number of used dimensions in wave.
    if ((result = MDGetWaveDimensions(p->w, &numDimensions, dimensionSizes)))
        return result;

    /*	Get wave scaling for all used dimensions.
            The scaled index value for point p of dimension d is computed as:
                    scaled index = p*sfA[d] + sfB[d];
    */
    for (dimension = 0; dimension < numDimensions; dimension++)
    {
        if ((result = MDGetWaveScaling(p->w, dimension, &sfA[dimension], &sfB[dimension])))
            return result;
    }

    // Get units for all dimensions.
    for (dimension = 0; dimension < numDimensions; dimension++)
    {
        if ((result = MDGetWaveUnits(p->w, dimension, &dimensionUnits[dimension][0])))
            return result;
    }

    /*	Get the data nominal full scale values for the wave.
            -1 means get full scale values instead of dimension scaling.
    */
    if ((result = MDGetWaveScaling(p->w, -1, &dataFullScaleMax, &dataFullScaleMin)))
        return result;

    /*	Get the data units for the wave.
            -1 means data units instead of dimension units.
    */
    if ((result = MDGetWaveUnits(p->w, -1, dataUnits)))
        return result;

    // Now, store all of the info in the handle to return to Igor.

    result_buf_tuple =
        fmt::format_to_n(buf, sizeof(buf) - 1, "Wave name: \'{}\'; type: {}; dimensions: {}",
                         waveName, waveType, numDimensions);
    *result_buf_tuple.out = '\0'; // Null-terminate the string
    if ((result = AddCStringToHandle(buf, p->strH)))
        return result;

    // Add the data units and nominal full scale values.
    result_buf_tuple =
        fmt::format_to_n(buf, sizeof(buf) - 1, "; data units=\"{}\"; data full scale={},{}",
                         dataUnits, dataFullScaleMin, dataFullScaleMax);
    *result_buf_tuple.out = '\0'; // Null-terminate the string
    if ((result = AddCStringToHandle(buf, p->strH)))
        return result;

    // Add information for each dimension.
    decltype(fmt::format_to_n(buf, 0, "")) result_buf_inner; // Declare here to avoid shadowing
    for (dimension = 0; dimension < numDimensions; dimension++)
    {
        if ((result = AddCStringToHandle(CR_STR, p->strH))) // Add CR.
            return result;
        result_buf_inner = fmt::format_to_n(
            buf, sizeof(buf) - 1,
            "\tDimension number: {}, size={}, sfA={}, sfB={}, dimensionUnits=\"{}\"{}", dimension,
            (SInt64)dimensionSizes[dimension], sfA[dimension], sfB[dimension],
            dimensionUnits[dimension], CR_STR);
        *result_buf_inner.out = '\0'; // Null-terminate the string
        if ((result = AddCStringToHandle(buf, p->strH)))
            return result;

        //	Get dimension label for each element of this dimension.
        if ((result = AddCStringToHandle("\t\tLabels: ", p->strH)))
            return result;
        for (element = -1; element < dimensionSizes[dimension]; element++)
        { // Loop starts from -1 because -1 returns
            if (element >= 5)
            { // the label for the entire dimension.
                if ((result = AddCStringToHandle("(and so on)", p->strH)))
                    return result;
                break;
            }
            if ((result = MDGetDimensionLabel(p->w, dimension, element, dimLabel)))
                return result;
            result_buf_inner = fmt::format_to_n(buf, sizeof(buf) - 1, "\'{}\'", dimLabel);
            if (element < dimensionSizes[dimension] - 1)
            {
                result_buf_inner = fmt::format_to_n(
                    result_buf_inner.out, sizeof(buf) - (result_buf_inner.out - buf), ", ");
            }
            *result_buf_inner.out = '\0'; // Null-terminate the string
            if ((result = AddCStringToHandle(buf, p->strH)))
                return result;
        }
    }

    return (0); // XFUNC error code.
}

/*	WAFill3DWaveDirectMethod()

        This example shows how to access the data in a multi-dimensional wave
        using the direct method.

        See the top of the file for instructions on how to invoke this function
        from Igor Pro 3.0 or later.
*/

#pragma pack(2) // All structures passed to Igor are two-byte aligned
struct WAFill3DWaveDirectMethodParams
{
    waveHndl w;
    double result;
};
typedef struct WAFill3DWaveDirectMethodParams WAFill3DWaveDirectMethodParams;
#pragma pack() // Reset structure alignment to default.

template <typename T>
static int Fill3DWave(T *dataStartPtr, CountInt numLayers, CountInt numColumns, CountInt numRows)
{
    CountInt pointsPerColumn = numRows;
    CountInt pointsPerLayer = pointsPerColumn * numColumns;
    int result = 0;

    T *layerPtr, *colPtr, *pointPtr;
    for (IndexInt layer = 0; layer < numLayers; layer++)
    {
        layerPtr = dataStartPtr + layer * pointsPerLayer;
        for (IndexInt column = 0; column < numColumns; column++)
        {
            if (gCallSpinProcess && SpinProcess())
            {
                result = -1; // User aborted.
                break;
            }
            colPtr = layerPtr + column * pointsPerColumn;
            for (IndexInt row = 0; row < numRows; row++)
            {
                pointPtr = colPtr + row;
                *pointPtr = (T)(row + 1000 * column + 1000000 * layer);
            }
        }
        if (result != 0)
            break; // User abort.
    }
    return result;
}

extern "C" int WAFill3DWaveDirectMethod(WAFill3DWaveDirectMethodParams *p)
{
    waveHndl waveH = NULL;
    int waveType;
    int numDimensions;
    CountInt dimensionSizes[MAX_DIMENSIONS + 1];
    char *dataStartPtr;
    IndexInt dataOffset;
    CountInt numRows, numColumns, numLayers;
    int result;

    p->result = 0; // The Igor function result is always zero.

    waveH = p->w;
    if (waveH == NULL)
        return NOWAV;

    waveType = WaveType(waveH);
    if (waveType & NT_CMPLX)
        return NO_COMPLEX_WAVE;
    if (waveType == TEXT_WAVE_TYPE)
        return NUMERIC_ACCESS_ON_TEXT_WAVE;

    if ((result = MDGetWaveDimensions(waveH, &numDimensions, dimensionSizes)))
        return result;

    if (numDimensions != 3)
        return NEEDS_3D_WAVE;

    numRows = dimensionSizes[0];
    numColumns = dimensionSizes[1];
    numLayers = dimensionSizes[2];

    if ((result = MDAccessNumericWaveData(waveH, kMDWaveAccessMode0, &dataOffset)))
        return result;

    dataStartPtr = (char *)(*waveH) + dataOffset;

    switch (waveType)
    {
    case NT_FP64:
        result = Fill3DWave<double>((double *)dataStartPtr, numLayers, numColumns, numRows);
        break;
    case NT_FP32:
        result = Fill3DWave<float>((float *)dataStartPtr, numLayers, numColumns, numRows);
        break;
    case NT_I32:
        result = Fill3DWave<SInt32>((SInt32 *)dataStartPtr, numLayers, numColumns, numRows);
        break;
    case NT_I16:
        result = Fill3DWave<short>((short *)dataStartPtr, numLayers, numColumns, numRows);
        break;
    case NT_I8:
        result = Fill3DWave<char>((char *)dataStartPtr, numLayers, numColumns, numRows);
        break;
    case NT_I32 | NT_UNSIGNED:
        result = Fill3DWave<UInt32>((UInt32 *)dataStartPtr, numLayers, numColumns, numRows);
        break;
    case NT_I16 | NT_UNSIGNED:
        result = Fill3DWave<unsigned short>((unsigned short *)dataStartPtr, numLayers, numColumns,
                                            numRows);
        break;
    case NT_I8 | NT_UNSIGNED:
        result = Fill3DWave<unsigned char>((unsigned char *)dataStartPtr, numLayers, numColumns,
                                           numRows);
        break;
    default:
        return NT_FNOT_AVAIL;
    }

    WaveHandleModified(waveH);

    return result;
}

/*	WAFill3DWavePointMethod()

        This example shows how to access the data in a multi-dimensional wave
        using a slower but very easy access method.

        See the top of the file for instructions on how to invoke this function
        from Igor Pro 3.0 or later.

        By using the MDSetNumericWavePointValue routine to store into the wave, instead of
        accessing the wave directly, we relieve ourselves of the need to worry about
        the data type of the wave, at the cost of running more slowly.
*/

#pragma pack(2) // All structures passed to Igor are two-byte aligned
struct WAFill3DWavePointMethodParams
{
    waveHndl w;
    double result;
};
typedef struct WAFill3DWavePointMethodParams WAFill3DWavePointMethodParams;
#pragma pack() // Reset structure alignment to default.

static int WAFill3DWavePointMethod(WAFill3DWavePointMethodParams *p)
{
    waveHndl waveH = NULL;
    int waveType;
    int numDimensions;
    CountInt dimensionSizes[MAX_DIMENSIONS + 1];
    IndexInt indices[MAX_DIMENSIONS]; // Used to pass the row, column and layer to
                                      // MDSetNumericWavePointValue.
    double value[2];                  // Contains, real/imaginary parts but we use the real only.
    CountInt numRows, numColumns, numLayers;
    IndexInt row, column, layer;
    int result;

    p->result = 0; // The Igor function result is always zero.

    waveH = p->w;
    if (waveH == NULL)
        return NOWAV;

    waveType = WaveType(waveH);
    if (waveType & NT_CMPLX)
        return NO_COMPLEX_WAVE;
    if (waveType == TEXT_WAVE_TYPE)
        return NUMERIC_ACCESS_ON_TEXT_WAVE;

    if ((result = MDGetWaveDimensions(waveH, &numDimensions, dimensionSizes)))
        return result;

    if (numDimensions != 3)
        return NEEDS_3D_WAVE;

    numRows = dimensionSizes[0];
    numColumns = dimensionSizes[1];
    numLayers = dimensionSizes[2];

    MemClear(indices, sizeof(indices)); // Unused indices must be zero.
    result = 0;
    for (layer = 0; layer < numLayers; layer++)
    {
        indices[2] = layer;
        for (column = 0; column < numColumns; column++)
        {
            if (gCallSpinProcess && SpinProcess())
            {                // Spins cursor and allows background processing.
                result = -1; // User aborted.
                break;
            }
            indices[1] = column;
            for (row = 0; row < numRows; row++)
            {
                indices[0] = row;
                value[0] = (double)(row + 1000 * column + 1000000 * layer);
                if ((result = MDSetNumericWavePointValue(waveH, indices, value)))
                {
                    WaveHandleModified(waveH); // Inform Igor that we have changed the wave.
                    return result;
                }
            }
        }
        if (result != 0)
            break;
    }

    WaveHandleModified(waveH); // Inform Igor that we have changed the wave.

    return result;
}

/*	WAFill3DWaveStorageMethod()

        This example shows how to access the data in a multi-dimensional wave
        using the temp storage method. It is fast and easy but requires enough
        memory for a temporary double-precision copy of the wave data.

        See the top of the file for instructions on how to invoke this function
        from Igor Pro 3.0 or later.
*/

#pragma pack(2) // All structures passed to Igor are two-byte aligned
struct WAFill3DWaveStorageMethodParams
{
    waveHndl w;
    double result;
};
typedef struct WAFill3DWaveStorageMethodParams WAFill3DWaveStorageMethodParams;
#pragma pack() // Reset structure alignment to default.

extern "C" int WAFill3DWaveStorageMethod(WAFill3DWaveStorageMethodParams *p)
{
    waveHndl waveH = NULL;
    int waveType;
    int numDimensions;
    CountInt dimensionSizes[MAX_DIMENSIONS + 1];
    CountInt numRows, numColumns, numLayers;
    BCInt numBytes;
    double *dPtr;
    int result, result2;

    p->result = 0; // The Igor function result is always zero.

    waveH = p->w;
    if (waveH == NULL)
        return NOWAV;

    waveType = WaveType(waveH);
    if (waveType & NT_CMPLX)
        return NO_COMPLEX_WAVE;
    if (waveType == TEXT_WAVE_TYPE)
        return NUMERIC_ACCESS_ON_TEXT_WAVE;

    if ((result = MDGetWaveDimensions(waveH, &numDimensions, dimensionSizes)))
        return result;

    if (numDimensions != 3)
        return NEEDS_3D_WAVE;

    numRows = dimensionSizes[0];
    numColumns = dimensionSizes[1];
    numLayers = dimensionSizes[2];

    numBytes = WavePoints(waveH) * sizeof(double); // Bytes needed for copy
    dPtr = (double *)WMNewPtr(numBytes);
    if (dPtr == NULL)
        return NOMEM;

    if ((result = MDGetDPDataFromNumericWave(waveH, dPtr)))
    { // Get a copy of the wave data.
        WMDisposePtr((Ptr)dPtr);
        return result;
    }

    result = Fill3DWave<double>(dPtr, numLayers, numColumns, numRows);

    if (result == 0)
    {
        if ((result2 = MDStoreDPDataInNumericWave(waveH, dPtr)))
        { // Store copy in the wave.
            WMDisposePtr((Ptr)dPtr);
            return result2;
        }
    }

    WMDisposePtr((Ptr)dPtr);
    WaveHandleModified(waveH); // Inform Igor that we have changed the wave.

    return result;
}

static int DoAppendAndPrepend(Handle textH, Handle prependStringH, Handle appendStringH)
{
    BCInt textHLength;
    BCInt appendStringHLength;
    BCInt prependStringHLength;

    textHLength = WMGetHandleSize(textH);
    prependStringHLength = WMGetHandleSize(prependStringH);
    appendStringHLength = WMGetHandleSize(appendStringH);

    int err = WMSetHandleSize(textH, textHLength + prependStringHLength + appendStringHLength);
    if (err != 0)
        return err;
    memmove(*textH + prependStringHLength, *textH, textHLength); // Make room for prependString.
    memcpy(*textH, *prependStringH, prependStringHLength);       // Prepend prependString.
    memcpy(*textH + textHLength + prependStringHLength, *appendStringH,
           appendStringHLength); // Append appendString.
    return 0;
}

/*	WAModifyTextWave()

        This example shows how to access the data in a multi-dimensional text wave.

        See the top of the file for instructions on how to invoke this function
        from Igor Pro 3.0 or later.
*/

#pragma pack(2) // All structures passed to Igor are two-byte aligned
struct WAModifyTextWaveParams
{
    Handle appendStringH;  // String to be appended to each wave point.
    Handle prependStringH; // String to be prepended to each wave point.
    waveHndl w;
    double result;
};
typedef struct WAModifyTextWaveParams WAModifyTextWaveParams;
#pragma pack() // Reset structure alignment to default.

extern "C" int WAModifyTextWave(WAModifyTextWaveParams *p)
{
    waveHndl waveH = NULL;
    int waveType;
    int numDimensions;
    CountInt dimensionSizes[MAX_DIMENSIONS + 1];
    IndexInt indices[MAX_DIMENSIONS]; // Used to pass the row, column and layer to
                                      // MDSetTextWavePointValue.
    CountInt numRows, numColumns, numLayers, numChunks;
    IndexInt row, column, layer, chunk;
    Handle textH;
    int result;

    result = 0;

    textH = WMNewHandle(0L); // Handle used to pass text wave characters to Igor.
    if (textH == NULL)
    {
        result = NOMEM;
        goto done;
    }

    if (p->prependStringH == NULL)
    {
        result = USING_NULL_STRVAR; // The user called the function with an uninitialized string
                                    // variable.
        goto done;
    }

    if (p->appendStringH == NULL)
    {
        result = USING_NULL_STRVAR; // The user called the function with an uninitialized string
                                    // variable.
        goto done;
    }

    waveH = p->w;
    if (waveH == NULL)
    {
        result = NOWAV; // The user called the function with a missing wave or uninitialized wave
                        // reference variable.
        goto done;
    }

    waveType = WaveType(waveH);
    if (waveType != TEXT_WAVE_TYPE)
    {
        result = TEXT_ACCESS_ON_NUMERIC_WAVE;
        goto done;
    }

    if ((result = MDGetWaveDimensions(waveH, &numDimensions, dimensionSizes)))
        goto done;

    numRows = dimensionSizes[0];
    numColumns = dimensionSizes[1];
    if (numColumns == 0)
        numColumns = 1;
    numLayers = dimensionSizes[2];
    if (numLayers == 0)
        numLayers = 1;
    numChunks = dimensionSizes[3];
    if (numChunks == 0)
        numChunks = 1;

    MemClear(indices, sizeof(indices)); // Clear unused indices.
    result = 0;
    for (chunk = 0; chunk < numChunks; chunk++)
    {
        indices[3] = chunk;
        for (layer = 0; layer < numLayers; layer++)
        {
            indices[2] = layer;
            for (column = 0; column < numColumns; column++)
            {
                if (gCallSpinProcess && SpinProcess())
                {                // Spins cursor and allows background processing.
                    result = -1; // User aborted.
                    break;
                }
                indices[1] = column;
                for (row = 0; row < numRows; row++)
                {
                    indices[0] = row;
                    if ((result = MDGetTextWavePointValue(waveH, indices, textH)))
                        goto done;
                    if ((result = DoAppendAndPrepend(textH, p->prependStringH, p->appendStringH)))
                        goto done;
                    if ((result = MDSetTextWavePointValue(waveH, indices, textH)))
                        goto done;
                }
            }
            if (result != 0)
                break;
        }
        if (result != 0)
            break;
    }

done:
    if (waveH != NULL)
        WaveHandleModified(waveH);      // Inform Igor that we have changed the wave.
    WMDisposeHandle(textH);             // OK if NULL
    WMDisposeHandle(p->prependStringH); // We need to get rid of input parameters. OK if NULL.
    WMDisposeHandle(p->appendStringH);  // We need to get rid of input parameters. OK if NULL.
    p->result = 0;                      // The Igor function result is always zero.

    return (result);
}

/*	RegisterFunction()

        Igor calls this at startup time to find the address of the
        XFUNCs added by this XOP. See XOP manual regarding "Direct XFUNCs".
*/
static XOPIORecResult RegisterFunction()
{
    int funcIndex;

    funcIndex = (int)GetXOPItem(0); // Which function is Igor asking about?
    switch (funcIndex)
    {
    case 0: // WAGetWaveInfo(wave)
        return (XOPIORecResult)WAGetWaveInfo;
        break;
    case 1: // WAFill3DWaveDirectMethod(wave)
        return (XOPIORecResult)WAFill3DWaveDirectMethod;
        break;
    case 2: // WAFill3DWavePointMethod(wave)
        return (XOPIORecResult)WAFill3DWavePointMethod;
        break;
    case 3: // WAFill3DWaveStorageMethod(wave)
        return (XOPIORecResult)WAFill3DWaveStorageMethod;
        break;
    case 4: // WAModifyTextWave(wave, prependString, appendString)
        return (XOPIORecResult)WAModifyTextWave;
        break;
    }
    return 0;
}

/*	DoFunction()

        Igor calls this when the user invokes one if the XOP's XFUNCs
        if we returned NULL for the XFUNC from RegisterFunction. In this
        XOP, we always use the direct XFUNC method, so Igor will never call
        this function. See XOP manual regarding "Direct XFUNCs".
*/
static int DoFunction()
{
    int funcIndex;
    void *p; // Pointer to structure containing function parameters and result.
    int err = 0;

    funcIndex = (int)GetXOPItem(0); // Which function is being invoked ?
    p = (void *)GetXOPItem(1);      // Get pointer to params/result.
    switch (funcIndex)
    {
    case 0: // WAGetWaveInfo(wave)
        err = WAGetWaveInfo((WAGetWaveInfoParams *)p);
        break;
    case 1: // WAFill3DWaveDirectMethod(wave)
        err = WAFill3DWaveDirectMethod((WAFill3DWaveDirectMethodParams *)p);
        break;
    case 2: // WAFill3DWavePointMethod(wave)
        err = WAFill3DWavePointMethod((WAFill3DWavePointMethodParams *)p);
        break;
    case 3: // WAFill3DWaveStorageMethod(wave)
        err = WAFill3DWaveStorageMethod((WAFill3DWaveStorageMethodParams *)p);
        break;
    case 4: // WAModifyTextWave(wave, prependString, appendString)
        err = WAModifyTextWave((WAModifyTextWaveParams *)p);
        break;
    }
    return (err);
}

/*	XOPEntry()

        This is the entry point from the host application to the XOP for all messages after the
        INIT message.
*/
extern "C" void XOPEntry(void)
{
    XOPIORecResult result = 0;

    switch (GetXOPMessage())
    {
    case FUNCTION: // Our external function being invoked ?
        result = DoFunction();
        break;

    case FUNCADDRS:
        result = RegisterFunction();
        break;
    }
    SetXOPResult(result);
}

/*	XOPMain(ioRecHandle)

        This is the initial entry point at which the host application calls XOP.
        The message sent by the host must be INIT.

        XOPMain does any necessary initialization and then sets the XOPEntry field of the
        ioRecHandle to the address to be called for future messages.
*/
HOST_IMPORT int XOPMain(IORecHandle ioRecHandle)
{
    XOPInit(ioRecHandle);  // Do standard XOP initialization.
    SetXOPEntry(XOPEntry); // Set entry point for future calls.
    using namespace pylabhub::utils;
    // Initialize our application's shared services (like Logger).
    // This is safe to call even if other plugins also call it, as it's idempotent.
    LifecycleGuard guard(MakeModDefList(FileLock::GetLifecycleModule(),
                                    JsonConfig::GetLifecycleModule(),
                                    Logger::GetLifecycleModule()));
    LOGGER_INFO("pylabhubxop64 plugin loaded and logger initialized.");

    if (igorVersion < 800)
    {                           // XOP Toolkit 8.00 or later requires Igor Pro 8.00 or later
        SetXOPResult(OLD_IGOR); // OLD_IGOR is defined in WaveAccess.h and there are corresponding
                                // error strings in WaveAccess.r and WaveAccessWinCustom.rc.
        return EXIT_FAILURE;
    }

    SetXOPResult(0L);
    return EXIT_SUCCESS;
}
