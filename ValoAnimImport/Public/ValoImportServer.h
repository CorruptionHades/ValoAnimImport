#pragma once
#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/ThreadSafeBool.h"

class FSocket;

/**
 * Line-based JSON TCP server on 127.0.0.1:8765.
 *   {"cmd":"ping"}
 *   {"cmd":"import","file":"/abs/path.psa","package":"/Game/Folder","name":"Asset","skeleton":"/Game/..."}
 *   {"cmd":"import_batch","files":["a.psa","b.ueanim"],"package":"/Game/Folder","skeleton":"/Game/..."}
 */
class FValoImportServer : public FRunnable
{
public:
    static const int32 DefaultPort = 8765;

    bool Start(int32 Port = DefaultPort);
    void StopServer();

    virtual uint32 Run() override;
    virtual void Stop() override;

private:
    void HandleClient(FSocket* Client);
    FString ProcessLine(const FString& Line);

    FSocket* ListenSocket = nullptr;
    FThreadSafeBool bStopping = false;
    TUniquePtr<FRunnableThread> Thread;
};
