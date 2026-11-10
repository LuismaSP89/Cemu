#pragma once
#include "UsbBluetoothProtocol.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

namespace UsbBluetooth {
inline std::filesystem::path AdapterBondPath(const std::filesystem::path& legacy, const Address& host) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto byte : host) { hash ^= byte; hash *= 1099511628211ull; }
    std::string suffix(16, '0');
    for (unsigned i = 0; i < 16; ++i) { suffix[15 - i] = "0123456789abcdef"[hash & 15]; hash >>= 4; }
    return legacy.parent_path() / ("usb-bluetooth-bonds-" + suffix + ".bin");
}
inline Bytes EncodeBonds(const Address& host, const std::vector<Bond>& bonds) {
    if(bonds.size()>Session::MaxRemotes) return {};
    Bytes data{'C','W','B','T',1}; data.insert(data.end(),host.begin(),host.end());data.push_back(static_cast<std::uint8_t>(bonds.size()));
    std::vector<Address> seen;
    for(const auto& b:bonds) {
        if(!IsWiiName(b.name) || b.address==Address{} || b.address==host || std::find(seen.begin(),seen.end(),b.address)!=seen.end()) return {};
        seen.push_back(b.address);data.insert(data.end(),b.address.begin(),b.address.end());
        data.push_back(b.name=="Nintendo RVL-CNT-01-TR"?1:0);data.insert(data.end(),b.key.begin(),b.key.end());
    }
    return data;
}
inline std::optional<std::vector<Bond>> DecodeBonds(std::span<const std::uint8_t> data,const Address& host) {
    if(data.size()<12 || !std::equal(data.begin(),data.begin()+5,Bytes{'C','W','B','T',1}.begin()) ||
        ReadAddress(data,5)!=host || data[11]>Session::MaxRemotes || data.size()!=12+23u*data[11]) return {};
    std::vector<Bond> result;
    for(unsigned i=0;i<data[11];++i) {
        const auto pos=12+i*23;
        if(data[pos+6]>1) return {};
        Bond b;b.address=ReadAddress(data,pos);b.name=data[pos+6]?"Nintendo RVL-CNT-01-TR":"Nintendo RVL-CNT-01";
        std::copy_n(data.begin()+pos+7,16,b.key.begin());result.push_back(b);
    }
    if(EncodeBonds(host,result).empty()) return {};
    return result;
}
#ifdef _WIN32
// User-scoped authenticated Windows encryption. Never store plaintext link keys.
inline bool SaveBonds(const std::filesystem::path& path,const Address& host,const std::vector<Bond>& bonds) {
    auto plain=EncodeBonds(host,bonds);if(plain.empty()) return false;
    DATA_BLOB input{static_cast<DWORD>(plain.size()),plain.data()},encrypted{};
    const bool ok=CryptProtectData(&input,L"Cemu dedicated Wiimote bonds",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&encrypted)!=FALSE;
    SecureZeroMemory(plain.data(),plain.size());if(!ok) return false;
    const auto temp=std::filesystem::path(path.wstring()+L".tmp");
    bool written=false;
    {
        std::ofstream file(temp,std::ios::binary|std::ios::trunc);
        file.write(reinterpret_cast<const char*>(encrypted.pbData),encrypted.cbData);file.flush();written=file.good();
    }
    LocalFree(encrypted.pbData);
    if(!written) return false;
    return MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
}
inline std::optional<std::vector<Bond>> LoadBonds(const std::filesystem::path& path,const Address& host) {
    std::error_code error;
    if(!std::filesystem::exists(path,error) && !error) return std::vector<Bond>{};
    const auto size=std::filesystem::file_size(path,error);if(error || size>16384 || !size) return {};
    std::ifstream file(path,std::ios::binary);Bytes sealed{std::istreambuf_iterator<char>(file),{}};
    if(sealed.size()!=size) return {};
    DATA_BLOB input{static_cast<DWORD>(sealed.size()),sealed.data()},plain{};
    if(!CryptUnprotectData(&input,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&plain)) return {};
    const auto result=DecodeBonds(std::span(plain.pbData,plain.cbData),host);
    SecureZeroMemory(plain.pbData,plain.cbData);LocalFree(plain.pbData);return result;
}
inline std::optional<std::vector<Bond>> LoadAdapterBonds(const std::filesystem::path& legacy, const Address& host) {
    const auto scoped = AdapterBondPath(legacy, host);
    std::error_code error;
    const bool exists = std::filesystem::exists(scoped, error);
    if (error) return {};
    if (exists) return LoadBonds(scoped, host);
    // Import only matching legacy identities. Never overwrite another adapter's
    // legacy cache; a newly selected adapter starts with its own empty cache.
    if (auto imported = LoadBonds(legacy, host)) return imported;
    return std::vector<Bond>{};
}
#endif
} // namespace UsbBluetooth
