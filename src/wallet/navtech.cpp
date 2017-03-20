#include "navtech.h"
#include "net.h"
#include "util.h"
#include "utilstrencodings.h"

#include <stdexcept>
#include <algorithm>
#include <sstream>
#include "rpc/server.h"
#include <stdio.h>
#include <iterator>
#include <openssl/aes.h>

using namespace std;

int padding = RSA_PKCS1_PADDING;
int encResultLength = 344;

static size_t CurlWriteResponse(void *contents, size_t size, size_t nmemb, void *userp) {
  ((string*)userp)->append((char*)contents, size * nmemb);
  return size * nmemb;
}

UniValue Navtech::CreateAnonTransaction(string address, CAmount nValue) {

  vector<anonServer> anonServers = this->GetAnonServers();
  UniValue serverData = this->FindAnonServer(anonServers, nValue);
  UniValue pubKey = find_value(serverData, "public_key");
  string encryptedAddress = this->EncryptAddress(address, pubKey.get_str());
  bool encryptionSuccess = this->TestEncryption(encryptedAddress, serverData);

  if (encryptionSuccess) {
    UniValue addresses = find_value(serverData, "nav_addresses");
    UniValue addrArray = addresses.get_array();

    UniValue navtechData;
    navtechData.setObject();
    navtechData.pushKV("anondestination", encryptedAddress);
    navtechData.pushKV("anonaddress", addrArray[0].get_str());
    navtechData.pushKV("anonfee", find_value(serverData, "transaction_fee"));
    return navtechData;
  } else {
    throw runtime_error("Unable to send NAVTech transaction, please try again");
  }

}

vector<anonServer> Navtech::GetAnonServers() {

  vector<anonServer> returnServers;

  if (vAddedAnonServers.size() < 1 && mapMultiArgs["-addanonserver"].size() < 1) {
      throw runtime_error("You must have at least one NAVTech server added to your conf file or by rpc command");
      return returnServers;
  }

  vector<string> anonServers = {};

  vector<string> confAnonServers = mapMultiArgs["-addanonserver"];

  BOOST_FOREACH(string confAnonServer, confAnonServers) {
      anonServers.push_back(confAnonServer);
  }

  BOOST_FOREACH(string vAddedAnonServer, vAddedAnonServers) {
      anonServers.push_back(vAddedAnonServer);
  }

  BOOST_FOREACH(string currentServer, anonServers) {
      anonServer tempServer;

      string::size_type portPos;
      portPos = currentServer.find(':');

      if (portPos == std::string::npos) {
        tempServer.address = currentServer;
        tempServer.port = "443";
      } else {
        tempServer.address = currentServer.substr(0, portPos);
        tempServer.port = currentServer.substr(portPos + 1);
      }

      size_t periodCount = count(tempServer.address.begin(), tempServer.address.end(), '.');

      if(periodCount == 3 && stoi(tempServer.port) > 0) {
        returnServers.push_back(tempServer);
      }
  }

  if (returnServers.size() < 1) {
      throw runtime_error("The anon servers you have added are invalid");
      return returnServers;
  }

  return returnServers;
}

UniValue Navtech::FindAnonServer(std::vector<anonServer> anonServers, CAmount nValue) {
  if (anonServers.size() < 1) {
      throw runtime_error("Tried all available servers");
  }

  int randIndex = rand() % anonServers.size();

  string readBuffer;

  curl_global_init(CURL_GLOBAL_ALL);
  curl = curl_easy_init();

  if (curl) {

    string serverURL = "https://" + anonServers[randIndex].address + ":" + anonServers[randIndex].port + "/api/check-node";

    curl_easy_setopt(curl, CURLOPT_URL, serverURL.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "num_addresses=1");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
      LogPrintf("CURL failed to contact server %s:%s\n", anonServers[randIndex].address, anonServers[randIndex].port);
      anonServers.erase(anonServers.begin()+randIndex);
      curl_easy_cleanup(curl);
      curl_global_cleanup();
      return this->FindAnonServer(anonServers, nValue);
    } else {
      UniValue parsedResponse = this->ParseJSONResponse(readBuffer);
      UniValue type = find_value(parsedResponse, "type");
      UniValue data = find_value(parsedResponse, "data").get_obj();

      if (type.get_str() != "SUCCESS") {
          LogPrintf("Server retured bad response %s:%s\n", anonServers[randIndex].address, anonServers[randIndex].port);
          anonServers.erase(anonServers.begin()+randIndex);
          return this->FindAnonServer(anonServers, nValue);
      }
      if (nValue != -1) {
        UniValue maxAmount = find_value(data, "max_amount");
        UniValue minAmount = find_value(data, "min_amount");
        int satoshiFactor = 100000000;

        if (nValue/satoshiFactor > maxAmount.get_int() || nValue/satoshiFactor < minAmount.get_int()) {
          LogPrintf("Transaction amount outside of specified range min_amount=%i max_amount=%i value=%i\n", minAmount.get_int(), maxAmount.get_int(), nValue/satoshiFactor);
          anonServers.erase(anonServers.begin()+randIndex);
          curl_easy_cleanup(curl);
          curl_global_cleanup();
          return this->FindAnonServer(anonServers, nValue);
        }
        curl_easy_cleanup(curl);
        curl_global_cleanup();
        return data;
      } else {
        curl_easy_cleanup(curl);
        curl_global_cleanup();
        return data;
      }

    }
  } else {
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    throw runtime_error("CURL unavailable");
  }
}

