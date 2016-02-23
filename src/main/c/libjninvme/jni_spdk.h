#ifndef _JNI_SPDK_H_
#define _JNI_SPDK_H_

#include <jni.h>

#ifdef __cplusplus
extern "C" {
#endif

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *, void *);
//JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *, void *);

JNIEXPORT int JNICALL spdkIdentify(JNIEnv *, jobject);

#ifdef __cplusplus
}
#endif

#endif  /* _JNI_SPDK_H_ */
