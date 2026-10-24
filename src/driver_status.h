#ifndef DRIVER_STATUS_H
#define DRIVER_STATUS_H
/* In wizard order: B210/UHD, HackRF/SoapyHackRF, Pluto/libiio, SDRplay/SoapySDRPlay3.
 * Check usable driver support without enumerating or opening SDR hardware. */
void driver_status(int installed[4]);
#endif
