#pragma once

#ifdef _WIN32
#define SENSOR_EXPORT __declspec(dllexport)
#define SENSOR_IMPORT __declspec(dllimport)
#else
#define SENSOR_EXPORT __attribute__((visibility("default")))
#define SENSOR_IMPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif
	void R_DrawWorld_Sensor( void );
	void R_Sensor_Init( void );
	void R_Sensor_Shutdown( void );
	SENSOR_EXPORT void R_Sensor_SetEnabled( int enabled );
	void R_Sensor_CaptureFrame( void );

	SENSOR_EXPORT const unsigned char *R_Sensor_GetMask( int *w, int *h );
	SENSOR_EXPORT const float *R_Sensor_GetDepth( int *w, int *h );

#ifdef __cplusplus
}
#endif
