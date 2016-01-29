/******************************************************************************
 * $Id$
 *
 * Project:  GDAL
 * Purpose:  Implements Arc/Info ASCII Grid Format.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2001, Frank Warmerdam (warmerdam@pobox.com)
 * Copyright (c) 2007-2012, Even Rouault <even dot rouault at mines-paris dot org>
 * Copyright (c) 2014, Kyle Shannon <kyle at pobox dot com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/

// We need cpl_port as first include to avoid VSIStatBufL being not
// defined on i586-mingw32msvc.
#include "cpl_port.h"

#include <ctype.h>
#include <climits>

#include "cpl_string.h"
#include "gdal_pam.h"
#include "gdal_frmts.h"
#include "ogr_spatialref.h"

CPL_CVSID("$Id$");

static CPLString OSR_GDS( char **papszNV, const char * pszField,
                          const char *pszDefaultValue );

typedef enum
{
    FORMAT_AAIG,
    FORMAT_GRASSASCII
} GridFormat;

/************************************************************************/
/* ==================================================================== */
/*                              AAIGDataset                             */
/* ==================================================================== */
/************************************************************************/

class AAIGRasterBand;

class CPL_DLL AAIGDataset : public GDALPamDataset
{
    friend class AAIGRasterBand;

    VSILFILE   *fp;

    char        **papszPrj;
    CPLString   osPrjFilename;
    char        *pszProjection;

    unsigned char achReadBuf[256];
    GUIntBig    nBufferOffset;
    int         nOffsetInBuffer;

    char        Getc();
    GUIntBig    Tell();
    int         Seek( GUIntBig nOffset );

  protected:
    GDALDataType eDataType;
    double      adfGeoTransform[6];
    int         bNoDataSet;
    double      dfNoDataValue;


    virtual int ParseHeader(const char* pszHeader, const char* pszDataType);

  public:
                AAIGDataset();
                ~AAIGDataset();

    virtual char **GetFileList(void);

    static GDALDataset *CommonOpen( GDALOpenInfo * poOpenInfo,
                                    GridFormat eFormat );

    static GDALDataset *Open( GDALOpenInfo * );
    static int          Identify( GDALOpenInfo * );
    static CPLErr       Delete( const char *pszFilename );
    static CPLErr       Remove( const char *pszFilename, int bRepError );
    static GDALDataset *CreateCopy( const char * pszFilename,
                                    GDALDataset *poSrcDS,
                                    int bStrict, char ** papszOptions,
                                    GDALProgressFunc pfnProgress,
                                    void * pProgressData );

    virtual CPLErr GetGeoTransform( double * );
    virtual const char *GetProjectionRef(void);
};

/************************************************************************/
/* ==================================================================== */
/*                        GRASSASCIIDataset                             */
/* ==================================================================== */
/************************************************************************/

class GRASSASCIIDataset : public AAIGDataset
{
    virtual int ParseHeader(const char* pszHeader, const char* pszDataType);

  public:
                GRASSASCIIDataset() : AAIGDataset() {}

    static GDALDataset *Open( GDALOpenInfo * );
    static int          Identify( GDALOpenInfo * );
};

/************************************************************************/
/* ==================================================================== */
/*                            AAIGRasterBand                             */
/* ==================================================================== */
/************************************************************************/

class AAIGRasterBand : public GDALPamRasterBand
{
    friend class AAIGDataset;

    GUIntBig      *panLineOffset;

  public:

                   AAIGRasterBand( AAIGDataset *, int );
    virtual       ~AAIGRasterBand();

    virtual double GetNoDataValue( int * );
    virtual CPLErr SetNoDataValue( double );
    virtual CPLErr IReadBlock( int, int, void * );
};

/************************************************************************/
/*                           AAIGRasterBand()                            */
/************************************************************************/

AAIGRasterBand::AAIGRasterBand( AAIGDataset *poDSIn, int nDataStart )

{
    this->poDS = poDSIn;

    nBand = 1;
    eDataType = poDSIn->eDataType;

    nBlockXSize = poDSIn->nRasterXSize;
    nBlockYSize = 1;

    panLineOffset = 
        (GUIntBig *) VSI_CALLOC_VERBOSE( poDSIn->nRasterYSize, sizeof(GUIntBig) );
    if (panLineOffset == NULL)
    {
        return;
    }
    panLineOffset[0] = nDataStart;
}

/************************************************************************/
/*                          ~AAIGRasterBand()                           */
/************************************************************************/

AAIGRasterBand::~AAIGRasterBand()

{
    CPLFree( panLineOffset );
}

/************************************************************************/
/*                             IReadBlock()                             */
/************************************************************************/

CPLErr AAIGRasterBand::IReadBlock( int nBlockXOff, int nBlockYOff,
                                  void * pImage )

