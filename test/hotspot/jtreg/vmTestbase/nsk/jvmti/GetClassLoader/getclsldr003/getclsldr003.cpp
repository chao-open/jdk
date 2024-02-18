/*
 * Copyright (c) 2003, 2024, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include <stdio.h>
#include <string.h>
#include "jvmti.h"
#include "agent_common.h"
#include "JVMTITools.h"

extern "C" {


#define PASSED 0
#define STATUS_FAILED 2

static jvmtiEnv *jvmti = nullptr;
static jint result = PASSED;
static jboolean printdump = JNI_FALSE;

#ifdef STATIC_BUILD
JNIEXPORT jint JNICALL Agent_OnLoad_getclsldr003(JavaVM *jvm, char *options, void *reserved) {
    return Agent_Initialize(jvm, options, reserved);
}
JNIEXPORT jint JNICALL Agent_OnAttach_getclsldr003(JavaVM *jvm, char *options, void *reserved) {
    return Agent_Initialize(jvm, options, reserved);
}
JNIEXPORT jint JNI_OnLoad_getclsldr003(JavaVM *jvm, char *options, void *reserved) {
    return JNI_VERSION_1_8;
}
#endif
jint Agent_Initialize(JavaVM *jvm, char *options, void *reserved) {
    jint res;

    if (options != nullptr && strcmp(options, "printdump") == 0) {
        printdump = JNI_TRUE;
    }

    res = jvm->GetEnv((void **) &jvmti, JVMTI_VERSION_1_1);
    if (res != JNI_OK || jvmti == nullptr) {
        printf("Wrong result of a valid call to GetEnv!\n");
        return JNI_ERR;
    }

    return JNI_OK;
}

JNIEXPORT void JNICALL
Java_nsk_jvmti_GetClassLoader_getclsldr003_check(JNIEnv *env,
        jclass cls, jclass clazz, jobject cl) {
    jvmtiError err;
    jobject classloader;
    char *sig, *generic;

    if (jvmti == nullptr) {
        printf("JVMTI client was not properly loaded!\n");
        result = STATUS_FAILED;
        return;
    }

    err = jvmti->GetClassSignature(clazz, &sig, &generic);
    if (err != JVMTI_ERROR_NONE) {
        printf("(GetClassSignature) unexpected error: %s (%d)\n",
               TranslateError(err), err);
        result = STATUS_FAILED;
        return;
    }

    err = jvmti->GetClassLoader(clazz, &classloader);
    if (err != JVMTI_ERROR_NONE) {
        printf("(GetClassLoader) unexpected error: %s (%d)\n",
               TranslateError(err), err);
        result = STATUS_FAILED;
        return;
    }

    if (printdump == JNI_TRUE) {
        printf(">>> %s - 0x%p\n", sig, classloader);
    }

    if (env->IsSameObject(classloader, cl) != JNI_TRUE) {
        printf("(%s) unexpected class loader\n", sig);
        result = STATUS_FAILED;
    }
}

JNIEXPORT int JNICALL
Java_nsk_jvmti_GetClassLoader_getclsldr003_getRes(JNIEnv *env, jclass cls) {
    return result;
}

}
