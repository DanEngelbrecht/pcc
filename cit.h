#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
	
typedef struct CIT_private CIT;

typedef struct WaitCallback_private WaitCallback;
typedef struct WakeCallback_private WakeCallback;

typedef void (*Wait)(CIT* cit, WaitCallback* callback);
typedef void (*Wake)(CIT* cit, WakeCallback* callback);

struct WaitCallback_private {
    Wait wait;
};

struct WakeCallback_private {
    Wake wake;
};

CIT* cit_create(uint32_t channel_size, size_t item_size, WaitCallback* waitPop, WakeCallback* wakePop, WaitCallback* waitPush, WakeCallback* wakePush);

void cit_push(CIT* cit, const void* item);
int cit_trypush(CIT* cit, const void* item);
void cit_pop(CIT* cit, void* item);
int cit_trypop(CIT* cit, void* item);
void cit_close(CIT* cit);

#ifdef __cplusplus
}
#endif