{
    AAIGDataset *poODS = (AAIGDataset *) poDS;
    int         iPixel;

    if( nBlockYOff < 0 || nBlockYOff > poODS->nRasterYSize - 1
        || nBlockXOff != 0 || panLineOffset == NULL || poODS->fp == NULL )
        return CE_Failure;

    if( panLineOffset[nBlockYOff] == 0 )
    {
        int iPrevLine;

        for( iPrevLine = 1; iPrevLine <= nBlockYOff; iPrevLine++ )
            if( panLineOffset[iPrevLine] == 0 )
                IReadBlock( nBlockXOff, iPrevLine-1, NULL );
    }

    if( panLineOffset[nBlockYOff] == 0 )
        return CE_Failure;


    if( poODS->Seek( panLineOffset[nBlockYOff] ) != 0 )
    {
        CPLError( CE_Failure, CPLE_FileIO,
                  "Can't seek to offset %lu in input file to read data.",
                  (long unsigned int)panLineOffset[nBlockYOff] );
        return CE_Failure;
    }

    for( iPixel = 0; iPixel < poODS->nRasterXSize; )
    {
        char szToken[500];
        char chNext;
        int  iTokenChar = 0;

        /* suck up any pre-white space. */
        do {
            chNext = poODS->Getc();
        } while( isspace( (unsigned char)chNext ) );

        while( chNext != '\0' && !isspace((unsigned char)chNext)  )
        {
            if( iTokenChar == sizeof(szToken)-2 )
            {
                CPLError( CE_Failure, CPLE_FileIO, 
                          "Token too long at scanline %d.", 
                          nBlockYOff );
                return CE_Failure;
            }

            szToken[iTokenChar++] = chNext;
            chNext = poODS->Getc();
        }

        if( chNext == '\0' &&
            (iPixel != poODS->nRasterXSize - 1 ||
            nBlockYOff != poODS->nRasterYSize - 1) )
        {
            CPLError( CE_Failure, CPLE_FileIO, 
                      "File short, can't read line %d.",
                      nBlockYOff );
            return CE_Failure;
        }

        szToken[iTokenChar] = '\0';

        if( pImage != NULL )
        {
            if( eDataType == GDT_Float64 )
                ((double *) pImage)[iPixel] = CPLAtofM(szToken);
            else if( eDataType == GDT_Float32 )
                ((float *) pImage)[iPixel] = (float) CPLAtofM(szToken);
            else
                ((GInt32 *) pImage)[iPixel] = (GInt32) atoi(szToken);
        }

        iPixel++;
    }

    if( nBlockYOff < poODS->nRasterYSize - 1 )
        panLineOffset[nBlockYOff + 1] = poODS->Tell();

    return CE_None;
}

/************************************************************************/
/*                           GetNoDataValue()                           */
/************************************************************************/

double AAIGRasterBand::GetNoDataValue( int * pbSuccess )

{
    AAIGDataset *poODS = (AAIGDataset *) poDS;

    if( pbSuccess )
        *pbSuccess = poODS->bNoDataSet;

    return poODS->dfNoDataValue;
}


/************************************************************************/
/*                           SetNoDataValue()                           */
/************************************************************************/

CPLErr AAIGRasterBand::SetNoDataValue( double dfNoData )

{
    AAIGDataset *poODS = (AAIGDataset *) poDS;

    poODS->bNoDataSet = TRUE;
    poODS->dfNoDataValue = dfNoData;

    return CE_None;
}

/************************************************************************/
/* ==================================================================== */
/*                            AAIGDataset                               */
/* ==================================================================== */
/************************************************************************/


/************************************************************************/
/*                            AAIGDataset()                            */
/************************************************************************/

AAIGDataset::AAIGDataset()

{
    papszPrj = NULL;
    pszProjection = CPLStrdup("");
    fp = NULL;
    eDataType = GDT_Int32;
    bNoDataSet = FALSE;
    dfNoDataValue = -9999.0;

    adfGeoTransform[0] = 0.0;
    adfGeoTransform[1] = 1.0;
    adfGeoTransform[2] = 0.0;
    adfGeoTransform[3] = 0.0;
    adfGeoTransform[4] = 0.0;
    adfGeoTransform[5] = 1.0;

    nOffsetInBuffer = 256;
    nBufferOffset = 0;
}

/************************************************************************/
/*                           ~AAIGDataset()                            */
/************************************************************************/

AAIGDataset::~AAIGDataset()

{
    FlushCache();

    if( fp != NULL )
    {
        if( VSIFCloseL( fp ) != 0 )
        {
            CPLError(CE_Failure, CPLE_FileIO, "I/O error");
        }
    }

    CPLFree( pszProjection );
    CSLDestroy( papszPrj );
}

/************************************************************************/
/*                                Tell()                                */
/************************************************************************/

GUIntBig AAIGDataset::Tell()

{
    return nBufferOffset + nOffsetInBuffer;
}

/************************************************************************/
/*                                Seek()                                */
/************************************************************************/

int AAIGDataset::Seek( GUIntBig nNewOffset )

{
    nOffsetInBuffer = sizeof(achReadBuf);
    return VSIFSeekL( fp, nNewOffset, SEEK_SET );
}

/************************************************************************/
/*                                Getc()                                */
/*                                                                      */
/*      Read a single character from the input file (efficiently we     */
/*      hope).                                                          */
/************************************************************************/

char AAIGDataset::Getc()

{
    if( nOffsetInBuffer < (int) sizeof(achReadBuf) )
        return achReadBuf[nOffsetInBuffer++];

    nBufferOffset = VSIFTellL( fp );
    int nRead = static_cast<int>(VSIFReadL( achReadBuf, 1, sizeof(achReadBuf), fp ));
    unsigned int i;
    for(i=nRead;i<sizeof(achReadBuf);i++)
        achReadBuf[i] = '\0';

    nOffsetInBuffer = 0;

    return achReadBuf[nOffsetInBuffer++];
}

/************************************************************************/
/*                            GetFileList()                             */
/************************************************************************/

char **AAIGDataset::GetFileList()

{
    char **papszFileList = GDALPamDataset::GetFileList();

    if( papszPrj != NULL )
        papszFileList = CSLAddString( papszFileList, osPrjFilename );

    return papszFileList;
}

/************************************************************************/
/*                            Identify()                                */
/************************************************************************/

int AAIGDataset::Identify( GDALOpenInfo * poOpenInfo )

{
/* -------------------------------------------------------------------- */
/*      Does this look like an AI grid file?                            */
/* -------------------------------------------------------------------- */
    if( poOpenInfo->nHeaderBytes < 40
        || !( STARTS_WITH_CI((const char *) poOpenInfo->pabyHeader, "ncols") ||
              STARTS_WITH_CI((const char *) poOpenInfo->pabyHeader, "nrows") ||
              STARTS_WITH_CI((const char *) poOpenInfo->pabyHeader, "xllcorner")||
              STARTS_WITH_CI((const char *) poOpenInfo->pabyHeader, "yllcorner")||
              STARTS_WITH_CI((const char *) poOpenInfo->pabyHeader, "xllcenter")||
              STARTS_WITH_CI((const char *) poOpenInfo->pabyHeader, "yllcenter")||
              STARTS_WITH_CI((const char *) poOpenInfo->pabyHeader, "dx")||
              STARTS_WITH_CI((const char *) poOpenInfo->pabyHeader, "dy")||
              STARTS_WITH_CI((const char *) poOpenInfo->pabyHeader, "cellsize")) )
        return FALSE;

    return TRUE;
}

