#pragma once
#ifdef _WIN32
#include <Windows.h>
#include "input/api/Wiimote/WiimoteAudioWriteQueue.h"
#include <stdexcept>
#include <string>

class WindowsSpeakerWriter
{
    struct File
    {
        HANDLE handle=INVALID_HANDLE_VALUE;
        explicit File(const std::string& path)
        {
            const auto count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path.c_str(),-1,nullptr,0);
            if(count<=0)throw std::runtime_error("Invalid speaker HID path");
            std::wstring wide(count,L'\0');
            if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path.c_str(),-1,wide.data(),count))throw std::runtime_error("Invalid speaker HID path");
            handle=CreateFileW(wide.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
            if(handle==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot open queued speaker HID output");
        }
        ~File(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
    };
    struct Writer
    {
        struct Token
        {
            OVERLAPPED request{};
            bool immediate=false;
            DWORD error=0;
            Token(){request.hEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!request.hEvent)throw std::runtime_error("Cannot create speaker completion event");}
            ~Token(){CloseHandle(request.hEvent);}
            Token(const Token&)=delete;
            Token& operator=(const Token&)=delete;
        };
        HANDLE handle;
        std::uint64_t attempted=0,completed=0,failed=0;
        bool Begin(Token& token,std::span<const std::uint8_t> bytes) noexcept
        {
            const auto event=token.request.hEvent;token.request={};token.request.hEvent=event;
            ResetEvent(event);token.error=0;++attempted;
            const bool written=WriteFile(handle,bytes.data(),static_cast<DWORD>(bytes.size()),nullptr,&token.request)!=FALSE;
            const auto error=written?ERROR_SUCCESS:GetLastError();
            token.immediate=written || error!=ERROR_IO_PENDING;
            if(!written && error!=ERROR_IO_PENDING){token.error=error;++failed;return false;}
            return true;
        }
        bool Finish(Token& token,std::size_t wanted) noexcept
        {
            DWORD count=0;
            if(!token.immediate)
            {
                const auto status=WaitForSingleObject(token.request.hEvent,1000);
                if(status!=WAIT_OBJECT_0)
                {
                    token.error=status==WAIT_TIMEOUT?ERROR_TIMEOUT:GetLastError();
                    CancelIoEx(handle,&token.request);
                    GetOverlappedResult(handle,&token.request,&count,TRUE);
                }
            }
            if(!token.error && !GetOverlappedResult(handle,&token.request,&count,FALSE))token.error=GetLastError();
            const bool valid=token.error==0 && count==wanted;
            if(valid)++completed;else ++failed;
            return valid;
        }
    };
    File m_file;
    Writer m_writer;
    WiimoteAudioWriteQueue<Writer> m_queue;
public:
    explicit WindowsSpeakerWriter(const std::string& path):m_file(path),m_writer{m_file.handle},m_queue(m_writer){}
    bool Submit(std::span<const std::uint8_t> bytes){return m_queue.Submit(bytes);}
    bool Drain() noexcept{return m_queue.Drain();}
    std::uint64_t Attempted()const{return m_writer.attempted;}
    std::uint64_t Completed()const{return m_writer.completed;}
    std::uint64_t Failed()const{return m_writer.failed;}
};
#endif
