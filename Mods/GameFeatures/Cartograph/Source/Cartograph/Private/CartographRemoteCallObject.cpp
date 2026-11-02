#include "CartographRemoteCallObject.h"

#include "Net/UnrealNetwork.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

#include "Module/GameInstanceModuleManager.h"

#include "CartographGameInstanceModule.h"
#include "Cartograph_ConfigStruct.h"


// The server sends a window of slices per request instead of one, so a big save takes
// SlicesPerRequest times fewer round trips. 16 slices is ~32 KB in flight, far below the reliable buffer.
// Both sides must run the same build: the client asks for the next window after the last slice of one.
constexpr int32 SlicesPerRequest = 16;

bool FBuildingDataBuffer::NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
{
    Ar.Serialize(Data, BuildingDataBufferMaxSize);
    bOutSuccess = !Ar.IsError();
    return bOutSuccess;
}


void UCartographRemoteCallObject::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

    DOREPLIFETIME(UCartographRemoteCallObject, bDummy);
}


// No time out needed: the RCO (and the data with it) goes away with its player.
void UCartographRemoteCallObject::ServerRequestInitialBuildingData_Implementation(APlayerController*, EInitialDataSendPhase SendPhase)
{
    switch (SendPhase)
    {
    case EInitialDataSendPhase::Initial:
    {
        CARTO_LOG_ERROR_RETURN_IF_NULL(UCartographGameInstanceModule::Instance);
        if (FPlatformProperties::IsServerOnly())
        {
            UCartographGameInstanceModule::Instance->CompactServerBuildingData();
        }
        InitialBuildingDataToSend.Reset();
        FMemoryWriter Archive{ InitialBuildingDataToSend };

        // ArIsNetArchive should NOT be set since it'll prevent big array from being serialized.
        // It's OK since we'll divide them and send gradually.
        Archive << UCartographGameInstanceModule::Instance->CurrentBuildingData;
        CARTO_LOG("Started Sending Initial Data: %d", InitialBuildingDataToSend.Num());

        SliceCount = InitialBuildingDataToSend.Num() / BuildingDataBufferMaxSize + 1;
        LastSentSlice = -1;
        break;
    }

    case EInitialDataSendPhase::Normal:
        break;

    case EInitialDataSendPhase::Finished:
        CARTO_LOG("Finished Sending Initial Data");
        InitialBuildingDataToSend.Empty();
        SliceCount = 0;
        return;
    }

    for (int32 Sent = 0; Sent < SlicesPerRequest && LastSentSlice + 1 < SliceCount; ++Sent)
    {
        const int32 i = ++LastSentSlice;
        FBuildingDataBuffer SendBuffer;
        const int16 Size = FMath::Min(BuildingDataBufferMaxSize, InitialBuildingDataToSend.Num() - i * BuildingDataBufferMaxSize);
        FMemory::Memcpy(SendBuffer.Data, InitialBuildingDataToSend.GetData() + i * BuildingDataBufferMaxSize, Size);
        ClientReceiveInitialBuildingData(SendBuffer, Size, SliceCount);

        CARTO_LOG_DEBUG("Sending Initial Data (%d/%d)", i + 1, SliceCount);
    }
}


void UCartographRemoteCallObject::ClientReceiveInitialBuildingData_Implementation(const FBuildingDataBuffer& Array, int16 Size, int16 TotalSliceCount)
{
    CARTO_LOG_DEBUG("Received Initial Data");
    Buffer.Append(Array.Data, FMath::Clamp<int32>(Size, 0, BuildingDataBufferMaxSize));

    CARTO_LOG_ERROR_RETURN_IF_NULL(UCartographGameInstanceModule::Instance);
    ReceivedSliceCount++;
    UCartographGameInstanceModule::Instance->InitializeProgress = static_cast<float>(ReceivedSliceCount) / TotalSliceCount;

    const bool IsLast = ReceivedSliceCount == TotalSliceCount;
    if (IsLast)
    {
        CARTO_LOG("Received Last Initial Data: %d.", Buffer.Num());
        InitialBuildableDeserialize();
    }

    // Only the last slice of a window asks for more
    if (IsLast || ReceivedSliceCount % SlicesPerRequest == 0)
    {
        ServerRequestInitialBuildingData(GetWorld()->GetFirstPlayerController(),
            IsLast ? EInitialDataSendPhase::Finished : EInitialDataSendPhase::Normal);
    }
}


UE5Coro::TCoroutine<> UCartographRemoteCallObject::InitialBuildableDeserialize(FForceLatentCoroutine)
{
    FMemoryReader Ar{ Buffer };
    Ar.ArIsNetArchive = true;

    // Will use redraw time budget since it's most likely to be in the middle of the game.
    const float TimeBudget = FCartograph_ConfigStruct::GetActiveConfig(GetWorld()).RedrawTimeBudget;
    UE5Coro::Latent::FTickTimeBudget Budget = UE5Coro::Latent::FTickTimeBudget::Milliseconds(TimeBudget);

    if (!UCartographGameInstanceModule::Instance)
    {
        CARTO_LOG_ERROR("UCartographGameInstanceModule::Instance is null");
        co_return;
    }

    auto& A = UCartographGameInstanceModule::Instance->CurrentBuildingData;

    /// Below is from `FArchive& TArrayPrivateFriend::Serialize(FArchive& Ar, TArray<ElementType, AllocatorType>& A)`
    A.CountBytes(Ar);

    int32 SerializeNum = 0;
    Ar << SerializeNum;

    A.Empty();  // Not reserving SerializeNum: if it's big, that can cause a lag spike.

    for (int32 i = 0; i < SerializeNum; i++)
    {
	    FBuildingData& NewElement = A.AddDefaulted_GetRef();
        Ar << NewElement;
        if (Ar.IsError())
        {
            CARTO_LOG_ERROR("Initial data is corrupted, stopping at %d/%d", i, SerializeNum);
            A.RemoveAt(A.Num() - 1);
            break;
        }
        UCartographGameInstanceModule::Instance->OnBuildingDataAdd(NewElement, i);
        co_await Budget;
    }
    /// End

    for (const auto& [ClassHash, Count] : UCartographGameInstanceModule::Instance->BuildingCountMap)
    {
        CARTO_LOG_DEBUG("Building: %u, Count: %d", ClassHash, Count);
    }

    CARTO_LOG("InitialBuildableDeserialize Finished, %d", SerializeNum);
    Buffer.Empty();

    UCartographGameInstanceModule::Instance->IsInitializing = false;
    UCartographGameInstanceModule::Instance->OnZFilterUpdated(0, 1);
    UCartographGameInstanceModule::Instance->RedrawMap(true);
}
