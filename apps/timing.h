#ifndef __TIMING__
#define __TIMING__

#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

double stamp_;
double getusec_() {
        struct timeval time;
        gettimeofday(&time, 0);
        return ((double)time.tv_sec * (double)1e6 + (double)time.tv_usec);
}

#define START_COUNT_TIME stamp_=getusec_();
#define STOP_COUNT_TIME(_m) stamp_=getusec_()-stamp_;\
                        stamp_=stamp_/1e6;\
                        printf ("%s:%0.6fs\n",(_m), stamp_);


#ifdef __cplusplus
}
#endif


#endif //__TIMING__
