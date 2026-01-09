#pragma once

#ifdef __cplusplus
extern "C" {
#endif

	void R_Sensor_Init( void );
	void R_Sensor_Shutdown( void );
	void R_Sensor_SetEnabled( int enabled );
	void R_Sensor_CaptureFrame( void );

	const unsigned char *R_Sensor_GetMask( int *w, int *h );
	const float *R_Sensor_GetDepth( int *w, int *h );

#ifdef __cplusplus
}
#endif
