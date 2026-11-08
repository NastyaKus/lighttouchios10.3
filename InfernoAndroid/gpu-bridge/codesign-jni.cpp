/* SPDX-License-Identifier: AGPL-3.0-only
 * JNI facade for pinned Procursus ldid. No signing credentials: ad-hoc only.
 * Preserve the supplied original entitlements; no broadening is performed. */
#include <jni.h>
#include <exception>
#include <sstream>
#include <vector>
#include "ldid.hpp"
namespace {
class Adhoc final:public ldid::Signer {
public:
    operator EVP_PKEY *() const override {return nullptr;}
    operator X509 *() const override {return nullptr;}
    operator STACK_OF(X509) *() const override {return nullptr;}
    operator bool() const override {return false;}
};
class Quiet final:public ldid::Progress {
public:
    void operator()(const std::string &) const override {}
    void operator()(double) const override {}
};
std::vector<unsigned char> bytes(JNIEnv *env,jbyteArray input,int maximum) {
    if(!input)throw std::runtime_error("Missing signing input");
    auto size=env->GetArrayLength(input);if(size<0 || size>maximum)throw std::runtime_error("Signing input bounds");
    std::vector<unsigned char> data(size);env->GetByteArrayRegion(input,0,size,reinterpret_cast<jbyte *>(data.data()));
    if(env->ExceptionCheck())throw std::runtime_error("Cannot copy signing input");return data;
}
void fail(JNIEnv *env,const char *message) {
    if(!env->ExceptionCheck())env->ThrowNew(env->FindClass("java/lang/IllegalStateException"),message);
}
}
extern "C" JNIEXPORT jbyteArray JNICALL Java_dev_embervm_android_SystemCodeSigner_nativeSign(
    JNIEnv *env,jclass,jbyteArray image,jbyteArray entitlements,jstring identifier,jint flags,jint platform) {
    try {
        auto code=bytes(env,image,64<<20),xml=bytes(env,entitlements,1<<20);
        if(code.size()<32 || !identifier || env->GetStringUTFLength(identifier)>512 || platform<0 || platform>255)
            throw std::runtime_error("Invalid signing metadata");
        const char *value=env->GetStringUTFChars(identifier,nullptr);if(!value)throw std::runtime_error("Identifier allocation");
        std::string name(value);env->ReleaseStringUTFChars(identifier,value);
        std::string entitlement_text(xml.begin(),xml.end());std::stringbuf output;Adhoc signer;Quiet progress;
        ldid::Sign(code.data(),code.size(),output,name,entitlement_text,false,"",signer,{},static_cast<uint32_t>(flags)|2u,static_cast<uint8_t>(platform),progress);
        auto signed_code=output.str();if(signed_code.size()>65u<<20)throw std::runtime_error("Signed image bounds");
        jbyteArray result=env->NewByteArray(static_cast<jsize>(signed_code.size()));if(!result)return nullptr;
        env->SetByteArrayRegion(result,0,static_cast<jsize>(signed_code.size()),reinterpret_cast<const jbyte *>(signed_code.data()));
        return result;
    } catch(const std::exception &error){fail(env,error.what());}
      catch(const char *error){fail(env,error);}
      catch(...){fail(env,"Native signing failed");}
    return nullptr;
}
