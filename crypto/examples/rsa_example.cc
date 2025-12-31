/*************************************************************************
    > File Name: rsa_example.cc
    > Author: hsz
    > Brief:
    > Created Time: 2025年07月15日 星期二 11时36分56秒
 ************************************************************************/

#include <stdio.h>
#include <string.h>

#include <rsa.h>
#include <sha.h>

using namespace eular;

int main(int argc, char **argv)
{
    crypto::Rsa rsa;

    std::string publicKey;
    std::string privateKey;
    int32_t status = crypto::Rsa::GenerateRSAKey(publicKey, privateKey);
    printf("GenerateRSAKey status: %d\n", status);
    printf("Public Key:\n%s\n", publicKey.c_str());
    printf("Private Key:\n%s\n", privateKey.c_str());

    status = rsa.initRSAKey(publicKey, privateKey);
    printf("RSA key initialization status: %d\n", status);

    std::string message = "Hello, RSA!";
    std::vector<uint8_t> encryptedMessage;
    std::string decryptedMessage;

    status = rsa.publicEncrypt(message, encryptedMessage);
    printf("Public encryption status: %d\n", status);
    status = rsa.privateDecrypt(encryptedMessage, decryptedMessage);
    printf("Private decryption status: %d\n", status);
    if (decryptedMessage == message) {
        printf("Encryption and decryption successful!\n");
    } else {
        printf("Encryption and decryption failed!\n");
    }

    printf("\n");

    crypto::SHA sha256;
    sha256.init(crypto::SHA::SHA_256);
    sha256.update(message);
    std::vector<uint8_t> hashVec;
    sha256.finalize(hashVec);

    rsa.setHashMode(crypto::Rsa::MT_SHA256);
    std::vector<uint8_t> signatureVec;
    status = rsa.sign(hashVec, signatureVec);
    status = rsa.verifySignature(signatureVec, hashVec);
    printf("Verification status: %d\n", status);
    if (status == 0) {
        printf("Signature and verification successful!\n");
    } else {
        printf("Signature and verification failed!\n");
    }

    return 0;
}
