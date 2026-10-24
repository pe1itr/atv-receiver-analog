#include "driver_status.h"
#include <SoapySDR/Modules.h>
#include <string.h>

void driver_status(int installed[4])
{
    /* UHD and SoapySDR are linked dependencies. Pluto is an optional build. */
#ifdef ATV_NO_UHD
    installed[0]=0;
#else
    installed[0]=1;
#endif
    installed[1]=0;
    installed[3]=0;
#ifdef HAVE_LIBIIO
    installed[2]=1;
#else
    installed[2]=0;
#endif
    SoapySDR_loadModules();
    size_t count=0;
    char **modules=SoapySDR_listModules(&count);
    for(size_t i=0;i<count;++i) {
        SoapySDRKwargs result=SoapySDR_getLoaderResult(modules[i]);
        for(size_t j=0;j<result.size;++j) {
            if(!strcmp(result.keys[j],"hackrf") && !*result.vals[j]) installed[1]=1;
            if(!strcmp(result.keys[j],"sdrplay") && !*result.vals[j]) installed[3]=1;
        }
        SoapySDRKwargs_clear(&result);
    }
    SoapySDRStrings_clear(&modules,count);
}
