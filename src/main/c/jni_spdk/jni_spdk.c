#include "jni_spdk.h"
#include "spdk_app.h"

static const JNINativeMethod methods[] = {
	{ "spdkIdentify", "()I", (void *)spdkIdentify },
};

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *jvm, void *reserved)
{
	JNIEnv *env = NULL;

//	if (jvm->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK)
	if ((*jvm)->GetEnv(jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
		return JNI_ERR;
	}

//	if (env->RegisterNatives(env->FindClass("Lac/ncic/syssw/jni/ubmk/JniUbmk;"),
	if ((*env)->RegisterNatives(env,
	                            (*env)->FindClass(env, "Lac/ncic/syssw/jni/spdk/Identify;"),
	                            methods,
	                            sizeof(methods) / sizeof(methods[0])
	                           ) < -1) {
		return JNI_ERR;
	}

	return JNI_VERSION_1_6;
}

//JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *jvm, void *reserved)
//{
//}

JNIEXPORT jint JNICALL identify(JNIEnv *env, jobject thisObj)
{
	return spdk_identity();
}

