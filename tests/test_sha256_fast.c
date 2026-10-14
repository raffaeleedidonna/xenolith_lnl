#define _POSIX_C_SOURCE 200809L
#include "../format.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do {if(!(x)){fprintf(stderr,"sha256 fast line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static unsigned char data[1024 * 1024 + 128];
static int expected = -1;
static int compare(size_t start, size_t size) {
    uint8_t reference[32], output[34]; memset(output,0xa5,sizeof output);
    format_sha256_bytes(data+start,size,reference);
    format_sha256_backend backend = format_sha256_bytes_fast(data+start,size,output+1);
    if(expected>=0 && (int)backend!=expected) return 0;
    if(output[0]!=0xa5||output[33]!=0xa5)return 0;
    if(backend==FORMAT_SHA256_OPENSSL)return !memcmp(reference,output+1,32);
    for(int i=1;i<=32;i++)if(output[i]!=0xa5)return 0;
    return backend==FORMAT_SHA256_UNAVAILABLE||backend==FORMAT_SHA256_FAILED||backend==FORMAT_SHA256_DISABLED;
}
static void *worker(void *arg) {
    int *ok=arg;*ok=1;
    for(int i=0;i<20;i++)*ok &= compare(1,(size_t)(55+i));
    return NULL;
}
int main(int argc,char **argv) {
    if(argc==2)expected=atoi(argv[1]);else CHECK(argc==1);
    for(size_t i=0;i<sizeof data;i++)data[i]=(unsigned char)((i*131)^(i>>8));
    /* First accelerated calls race initialization, not already-initialized code. */
    pthread_t threads[6];int ok[6];
    for(int i=0;i<6;i++)CHECK(!pthread_create(threads+i,NULL,worker,ok+i));
    for(int i=0;i<6;i++){CHECK(!pthread_join(threads[i],NULL));CHECK(ok[i]);}
    size_t sizes[]={0,1,2,3,55,56,57,63,64,65,127,128,129,4095,4096,4097,1024*1024+3};
    for(size_t i=0;i<sizeof sizes/sizeof sizes[0];i++){CHECK(compare(0,sizes[i]));CHECK(compare(1,sizes[i]));}
    const char *mode=getenv("XENOLITH_WEIGHTS_VERIFY");char *saved=mode?strdup(mode):NULL;
    CHECK(!mode||saved);CHECK(!setenv("XENOLITH_WEIGHTS_VERIFY","metadata",1));
    uint8_t untouched[32];memset(untouched,0xa5,sizeof untouched);
    CHECK(format_sha256_bytes_fast((const void *)1,SIZE_MAX,untouched)==FORMAT_SHA256_DISABLED);
    for(int i=0;i<32;i++)CHECK(untouched[i]==0xa5); /* No full-file fallback read. */
    if(saved){CHECK(!setenv("XENOLITH_WEIGHTS_VERIFY",saved,1));free(saved);}else CHECK(!unsetenv("XENOLITH_WEIGHTS_VERIFY"));
    puts("sha256 fast: exact bytes, boundaries, unaligned inputs, initialization and no-scan fallback PASS");return 0;
}
