/***************************************************************
* StreamDevice Support                                         *
*                                                              *
* (C) 1999 Dirk Zimoch (zimoch@delta.uni-dortmund.de)          *
* (C) 2005 Dirk Zimoch (dirk.zimoch@psi.ch)                    *
*                                                              *
* This header defines the format stucture used to interface    *
* format converters and record interfaces to StreamDevice      *
* Please refer to the HTML files in ../doc/ for a detailed     *
* documentation.                                               *
*                                                              *
* If you do any changes in this file, you are not allowed to   *
* redistribute it any more. If there is a bug or a missing     *
* feature, send me an email and/or your patch. If I accept     *
* your changes, they will go to the next release.              *
*                                                              *
* DISCLAIMER: If this software breaks something or harms       *
* someone, it's your problem.                                  *
*                                                              *
***************************************************************/

#ifndef StreamFormat_h
#define StreamFormat_h

typedef enum {
    left_flag  = 0x01,
    sign_flag  = 0x02,
    space_flag = 0x04,
    alt_flag   = 0x08,
    zero_flag  = 0x10,
    skip_flag  = 0x20
} StreamFormatFlag;

typedef enum {
    long_format   = 1,
    enum_format   = 2,
    double_format = 3,
    string_format = 4,
    pseudo_format = 5
} StreamFormatType;

typedef struct StreamFormat
{
    char conv;
    StreamFormatType type;
    unsigned char flags;
    signed short prec;
    unsigned short width;
    unsigned short infolen;
#ifdef __cplusplus
    const char* info() const
        { return reinterpret_cast<const char*>(this+1);}
#endif
} StreamFormat;

/*
   In C use this macro instead of info() member but be careful
   not to use it on anything but StreamFormat structures
*/
#define StreamFormatInfoString(f) ((const char*)(&(f)+1))

#endif