UniValue Navtech::ParseJSONResponse(string readBuffer) {
  try {
      UniValue valRequest;
      if (!valRequest.read(readBuffer))
        throw JSONRPCError(RPC_PARSE_ERROR, "Parse error");

      if (valRequest.isObject()) {
        const UniValue& request = valRequest.get_obj();
        return request;

      } else {
        throw JSONRPCError(RPC_PARSE_ERROR, "Top-level object parse error");
      }

  } catch (const UniValue& objError) {
    throw runtime_error("ParseJSONResponse objError");
  } catch (const std::exception& e) {
    throw runtime_error("ParseJSONResponse exception");
  }
}

string Navtech::EncryptAddress(string address, string pubKeyStr) {

  unsigned char pubKeyChar[(int)pubKeyStr.length()+1];
  memcpy(pubKeyChar, pubKeyStr.c_str(), pubKeyStr.length());
  pubKeyChar[pubKeyStr.length()] = 0;

  unsigned char addressChar[(int)address.length()+1];
  memcpy(addressChar, address.c_str(), address.length());
  addressChar[address.length()] = 0;

  unsigned char encrypted[4098]={};

  int encryptedLength = this->PublicEncrypt(addressChar, (int)address.length(), pubKeyChar, encrypted);

  if(encryptedLength == -1)
  {
    throw runtime_error("Encryption failed");
  } else {
    string encoded = EncodeBase64(encrypted, encryptedLength);
    return encoded;
  }
}

int Navtech::PublicEncrypt(unsigned char* data, int data_len, unsigned char* key, unsigned char* encrypted)
{
    RSA * rsa = this->CreateRSA(key,1);
    int result = RSA_public_encrypt(data_len, data, encrypted, rsa,padding);
    return result;
}

RSA * Navtech::CreateRSA(unsigned char * key,int isPublic)
{
    RSA *rsa= NULL;
    BIO *keybio ;
    keybio = BIO_new_mem_buf(key, -1);
    if (keybio==NULL)
    {
        // printf( "Failed to create key BIO\n");
        return 0;
    }
    if(isPublic)
    {
        rsa = PEM_read_bio_RSA_PUBKEY(keybio, &rsa,NULL, NULL);
        // printf( "Created RSA from Public Key\n");
    }
    else
    {
        rsa = PEM_read_bio_RSAPrivateKey(keybio, &rsa,NULL, NULL);
        // printf( "Created RSA from Private Key\n");
    }
    if(rsa == NULL)
    {
        // printf( "Failed to create RSA\n");
    }

    return rsa;
}

bool Navtech::TestEncryption(string encrypted, UniValue serverData) {

  UniValue server = find_value(serverData, "server");
  UniValue port = find_value(serverData, "server_port");

  string readBuffer;

  curl_global_init(CURL_GLOBAL_ALL);
  curl = curl_easy_init();

  if (curl) {

    string serverURL = "https://" + server.get_str() + ":" + to_string(port.get_int()) + "/api/test-decryption";

    char* escapedEncrypted = curl_easy_escape(curl, encrypted.c_str(), 0);
    string encryptedData = string("encrypted_data=");
    encryptedData.append(escapedEncrypted);

    curl_easy_setopt(curl, CURLOPT_URL, serverURL.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, encryptedData.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
      LogPrintf("CURL failed to contact server %s:%s/api/test-decryption\n", server.get_str(), port.get_str());
    } else {
      UniValue parsedResponse = this->ParseJSONResponse(readBuffer);
      UniValue type = find_value(parsedResponse, "type");

      if (type.get_str() != "SUCCESS") {
          LogPrintf("Server unable to decrypt address %s:%s\n", server.get_str(), port.get_str());
          throw runtime_error("Encryption failed");
          return false;
      } else {
        LogPrintf("Decryption test successful\n");
        return true;
      }
    }
    curl_easy_cleanup(curl);
  } else {
    throw runtime_error("CURL unavailable");
  }
  curl_global_cleanup();
  throw runtime_error("TestEncryption End of Function");
}