/************************************************************************/
/*                            Identify()                                */
/************************************************************************/

int GRASSASCIIDataset::Identify( GDALOpenInfo * poOpenInfo )

{
/* -------------------------------------------------------------------- */
/*      Does this look like a GRASS ASCII grid file?                    */
/* -------------------------------------------------------------------- */
    if( poOpenInfo->nHeaderBytes < 40
        || !( STARTS_WITH_CI((const char *) poOpenInfo->pabyHeader, "north:") ||
              STARTS_WITH_CI((const char *) poOpenInfo->pabyHeader, "south:") ||
              STARTS_WITH_CI((const char *) poOpenInfo->pabyHeader, "east:")||
              STARTS_WITH_CI((const char *) poOpenInfo->pabyHeader, "west:")||
              STARTS_WITH_CI((const char *) poOpenInfo->pabyHeader, "rows:")||
              STARTS_WITH_CI((const char *) poOpenInfo->pabyHeader, "cols:") ) )
        return FALSE;

    return TRUE;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *AAIGDataset::Open( GDALOpenInfo * poOpenInfo )
{
    if (!Identify(poOpenInfo))
        return NULL;

    return CommonOpen(poOpenInfo, FORMAT_AAIG);
}

/************************************************************************/
/*                          ParseHeader()                               */
/************************************************************************/

int AAIGDataset::ParseHeader(const char* pszHeader, const char* pszDataType)
{
    int i, j;
    char** papszTokens =
        CSLTokenizeString2( pszHeader,  " \n\r\t" , 0 );
    int nTokens = CSLCount(papszTokens);
    double dfCellDX = 0;
    double dfCellDY = 0;

    if ( (i = CSLFindString( papszTokens, "ncols" )) < 0 ||
         i + 1 >= nTokens)
    {
        CSLDestroy( papszTokens );
        return FALSE;
    }
    nRasterXSize = atoi(papszTokens[i + 1]);
    if ( (i = CSLFindString( papszTokens, "nrows" )) < 0 ||
         i + 1 >= nTokens)
    {
        CSLDestroy( papszTokens );
        return FALSE;
    }
    nRasterYSize = atoi(papszTokens[i + 1]);

    if (!GDALCheckDatasetDimensions(nRasterXSize, nRasterYSize))
    {
        CSLDestroy( papszTokens );
        return FALSE;
    }

    if ( (i = CSLFindString( papszTokens, "cellsize" )) < 0 )
    {
        int iDX, iDY;
        if( (iDX = CSLFindString(papszTokens,"dx")) < 0
            || (iDY = CSLFindString(papszTokens,"dy")) < 0
            || iDX+1 >= nTokens
            || iDY+1 >= nTokens)
        {
            CSLDestroy( papszTokens );
            return FALSE;
        }

        dfCellDX = CPLAtofM( papszTokens[iDX+1] );
        dfCellDY = CPLAtofM( papszTokens[iDY+1] );
    }
    else
    {
        if (i + 1 >= nTokens)
        {
            CSLDestroy( papszTokens );
            return FALSE;
        }
        dfCellDX = dfCellDY = CPLAtofM( papszTokens[i + 1] );
    }

    if ((i = CSLFindString( papszTokens, "xllcorner" )) >= 0 &&
        (j = CSLFindString( papszTokens, "yllcorner" )) >= 0 &&
        i + 1 < nTokens && j + 1 < nTokens)
    {
        adfGeoTransform[0] = CPLAtofM( papszTokens[i + 1] );

        /* Small hack to compensate from insufficient precision in cellsize */
        /* parameter in datasets of http://ccafs-climate.org/data/A2a_2020s/hccpr_hadcm3 */
        if ((nRasterXSize % 360) == 0 &&
            fabs(adfGeoTransform[0] - (-180.0)) < 1e-12 &&
            dfCellDX == dfCellDY &&
            fabs(dfCellDX - (360.0 / nRasterXSize)) < 1e-9)
        {
            dfCellDX = dfCellDY = 360.0 / nRasterXSize;
        }

        adfGeoTransform[1] = dfCellDX;
        adfGeoTransform[2] = 0.0;
        adfGeoTransform[3] = CPLAtofM( papszTokens[j + 1] )
            + nRasterYSize * dfCellDY;
        adfGeoTransform[4] = 0.0;
        adfGeoTransform[5] = - dfCellDY;
    }
    else if ((i = CSLFindString( papszTokens, "xllcenter" )) >= 0 &&
             (j = CSLFindString( papszTokens, "yllcenter" )) >= 0  &&
             i + 1 < nTokens && j + 1 < nTokens)
    {
        SetMetadataItem( GDALMD_AREA_OR_POINT, GDALMD_AOP_POINT );

        adfGeoTransform[0] = CPLAtofM(papszTokens[i + 1]) - 0.5 * dfCellDX;
        adfGeoTransform[1] = dfCellDX;
        adfGeoTransform[2] = 0.0;
        adfGeoTransform[3] = CPLAtofM( papszTokens[j + 1] )
            - 0.5 * dfCellDY
            + nRasterYSize * dfCellDY;
        adfGeoTransform[4] = 0.0;
        adfGeoTransform[5] = - dfCellDY;
    }
    else
    {
        adfGeoTransform[0] = 0.0;
        adfGeoTransform[1] = dfCellDX;
        adfGeoTransform[2] = 0.0;
        adfGeoTransform[3] = 0.0;
        adfGeoTransform[4] = 0.0;
        adfGeoTransform[5] = - dfCellDY;
    }

    if( (i = CSLFindString( papszTokens, "NODATA_value" )) >= 0 &&
        i + 1 < nTokens)
    {
        const char* pszNoData = papszTokens[i + 1];

        bNoDataSet = TRUE;
        dfNoDataValue = CPLAtofM(pszNoData);
        if( pszDataType == NULL &&
            (strchr( pszNoData, '.' ) != NULL ||
             strchr( pszNoData, ',' ) != NULL ||
             INT_MIN > dfNoDataValue || dfNoDataValue > INT_MAX) )
        {
            eDataType = GDT_Float32;
        }
        if( eDataType == GDT_Float32 )
        {
            dfNoDataValue = (double) (float) dfNoDataValue;
        }
    }

    CSLDestroy( papszTokens );

    return TRUE;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *GRASSASCIIDataset::Open( GDALOpenInfo * poOpenInfo )
{
    if (!Identify(poOpenInfo))
        return NULL;

    return CommonOpen(poOpenInfo, FORMAT_GRASSASCII);
}


/************************************************************************/
/*                          ParseHeader()                               */
/************************************************************************/

int GRASSASCIIDataset::ParseHeader(const char* pszHeader, const char* pszDataType)
{
    int i;
    char** papszTokens =
        CSLTokenizeString2( pszHeader,  " \n\r\t:" , 0 );
    int nTokens = CSLCount(papszTokens);
    if ( (i = CSLFindString( papszTokens, "cols" )) < 0 ||
         i + 1 >= nTokens)
    {
        CSLDestroy( papszTokens );
        return FALSE;
    }
    nRasterXSize = atoi(papszTokens[i + 1]);
    if ( (i = CSLFindString( papszTokens, "rows" )) < 0 ||
         i + 1 >= nTokens)
    {
        CSLDestroy( papszTokens );
        return FALSE;
    }
    nRasterYSize = atoi(papszTokens[i + 1]);

    if (!GDALCheckDatasetDimensions(nRasterXSize, nRasterYSize))
    {
        CSLDestroy( papszTokens );
        return FALSE;
    }

    int iNorth = CSLFindString( papszTokens, "north" );
    int iSouth = CSLFindString( papszTokens, "south" );
    int iEast = CSLFindString( papszTokens, "east" );
    int iWest = CSLFindString( papszTokens, "west" );

    if (iNorth == -1 || iSouth == -1 || iEast == -1 || iWest == -1 ||
        MAX(MAX(iNorth, iSouth), MAX(iEast, iWest)) + 1 >= nTokens)
    {
        CSLDestroy( papszTokens );
        return FALSE;
    }

    double dfNorth = CPLAtofM( papszTokens[iNorth + 1] );
    double dfSouth = CPLAtofM( papszTokens[iSouth + 1] );
    double dfEast = CPLAtofM( papszTokens[iEast + 1] );
    double dfWest = CPLAtofM( papszTokens[iWest + 1] );
    double dfPixelXSize = (dfEast - dfWest) / nRasterXSize;
    double dfPixelYSize = (dfNorth - dfSouth) / nRasterYSize;

    adfGeoTransform[0] = dfWest;
    adfGeoTransform[1] = dfPixelXSize;
    adfGeoTransform[2] = 0.0;
    adfGeoTransform[3] = dfNorth;
    adfGeoTransform[4] = 0.0;
    adfGeoTransform[5] = - dfPixelYSize;

    if( (i = CSLFindString( papszTokens, "null" )) >= 0 &&
        i + 1 < nTokens)
    {
        const char* pszNoData = papszTokens[i + 1];

        bNoDataSet = TRUE;
        dfNoDataValue = CPLAtofM(pszNoData);
        if( pszDataType == NULL &&
            (strchr( pszNoData, '.' ) != NULL ||
             strchr( pszNoData, ',' ) != NULL ||
             INT_MIN > dfNoDataValue || dfNoDataValue > INT_MAX) )
        {
            eDataType = GDT_Float32;
        }
        if( eDataType == GDT_Float32 )
        {
            dfNoDataValue = (double) (float) dfNoDataValue;
        }
    }

    if( (i = CSLFindString( papszTokens, "type" )) >= 0 &&
        i + 1 < nTokens)
    {
        const char* pszType = papszTokens[i + 1];
        if (EQUAL(pszType, "int"))
            eDataType = GDT_Int32;
        else if (EQUAL(pszType, "float"))
            eDataType = GDT_Float32;
        else if (EQUAL(pszType, "double"))
            eDataType = GDT_Float64;
        else
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Invalid value for type parameter : %s", pszType);
        }
    }

    CSLDestroy(papszTokens);

    return TRUE;
}

/************************************************************************/
/*                           CommonOpen()                               */
/************************************************************************/

GDALDataset *AAIGDataset::CommonOpen( GDALOpenInfo * poOpenInfo,
                                            GridFormat eFormat )
{
    int i = 0;

/* -------------------------------------------------------------------- */
/*      Create a corresponding GDALDataset.                             */
/* -------------------------------------------------------------------- */
    AAIGDataset         *poDS;

    if (eFormat == FORMAT_AAIG)
        poDS = new AAIGDataset();
    else
        poDS = new GRASSASCIIDataset();


    const char* pszDataTypeOption = (eFormat == FORMAT_AAIG) ? "AAIGRID_DATATYPE":
                                                               "GRASSASCIIGRID_DATATYPE";
    const char* pszDataType = CPLGetConfigOption(pszDataTypeOption, NULL);
    if( pszDataType == NULL )
        pszDataType = CSLFetchNameValue( poOpenInfo->papszOpenOptions, "DATATYPE" );
    if (pszDataType != NULL)
    {
        poDS->eDataType = GDALGetDataTypeByName(pszDataType);
        if (!(poDS->eDataType == GDT_Int32 || poDS->eDataType == GDT_Float32 ||
              poDS->eDataType == GDT_Float64))
        {
            CPLError(CE_Warning, CPLE_NotSupported,
                     "Unsupported value for %s : %s", pszDataTypeOption, pszDataType);
            poDS->eDataType = GDT_Int32;
            pszDataType = NULL;
        }
    }

/* -------------------------------------------------------------------- */
/*      Parse the header.                                               */
/* -------------------------------------------------------------------- */
    if (!poDS->ParseHeader((const char *) poOpenInfo->pabyHeader, pszDataType))
    {
        delete poDS;
        return NULL;
    }

/* -------------------------------------------------------------------- */
/*      Open file with large file API.                                  */
/* -------------------------------------------------------------------- */

    poDS->fp = VSIFOpenL( poOpenInfo->pszFilename, "r" );
    if( poDS->fp == NULL )
    {
        CPLError( CE_Failure, CPLE_OpenFailed, 
                  "VSIFOpenL(%s) failed unexpectedly.", 
                  poOpenInfo->pszFilename );
        delete poDS;
        return NULL;
    }

/* -------------------------------------------------------------------- */
/*      Find the start of real data.                                    */
/* -------------------------------------------------------------------- */
    int         nStartOfData;

    for( i = 2; true ; i++ )
    {
        if( poOpenInfo->pabyHeader[i] == '\0' )
        {
            CPLError( CE_Failure, CPLE_AppDefined, 
                      "Couldn't find data values in ASCII Grid file.\n" );
            delete poDS;
            return NULL;
        }

        if( poOpenInfo->pabyHeader[i-1] == '\n' 
            || poOpenInfo->pabyHeader[i-2] == '\n' 
            || poOpenInfo->pabyHeader[i-1] == '\r' 
            || poOpenInfo->pabyHeader[i-2] == '\r' )
        {
            if( !isalpha(poOpenInfo->pabyHeader[i]) 
                && poOpenInfo->pabyHeader[i] != '\n' 
                && poOpenInfo->pabyHeader[i] != '\r')
            {
                nStartOfData = i;

				/* Beginning of real data found. */
                break;
            }
        }
    }

/* -------------------------------------------------------------------- */
/*      Recognize the type of data.										*/
/* -------------------------------------------------------------------- */
    CPLAssert( NULL != poDS->fp );

    if( pszDataType == NULL && poDS->eDataType != GDT_Float32)
    {
        /* Allocate 100K chunk + 1 extra byte for NULL character. */
        const size_t nChunkSize = 1024 * 100;
        GByte* pabyChunk = (GByte *) VSI_CALLOC_VERBOSE( nChunkSize + 1, sizeof(GByte) );
        if (pabyChunk == NULL)
        {
            delete poDS;
            return NULL;
        }
        pabyChunk[nChunkSize] = '\0';

        if( VSIFSeekL( poDS->fp, nStartOfData, SEEK_SET ) < 0 )
        {
            delete poDS;
            VSIFree( pabyChunk );
            return NULL;
        }

        /* Scan for dot in subsequent chunks of data. */
        while( !VSIFEofL( poDS->fp) )
        {
            CPL_IGNORE_RET_VAL(VSIFReadL( pabyChunk, nChunkSize, 1, poDS->fp ));

            for(i = 0; i < (int)nChunkSize; i++)
            {
                GByte ch = pabyChunk[i];
                if (ch == '.' || ch == ',' || ch == 'e' || ch == 'E')
                {
                    poDS->eDataType = GDT_Float32;
                    break;
                }
            }
        }

        /* Deallocate chunk. */
        VSIFree( pabyChunk );
    }

/* -------------------------------------------------------------------- */
/*      Create band information objects.                                */
/* -------------------------------------------------------------------- */
    AAIGRasterBand* band = new AAIGRasterBand( poDS, nStartOfData );
    poDS->SetBand( 1, band );
    if (band->panLineOffset == NULL)
    {
        delete poDS;
        return NULL;
    }

/* -------------------------------------------------------------------- */
/*      Try to read projection file.                                    */
/* -------------------------------------------------------------------- */
    char        *pszDirname, *pszBasename;
    VSIStatBufL   sStatBuf;

    pszDirname = CPLStrdup(CPLGetPath(poOpenInfo->pszFilename));
    pszBasename = CPLStrdup(CPLGetBasename(poOpenInfo->pszFilename));

    poDS->osPrjFilename = CPLFormFilename( pszDirname, pszBasename, "prj" );
    int nRet = VSIStatL( poDS->osPrjFilename, &sStatBuf );

    if( nRet != 0 && VSIIsCaseSensitiveFS(poDS->osPrjFilename) )
    {
        poDS->osPrjFilename = CPLFormFilename( pszDirname, pszBasename, "PRJ" );
        nRet = VSIStatL( poDS->osPrjFilename, &sStatBuf );
    }

    if( nRet == 0 )
    {
        OGRSpatialReference     oSRS;

        poDS->papszPrj = CSLLoad( poDS->osPrjFilename );

        CPLDebug( "AAIGrid", "Loaded SRS from %s", 
                  poDS->osPrjFilename.c_str() );

        if( oSRS.importFromESRI( poDS->papszPrj ) == OGRERR_NONE )
        {
            // If geographic values are in seconds, we must transform. 
            // Is there a code for minutes too? 
            if( oSRS.IsGeographic() 
                && EQUAL(OSR_GDS( poDS->papszPrj, "Units", ""), "DS") )
            {
                poDS->adfGeoTransform[0] /= 3600.0;
                poDS->adfGeoTransform[1] /= 3600.0;
                poDS->adfGeoTransform[2] /= 3600.0;
                poDS->adfGeoTransform[3] /= 3600.0;
                poDS->adfGeoTransform[4] /= 3600.0;
                poDS->adfGeoTransform[5] /= 3600.0;
            }

            CPLFree( poDS->pszProjection );
            oSRS.exportToWkt( &(poDS->pszProjection) );
        }
    }

    CPLFree( pszDirname );
    CPLFree( pszBasename );

/* -------------------------------------------------------------------- */
/*      Initialize any PAM information.                                 */
/* -------------------------------------------------------------------- */
    poDS->SetDescription( poOpenInfo->pszFilename );
    poDS->TryLoadXML();

/* -------------------------------------------------------------------- */
/*      Check for external overviews.                                   */
/* -------------------------------------------------------------------- */
    poDS->oOvManager.Initialize( poDS, poOpenInfo->pszFilename, poOpenInfo->GetSiblingFiles() );

    return( poDS );
}

/************************************************************************/
/*                          GetGeoTransform()                           */
/************************************************************************/

CPLErr AAIGDataset::GetGeoTransform( double * padfTransform )

{
    memcpy( padfTransform, adfGeoTransform, sizeof(double) * 6 );
    return( CE_None );
}

/************************************************************************/
/*                          GetProjectionRef()                          */
/************************************************************************/

const char *AAIGDataset::GetProjectionRef()

{
    return pszProjection;
}

/************************************************************************/
/*                          CreateCopy()                                */
/************************************************************************/

GDALDataset * AAIGDataset::CreateCopy(
    const char * pszFilename, GDALDataset *poSrcDS,
    CPL_UNUSED int bStrict,
    char ** papszOptions,
    GDALProgressFunc pfnProgress, void * pProgressData )
{
    const int nBands = poSrcDS->GetRasterCount();
    const int nXSize = poSrcDS->GetRasterXSize();
    const int nYSize = poSrcDS->GetRasterYSize();

/* -------------------------------------------------------------------- */
/*      Some rudimentary checks                                         */
/* -------------------------------------------------------------------- */
    if( nBands != 1 )
    {
        CPLError( CE_Failure, CPLE_NotSupported,
                  "AAIG driver doesn't support %d bands.  Must be 1 band.\n",
                  nBands );

        return NULL;
    }

    if( !pfnProgress( 0.0, NULL, pProgressData ) )
        return NULL;

/* -------------------------------------------------------------------- */
/*      Create the dataset.                                             */
/* -------------------------------------------------------------------- */
    VSILFILE *fpImage = VSIFOpenL( pszFilename, "wt" );
    if( fpImage == NULL )
    {
        CPLError( CE_Failure, CPLE_OpenFailed, 
                  "Unable to create file %s.\n", 
                  pszFilename );
        return NULL;
    }

/* -------------------------------------------------------------------- */
/*      Write ASCII Grid file header                                    */
/* -------------------------------------------------------------------- */
    double      adfGeoTransform[6];
    char        szHeader[2000];
    const char *pszForceCellsize =
        CSLFetchNameValue( papszOptions, "FORCE_CELLSIZE" );

    poSrcDS->GetGeoTransform( adfGeoTransform );

    if( ABS(adfGeoTransform[1]+adfGeoTransform[5]) < 0.0000001
        || ABS(adfGeoTransform[1]-adfGeoTransform[5]) < 0.0000001
        || (pszForceCellsize && CPLTestBool(pszForceCellsize)) )
        CPLsnprintf( szHeader, sizeof(szHeader),
                 "ncols        %d\n"
                 "nrows        %d\n"
                 "xllcorner    %.12f\n"
                 "yllcorner    %.12f\n"
                 "cellsize     %.12f\n",
                 nXSize, nYSize,
                 adfGeoTransform[0],
                 adfGeoTransform[3] - nYSize * adfGeoTransform[1],
                 adfGeoTransform[1] );
    else
    {
        if( pszForceCellsize == NULL )
            CPLError( CE_Warning, CPLE_AppDefined,
                      "Producing a Golden Surfer style file with DX and DY instead\n"
                      "of CELLSIZE since the input pixels are non-square.  Use the\n"
                      "FORCE_CELLSIZE=TRUE creation option to force use of DX for\n"
                      "even though this will be distorted.  Most ASCII Grid readers\n"
                      "(ArcGIS included) do not support the DX and DY parameters.\n" );
        CPLsnprintf( szHeader, sizeof(szHeader),
                 "ncols        %d\n"
                 "nrows        %d\n"
                 "xllcorner    %.12f\n"
                 "yllcorner    %.12f\n"
                 "dx           %.12f\n"
                 "dy           %.12f\n",
                 nXSize, nYSize,
                 adfGeoTransform[0],
                 adfGeoTransform[3] + nYSize * adfGeoTransform[5],
                 adfGeoTransform[1],
                 fabs(adfGeoTransform[5]) );
    }

/* -------------------------------------------------------------------- */
/*     Builds the format string used for printing float values.         */
/* -------------------------------------------------------------------- */
    char szFormatFloat[32];
    strcpy(szFormatFloat, " %.20g");
    const char *pszDecimalPrecision = 
        CSLFetchNameValue( papszOptions, "DECIMAL_PRECISION" );
    const char *pszSignificantDigits =
        CSLFetchNameValue( papszOptions, "SIGNIFICANT_DIGITS" );
    int bIgnoreSigDigits = FALSE;
    if( pszDecimalPrecision && pszSignificantDigits )
    {
        CPLError( CE_Warning, CPLE_AppDefined,
                  "Conflicting precision arguments, using DECIMAL_PRECISION" );
        bIgnoreSigDigits = TRUE;
    }
    int nPrecision;
    if ( pszSignificantDigits && !bIgnoreSigDigits )
    {
        nPrecision = atoi( pszSignificantDigits );
        if (nPrecision >= 0)
            snprintf( szFormatFloat, sizeof(szFormatFloat), " %%.%dg", nPrecision );
        CPLDebug( "AAIGrid", "Setting precision format: %s", szFormatFloat );
    }
    else if( pszDecimalPrecision )
    {
        nPrecision = atoi( pszDecimalPrecision );
        if ( nPrecision >= 0 )
            snprintf( szFormatFloat, sizeof(szFormatFloat), " %%.%df", nPrecision );
        CPLDebug( "AAIGrid", "Setting precision format: %s", szFormatFloat );
    }

/* -------------------------------------------------------------------- */
/*      Handle nodata (optionally).                                     */
/* -------------------------------------------------------------------- */
    GDALRasterBand * poBand = poSrcDS->GetRasterBand( 1 );
    double dfNoData;
    int bSuccess;
    int bReadAsInt;
    bReadAsInt = ( poBand->GetRasterDataType() == GDT_Byte
                || poBand->GetRasterDataType() == GDT_Int16
                || poBand->GetRasterDataType() == GDT_UInt16
                || poBand->GetRasterDataType() == GDT_Int32 );

    // Write `nodata' value to header if it is exists in source dataset
    dfNoData = poBand->GetNoDataValue( &bSuccess );
    if ( bSuccess )
    {
        snprintf( szHeader+strlen( szHeader ),
                  sizeof(szHeader) - strlen(szHeader), "%s", "NODATA_value " );
        if( bReadAsInt )
            snprintf( szHeader+strlen( szHeader ),
                  sizeof(szHeader) - strlen(szHeader), "%d", (int)dfNoData );
        else
            CPLsnprintf( szHeader+strlen( szHeader ),
                  sizeof(szHeader) - strlen(szHeader), szFormatFloat, dfNoData );
        snprintf( szHeader+strlen( szHeader ),
                  sizeof(szHeader) - strlen(szHeader), "%s", "\n" );
    }

    if( VSIFWriteL( szHeader, strlen(szHeader), 1, fpImage ) != 1)
    {
        CPL_IGNORE_RET_VAL(VSIFCloseL(fpImage));
        return NULL;
    }

/* -------------------------------------------------------------------- */
/*      Loop over image, copying image data.                            */
/* -------------------------------------------------------------------- */
    int         *panScanline = NULL;
    double      *padfScanline = NULL;
    int         iLine, iPixel;
    CPLErr      eErr = CE_None;

    // Write scanlines to output file
    if (bReadAsInt)
        panScanline = (int *) CPLMalloc( nXSize *
                                GDALGetDataTypeSize(GDT_Int32) / 8 );
    else
        padfScanline = (double *) CPLMalloc( nXSize *
                                    GDALGetDataTypeSize(GDT_Float64) / 8 );

    int bHasOutputDecimalDot = FALSE;
    for( iLine = 0; eErr == CE_None && iLine < nYSize; iLine++ )
    {
        CPLString osBuf;
        eErr = poBand->RasterIO( GF_Read, 0, iLine, nXSize, 1, 
                                 (bReadAsInt) ? (void*)panScanline : (void*)padfScanline,
                                 nXSize, 1, (bReadAsInt) ? GDT_Int32 : GDT_Float64,
                                 0, 0, NULL );

        if( bReadAsInt )
        {
            for ( iPixel = 0; iPixel < nXSize; iPixel++ )
            {
                snprintf( szHeader, sizeof(szHeader), " %d", panScanline[iPixel] );
                osBuf += szHeader;
                if( (iPixel & 1023) == 0 || iPixel == nXSize - 1 )
                {
                    if ( VSIFWriteL( osBuf, (int)osBuf.size(), 1, fpImage ) != 1 )
                    {
                        eErr = CE_Failure;
                        CPLError( CE_Failure, CPLE_AppDefined, 
                                  "Write failed, disk full?\n" );
                        break;
                    }
                    osBuf = "";
                }
            }
        }
        else
        {
            for ( iPixel = 0; iPixel < nXSize; iPixel++ )
            {
                CPLsnprintf( szHeader, sizeof(szHeader), szFormatFloat, padfScanline[iPixel] );

                // Make sure that as least one value has a decimal point (#6060)
                if( !bHasOutputDecimalDot )
                {
                    if( strchr(szHeader, '.') || strchr(szHeader, 'e') || strchr(szHeader, 'E') )
                        bHasOutputDecimalDot = TRUE;
                    else if( !CPLIsInf(padfScanline[iPixel]) && !CPLIsNan(padfScanline[iPixel]) )
                    {
                        strcat(szHeader, ".0");
                        bHasOutputDecimalDot = TRUE;
                    }
                }

                osBuf += szHeader;
                if( (iPixel & 1023) == 0 || iPixel == nXSize - 1 )
                {
                    if ( VSIFWriteL( osBuf, (int)osBuf.size(), 1, fpImage ) != 1 )
                    {
                        eErr = CE_Failure;
                        CPLError( CE_Failure, CPLE_AppDefined, 
                                  "Write failed, disk full?\n" );
                        break;
                    }
                    osBuf = "";
                }
            }
        }
        if( VSIFWriteL( (void *) "\n", 1, 1, fpImage ) != 1 )
            eErr = CE_Failure;

        if( eErr == CE_None &&
            !pfnProgress((iLine + 1) / ((double) nYSize), NULL, pProgressData) )
        {
            eErr = CE_Failure;
            CPLError( CE_Failure, CPLE_UserInterrupt, 
                      "User terminated CreateCopy()" );
        }
    }

    CPLFree( panScanline );
    CPLFree( padfScanline );
    if( VSIFCloseL( fpImage ) != 0 )
        eErr = CE_Failure;

    if( eErr != CE_None )
        return NULL;

/* -------------------------------------------------------------------- */
/*      Try to write projection file.                                   */
/* -------------------------------------------------------------------- */
    const char  *pszOriginalProjection;

    pszOriginalProjection = (char *)poSrcDS->GetProjectionRef();
    if( !EQUAL( pszOriginalProjection, "" ) )
    {
        char                    *pszDirname, *pszBasename;
        char                    *pszPrjFilename;
        char                    *pszESRIProjection = NULL;
        VSILFILE                *fp;
        OGRSpatialReference     oSRS;

        pszDirname = CPLStrdup( CPLGetPath(pszFilename) );
        pszBasename = CPLStrdup( CPLGetBasename(pszFilename) );

        pszPrjFilename = CPLStrdup( CPLFormFilename( pszDirname, pszBasename, "prj" ) );
        fp = VSIFOpenL( pszPrjFilename, "wt" );
        if (fp != NULL)
        {
            oSRS.importFromWkt( (char **) &pszOriginalProjection );
            oSRS.morphToESRI();
            oSRS.exportToWkt( &pszESRIProjection );
            CPL_IGNORE_RET_VAL(VSIFWriteL( pszESRIProjection, 1, strlen(pszESRIProjection), fp ));

            CPL_IGNORE_RET_VAL(VSIFCloseL( fp ));
            CPLFree( pszESRIProjection );
        }
        else
        {
            CPLError( CE_Failure, CPLE_FileIO, 
                      "Unable to create file %s.\n", pszPrjFilename );
        }
        CPLFree( pszDirname );
        CPLFree( pszBasename );
        CPLFree( pszPrjFilename );
    }

/* -------------------------------------------------------------------- */
/*      Re-open dataset, and copy any auxiliary pam information.         */
/* -------------------------------------------------------------------- */

    /* If writing to stdout, we can't reopen it, so return */
    /* a fake dataset to make the caller happy */
    CPLPushErrorHandler(CPLQuietErrorHandler);
    GDALPamDataset* poDS = (GDALPamDataset*) GDALOpen(pszFilename, GA_ReadOnly);
    CPLPopErrorHandler();
    if (poDS)
    {
        poDS->CloneInfo( poSrcDS, GCIF_PAM_DEFAULT );
        return poDS;
    }

    CPLErrorReset();

    AAIGDataset* poAAIG_DS = new AAIGDataset();
    poAAIG_DS->nRasterXSize = nXSize;
    poAAIG_DS->nRasterYSize = nYSize;
    poAAIG_DS->nBands = 1;
    poAAIG_DS->SetBand( 1, new AAIGRasterBand( poAAIG_DS, 1 ) );
    return poAAIG_DS;
}

/************************************************************************/
/*                              OSR_GDS()                               */
/************************************************************************/

static CPLString OSR_GDS( char **papszNV, const char * pszField, 
                           const char *pszDefaultValue )

{
    int         iLine;

    if( papszNV == NULL || papszNV[0] == NULL )
        return pszDefaultValue;

    for( iLine = 0; 
         papszNV[iLine] != NULL && 
             !EQUALN(papszNV[iLine],pszField,strlen(pszField));
         iLine++ ) {}

    if( papszNV[iLine] == NULL )
        return pszDefaultValue;
    else
    {
        CPLString osResult;
        char    **papszTokens;

        papszTokens = CSLTokenizeString(papszNV[iLine]);

        if( CSLCount(papszTokens) > 1 )
            osResult = papszTokens[1];
        else
            osResult = pszDefaultValue;

        CSLDestroy( papszTokens );
        return osResult;
    }
}

/************************************************************************/
/*                        GDALRegister_AAIGrid()                        */
/************************************************************************/

void GDALRegister_AAIGrid()

{
    if( GDALGetDriverByName( "AAIGrid" ) != NULL )
        return;

    GDALDriver *poDriver = new GDALDriver();

    poDriver->SetDescription( "AAIGrid" );
    poDriver->SetMetadataItem( GDAL_DCAP_RASTER, "YES" );
    poDriver->SetMetadataItem( GDAL_DMD_LONGNAME, "Arc/Info ASCII Grid" );
    poDriver->SetMetadataItem( GDAL_DMD_HELPTOPIC,
                               "frmt_various.html#AAIGrid" );
    poDriver->SetMetadataItem( GDAL_DMD_EXTENSION, "asc" );
    poDriver->SetMetadataItem( GDAL_DMD_CREATIONDATATYPES,
                               "Byte UInt16 Int16 Int32 Float32" );

    poDriver->SetMetadataItem( GDAL_DCAP_VIRTUALIO, "YES" );
    poDriver->SetMetadataItem( GDAL_DMD_CREATIONOPTIONLIST,
"<CreationOptionList>\n"
"   <Option name='FORCE_CELLSIZE' type='boolean' description='Force use of CELLSIZE, default is FALSE.'/>\n"
"   <Option name='DECIMAL_PRECISION' type='int' description='Number of decimal when writing floating-point numbers(%f).'/>\n"
"   <Option name='SIGNIFICANT_DIGITS' type='int' description='Number of significant digits when writing floating-point numbers(%g).'/>\n"
"</CreationOptionList>\n" );
    poDriver->SetMetadataItem( GDAL_DMD_OPENOPTIONLIST,
"<OpenOptionLists>\n"
"   <Option name='DATATYPE' type='string-select' description='Data type to be used.'>\n"
"       <Value>Int32</Value>\n"
"       <Value>Float32</Value>\n"
"       <Value>Float64</Value>\n"
"   </Option>\n"
"</OpenOptionLists>\n" );

    poDriver->pfnOpen = AAIGDataset::Open;
    poDriver->pfnIdentify = AAIGDataset::Identify;
    poDriver->pfnCreateCopy = AAIGDataset::CreateCopy;

    GetGDALDriverManager()->RegisterDriver( poDriver );
}

/************************************************************************/
/*                   GDALRegister_GRASSASCIIGrid()                      */
/************************************************************************/

void GDALRegister_GRASSASCIIGrid()

{
    if( GDALGetDriverByName( "GRASSASCIIGrid" ) != NULL )
        return;

    GDALDriver *poDriver = new GDALDriver();

    poDriver->SetDescription( "GRASSASCIIGrid" );
    poDriver->SetMetadataItem( GDAL_DCAP_RASTER, "YES" );
    poDriver->SetMetadataItem( GDAL_DMD_LONGNAME, "GRASS ASCII Grid" );
    poDriver->SetMetadataItem( GDAL_DMD_HELPTOPIC,
                               "frmt_various.html#GRASSASCIIGrid" );

    poDriver->SetMetadataItem( GDAL_DCAP_VIRTUALIO, "YES" );

    poDriver->pfnOpen = GRASSASCIIDataset::Open;
    poDriver->pfnIdentify = GRASSASCIIDataset::Identify;

    GetGDALDriverManager()->RegisterDriver( poDriver );
}