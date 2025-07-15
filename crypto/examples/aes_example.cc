/*************************************************************************
    > File Name: aes_example.cc
    > Author: hsz
    > Brief:
    > Created Time: 2025年07月15日 星期二 11时05分32秒
 ************************************************************************/

#include <stdio.h>
#include <aes.h>

using namespace eular;

void aes_example()
{
    crypto::AES aes;
    std::string key = "0123456789";
    std::string plaintext = "Hello, World!Hello, World!Hello, World!Hello, World!Hello, World!Hello, World!Hello, World!Hello, World!";
    aes.setKey(key);
    aes.setMode(crypto::AES::ECB);

    // Encrypt the plaintext
    std::vector<uint8_t> ciphertext = aes.encrypt(plaintext);

    // Decrypt the ciphertext
    std::vector<uint8_t> decrypted_text = aes.decrypt(ciphertext);

    std::string decrypted_string(decrypted_text.begin(), decrypted_text.end());
    if (decrypted_string == plaintext) {
        printf("AES encryption and decryption successful!\n");
    } else {
        printf("AES encryption and decryption failed!\n");
    }
}

int main(int argc, char **argv)
{
    aes_example();
    return 0;
}
