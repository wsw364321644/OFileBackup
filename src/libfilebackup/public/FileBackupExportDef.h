#pragma once
#ifdef __cplusplus
#if defined( _WIN32 ) && !defined( _X360 )
#if defined( LIB_FILEBACKUP_API_EXPORTS )
#define LIB_FILEBACKUP_EXPORT __declspec( dllexport ) 
#elif defined( LIB_FILEBACKUP_API_NODLL )
#define LIB_FILEBACKUP_EXPORT 
#else
#define LIB_FILEBACKUP_EXPORT  __declspec( dllimport ) 
#endif 
#elif defined( GNUC )
#if defined( LIB_FILEBACKUP_API_EXPORTS )
#define LIB_FILEBACKUP_EXPORT  __attribute__ ((visibility("default"))) 
#else
#define LIB_FILEBACKUP_EXPORT 
#endif 
#else // !WIN32
#if defined( LIB_FILEBACKUP_API_EXPORTS )
#define LIB_FILEBACKUP_EXPORT 
#else
#define LIB_FILEBACKUP_EXPORT 
#endif 
#endif
#else
#define LIB_FILEBACKUP_EXPORT 
#endif