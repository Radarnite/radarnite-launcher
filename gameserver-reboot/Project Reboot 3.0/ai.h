#pragma once

#include "reboot.h"
#include "Actor.h"
#include "SoftObjectPath.h"
#include "KismetStringLibrary.h"
#include "KismetSystemLibrary.h"
#include "GameplayStatics.h"
#include "FortPlayerPawn.h"
#include "FortAthenaMutator.h"
#include "FortPlayerController.h"
#include "FortGameModeAthena.h"
#include "FortGameStateAthena.h"
#include "FortPlayerControllerAthena.h"
#include "FortBotNameSettings.h"
#include "KismetTextLibrary.h"
#include "FortAthenaAIBotCustomizationData.h"
#include "FortAthenaAIBotSpawnerData.h"

using UNavigationSystemV1 = UObject;
using UNavigationSystemConfig = UObject;
using AAthenaNavSystemConfigOverride = UObject;
using UAthenaNavSystem = UObject;
using UAthenaNavSystemConfig = UObject;
inline AFortAthenaMutator_Bots* BotMutator = nullptr;

enum class EFNavigationSystemRunMode : uint8_t
{
    InvalidMode = 0,
    GameMode = 1,
    EditorMode = 2,
    SimulationMode = 3,
    PIEMode = 4,
    FNavigationSystemRunMode_MAX = 5
};

enum class ENavSystemOverridePolicy : uint8_t
{
    Override = 0,
    Append = 1,
    Skip = 2,
    ENavSystemOverridePolicy_MAX = 3
};

extern inline void (*NavSystemCleanUpOriginal)(UNavigationSystemV1*, uint8) = nullptr;

static bool SetNavigationSystem(AAthenaNavSystemConfigOverride* NavSystemOverride)
{
    UAthenaNavSystem*& NavSystem = (UAthenaNavSystem*&)GetWorld()->Get("NavigationSystem");

    LOG_INFO(LogDev, "NavSystem: {}", NavSystem->IsValidLowLevel() ? NavSystem->GetFullName() : "BadRead");

    if (NavSystem)
    {
        return false;
        NavSystemCleanUpOriginal(NavSystem, 0);
        GetWorld()->Get("NavigationSystem") = nullptr;
    }

    auto WorldSettings = GetWorld()->GetWorldSettings();

    LOG_INFO(LogDev, "WorldSettings: {}", WorldSettings->IsValidLowLevel() ? WorldSettings->GetFullName() : "BadRead");

    if (!WorldSettings)
        return false;

    static auto OverridePolicyOffset = NavSystemOverride->GetOffset("OverridePolicy", false);
    
    if (OverridePolicyOffset != -1)
        NavSystemOverride->Get<ENavSystemOverridePolicy>(OverridePolicyOffset) = ENavSystemOverridePolicy::Append;
    
    static auto NavSystemOverride_NavigationSystemConfigOffset = NavSystemOverride->GetOffset("NavigationSystemConfig");

    WorldSettings->Get("NavigationSystemConfigOverride") = NavSystemOverride->Get(NavSystemOverride_NavigationSystemConfigOffset);

    LOG_INFO(LogDev, "WorldSettings_NavigationSystemConfig: {}", __int64(WorldSettings->Get("NavigationSystemConfig")));

    if (WorldSettings->Get("NavigationSystemConfig"))
        WorldSettings->Get("NavigationSystemConfig")->Get<bool>("bIsOverriden") = true;

    if (!NavSystemOverride->Get("NavigationSystemConfig"))
    {
        LOG_ERROR(LogAI, "No NavigationSystemConfig!");
        return false;
    }

    auto& ClassPath = NavSystemOverride->Get("NavigationSystemConfig")->Get<FSoftObjectPath>("NavigationSystemClass");

    auto NewNavSystemClass = FindObject<UClass>(ClassPath.AssetPathName.ToString());

    if (!NewNavSystemClass)
    {
        LOG_ERROR(LogAI, "No NavigationSystemClass!");
        return false;
    }

    LOG_INFO(LogAI, "Setup navigation system.");

    // if (Fortnite_Version >= 10.40) // idk when they added the fifth arg or does it even matter????
    {
        static void (*AddNavigationSystemToWorldOriginal)(UWorld * WorldOwner, EFNavigationSystemRunMode RunMode, UNavigationSystemConfig * NavigationSystemConfig, char bInitializeForWorld, char bOverridePreviousNavSys) =
            decltype(AddNavigationSystemToWorldOriginal)(Addresses::AddNavigationSystemToWorld);

        AddNavigationSystemToWorldOriginal(GetWorld(), EFNavigationSystemRunMode::GameMode, NavSystemOverride->Get("NavigationSystemConfig"), true, false);
    }

    return true;
}

static inline AFortAthenaMutator_Bots* SpawnBotMutator() //sets up all the classes for phoebe
{
   auto GameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());
   auto GameMode = Cast<AFortGameModeAthena>(GetWorld()->GetGameMode());

   static auto BGAClass = FindObject<UClass>(L"/Script/Engine.BlueprintGeneratedClass");
   // fnoffline: the Phoebe blueprint (its spawning item data, MMR spawning and skill override tables), the base class
   // only when the blueprint is missing
   auto PhoebeMutatorClass = Fortnite_Version >= 14 ? LoadObject<UClass>(L"/Game/Athena/AI/Phoebe/BP_Phoebe_Mutator.BP_Phoebe_Mutator_C", BGAClass) : nullptr;

   if (!AFortAthenaMutator_Bots::StaticClass())
   {
       return nullptr;
   }

    BotMutator = GetWorld()->SpawnActor<AFortAthenaMutator_Bots>(PhoebeMutatorClass ? PhoebeMutatorClass : AFortAthenaMutator_Bots::StaticClass());
    LOG_INFO(LogAI, "[fnoffline] bot mutator: {}", PhoebeMutatorClass ? "BP_Phoebe_Mutator" : "FortAthenaMutator_Bots");

   if (!BotMutator)
   {
       LOG_WARN(LogAI, "Failed to spawn Bot Mutator!");
       return nullptr;
   }

   static auto CachedGameModeOffset = BotMutator->GetOffset("CachedGameMode");
   BotMutator->Get(CachedGameModeOffset) = GameMode;

   static auto CachedGameStateOffset = BotMutator->GetOffset("CachedGameState", false);

   if (CachedGameStateOffset != -1)
       BotMutator->Get(CachedGameStateOffset) = GameState;

   return BotMutator;
}

static void SetupServerBotManager()
{
    auto GameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());
    auto GameMode = Cast<AFortGameModeAthena>(GetWorld()->GetGameMode());

    static auto FortServerBotManagerClass = FindObject<UClass>(L"/Script/FortniteGame.FortServerBotManagerAthena"); // Is there a BP for this? // GameMode->ServerBotManagerClass

    if (!FortServerBotManagerClass)
        return;

    UFortServerBotManagerAthena*& ServerBotManager = GameMode->GetServerBotManager();

    if (!ServerBotManager)
        ServerBotManager = (UFortServerBotManagerAthena*)UGameplayStatics::SpawnObject(FortServerBotManagerClass, GameMode);

    // fnoffline: the playlist's AI settings (its AI services) before the AI system starts them
    if (Globals::bFnOfflineBotsAI)
    {
        static auto AISettingsOffset = GameMode->GetOffset("AISettings", false);
        auto Playlist = GameState ? GameState->GetCurrentPlaylist() : nullptr;
        static auto PlaylistAISettingsOffset = Playlist ? Playlist->GetOffset("AISettings", false) : -1;

        if (AISettingsOffset != -1)
        {
            bool WasSet = GameMode->Get(AISettingsOffset) != nullptr;
            if (!WasSet && Playlist && PlaylistAISettingsOffset != -1)
                GameMode->Get(AISettingsOffset) = Playlist->Get(PlaylistAISettingsOffset);
            LOG_INFO(LogAI, "[fnoffline] game mode AI settings: {} ({})", GameMode->Get(AISettingsOffset) ? GameMode->Get(AISettingsOffset)->GetName() : "none",
                WasSet ? "already set" : "from the playlist");
        }
    }

    // fnoffline: developer diagnostics of the AI system
    if (Globals::bFnOfflineBotsAI && ServerBotManager)
    {
        auto World = GetWorld();
        static auto AISystemOffset = World->GetOffset("AISystem", false);
        auto AISystem = AISystemOffset != -1 ? World->Get<UObject*>(AISystemOffset) : nullptr;
        // developer (FNOFFLINE_BOT_DIAG): the AI logs that say why a bot's behavior tree does not start, and the AI
        // services the AI system made
        static bool bDiag = [] { char V[8] = {}; return GetEnvironmentVariableA("FNOFFLINE_BOT_DIAG", V, sizeof(V)) && V[0] == '1'; }();

        if (bDiag)
        {
            for (auto Cmd : { L"log LogBehaviorTree VeryVerbose", L"log LogAISpawnerData VeryVerbose", L"log LogAthenaBots VeryVerbose", L"log LogAthenaAIServices VeryVerbose", L"log LogNavigation VeryVerbose", L"log LogNavigationDataBuild VeryVerbose", L"log LogNavOctree VeryVerbose" })
                UKismetSystemLibrary::ExecuteConsoleCommand(World, Cmd, nullptr);

            static auto ServiceManagerOffset = AISystem ? AISystem->GetOffset("AIServiceManager", false) : -1;
            auto ServiceManager = ServiceManagerOffset != -1 ? AISystem->Get<UObject*>(ServiceManagerOffset) : nullptr;
            std::string Out;

            if (ServiceManager)
                for (auto Cls = ServiceManager->ClassPrivate; Cls; Cls = *(UClass**)(__int64(Cls) + Offsets::SuperStruct))
                    for (void* Prop = *(void**)(__int64(Cls) + Offsets::Children); Prop; Prop = GetNext(Prop))
                    {
                        auto Name = GetFNameOfProp(Prop)->ToString();
                        int Off = *(int*)(__int64(Prop) + Offsets::Offset_Internal);
                        Out += " " + Name + "@" + std::to_string(Off);
                        if (Name.find("Service") != std::string::npos)
                        {
                            auto& Arr = *(TArray<UObject*>*)(__int64(ServiceManager) + Off);
                            Out += "[" + std::to_string(Arr.Num()) + ":";
                            for (int i = 0; i < Arr.Num() && i < 16; i++)
                                Out += " " + (Arr.at(i) && Arr.at(i)->IsValidLowLevel() ? Arr.at(i)->GetName() : std::string("?"));
                            Out += "]";
                        }
                    }

            LOG_INFO(LogAI, "[fnoffline] AI service manager {}:{}", ServiceManager ? ServiceManager->GetName() : "none", Out);
        }

    }

    if (ServerBotManager)
    {
        static auto CachedGameModeOffset = ServerBotManager->GetOffset("CachedGameMode");
        ServerBotManager->Get(CachedGameModeOffset) = GameMode;

        static auto CachedGameStateOffset = ServerBotManager->GetOffset("CachedGameState", false);

        if (CachedGameStateOffset != -1)
            ServerBotManager->Get(CachedGameStateOffset) = GameState;

        static auto CachedBotMutatorOffset = ServerBotManager->GetOffset("CachedBotMutator");
        auto BotMutator = FindFirstMutator(FindObject<UClass>(L"/Script/FortniteGame.FortAthenaMutator_Bots"));

        // fnoffline: with the game's AI on 14+, the playlist's gameplay modifier spawns its own Phoebe mutator after
        // this; a second one is refused ("Bot Mutator already present!"). bots.h SetupPhoebe picks it before the first bot.
        if (!BotMutator && Globals::bFnOfflineBotsAI && Fortnite_Version >= 14)
            return;

        if (!BotMutator)
        {
            LOG_WARN(LogAI, "Failed to find Bot Mutator! Spawning it..");
            BotMutator = SpawnBotMutator();
        }

        if (!BotMutator)
        {
            LOG_ERROR(LogAI, "Failed to spawn or find Bot Mutator!");
        }

        ServerBotManager->Get(CachedBotMutatorOffset) = BotMutator;
    }
}

static void SetupAIGoalManager()
{
    // Playlist->AISettings->bAllowAIGoalManager

    // There is some virtual function in the gamemode that calls a spawner for this, it gets the class from GameData, not sure why this doesn't work automatically or if we should even spawn this.

    auto GameMode = Cast<AFortGameModeAthena>(GetWorld()->GetGameMode());
    static auto AIGoalManagerOffset = GameMode->GetOffset("AIGoalManager");

    static auto AIGoalManagerClass = FindObject<UClass>(L"/Script.FortniteGame.FortAIGoalManager");

    if (!AIGoalManagerClass)
        return;

    LOG_INFO(LogDev, "AIGoalManager Before: {}", __int64(GameMode->Get(AIGoalManagerOffset)));

    if (!GameMode->Get(AIGoalManagerOffset))
        GameMode->Get(AIGoalManagerOffset) = GetWorld()->SpawnActor<AActor>(AIGoalManagerClass);

    if (GameMode->Get(AIGoalManagerOffset))
    {
        LOG_INFO(LogAI, "Successfully spawned AIGoalManager!");
    }
}

static void SetupAIDirector()
{
    // Playlist->AISettings->bAllowAIDirector

    auto GameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());
    auto GameMode = Cast<AFortGameModeAthena>(GetWorld()->GetGameMode());

    static auto AIDirectorClass = FindObject<UClass>(L"/Script/FortniteGame.AthenaAIDirector"); // Probably wrong class

    if (!AIDirectorClass)
        return;

    static auto AIDirectorOffset = GameMode->GetOffset("AIDirector");
    
    LOG_INFO(LogDev, "AIDirector Before: {}", __int64(GameMode->Get(AIDirectorOffset)));

    if (!GameMode->Get(AIDirectorOffset))
        GameMode->Get(AIDirectorOffset) = GetWorld()->SpawnActor<AActor>(AIDirectorClass);

    if (GameMode->Get(AIDirectorOffset))
    {
        LOG_INFO(LogAI, "Successfully spawned AIDirector!");

        // we have to set so much more from data tables..
        
        static auto OurEncounterClass = FindObject<UClass>(L"/Script/FortniteGame.FortAIEncounterInfo"); // ???
        static auto BaseEncounterClassOffset = GameMode->Get(AIDirectorOffset)->GetOffset("BaseEncounterClass");

        GameMode->Get(AIDirectorOffset)->Get(BaseEncounterClassOffset) = OurEncounterClass;

        static auto ActivateFn = FindObject<UFunction>(L"/Script/FortniteGame.FortAIDirector.Activate");

        if (ActivateFn) // ?
            GameMode->Get(AIDirectorOffset)->ProcessEvent(ActivateFn); // ?
    }
}

static void SetupNavConfig(const FName& AgentName)
{
    static auto AthenaNavSystemConfigOverrideClass = FindObject<UClass>(L"/Script/FortniteGame.AthenaNavSystemConfigOverride");
    auto NavSystemOverride = GetWorld()->SpawnActor<AActor>(AthenaNavSystemConfigOverrideClass);

    if (!NavSystemOverride)
        return;

    static auto AthenaNavSystemConfigClass = FindObject<UClass>(L"/Script/FortniteGame.AthenaNavSystemConfig");
    auto AthenaNavConfig = (UAthenaNavSystemConfig*)UGameplayStatics::SpawnObject(AthenaNavSystemConfigClass, NavSystemOverride);
    AthenaNavConfig->Get<bool>("bUseBuildingGridAsNavigableSpace") = false;

    static auto bUsesStreamedInNavLevelOffset = AthenaNavConfig->GetOffset("bUsesStreamedInNavLevel", false);

    if (bUsesStreamedInNavLevelOffset != -1)
        AthenaNavConfig->Get<bool>(bUsesStreamedInNavLevelOffset) = true;

    AthenaNavConfig->Get<bool>("bAllowAutoRebuild") = true;
    AthenaNavConfig->Get<bool>("bCreateOnClient") = true; // BITFIELD
    AthenaNavConfig->Get<bool>("bAutoSpawnMissingNavData") = true; // BITFIELD
    AthenaNavConfig->Get<bool>("bSpawnNavDataInNavBoundsLevel") = true; // BITFIELD

    static auto bUseNavigationInvokersOffset = AthenaNavConfig->GetOffset("bUseNavigationInvokers", false);

    if (bUseNavigationInvokersOffset != -1)
        AthenaNavConfig->Get<bool>(bUseNavigationInvokersOffset) = false;

    static auto DefaultAgentNameOffset = AthenaNavConfig->GetOffset("DefaultAgentName", false);

    if (DefaultAgentNameOffset != -1)
        AthenaNavConfig->Get<FName>(DefaultAgentNameOffset) = AgentName;

    // NavSystemOverride->Get<ENavSystemOverridePolicy>("OverridePolicy") = ENavSystemOverridePolicy::Append;
    NavSystemOverride->Get("NavigationSystemConfig") = AthenaNavConfig;

    SetNavigationSystem(NavSystemOverride);
}

static AFortPlayerPawn* SpawnAIFromSpawnerData(const FVector& Location, UFortAthenaAIBotSpawnerData* SpawnerData)
{
    auto SpawnParamsComponent = SpawnerData->GetSpawnParamsComponent();

    if (!SpawnParamsComponent)
    {
        LOG_INFO(LogAI, "Invalid SpawnParamsComponent for AI!");
        return nullptr;
    }

    auto PawnClass = SpawnerData->GetSpawnParamsComponent()->GetPawnClass();

    LOG_INFO(LogAI, "PawnClass: {}", __int64(PawnClass));

    if (!PawnClass)
    {
        LOG_INFO(LogAI, "Invalid PawnClass for AI!");
        return nullptr;
    }

    LOG_INFO(LogAI, "PawnClass Name: {}", PawnClass->GetFullName());
    auto Pawn = GetWorld()->SpawnActor<AFortPlayerPawn>(PawnClass, Location);

    if (!Pawn)
    {
        LOG_INFO(LogAI, "Failed to spawn pawn!");
        return nullptr;
    }

    auto CharacterToApply = SpawnerData->GetCosmeticComponent()->GetCosmeticLoadout()->GetCharacter();
    LOG_INFO(LogAI, "CharacterToApply: {}", __int64(CharacterToApply));
    ApplyCID(Pawn, CharacterToApply, true); // bruhh

    return Pawn;
}

static AFortPlayerPawn* SpawnAIFromCustomizationData(const FVector& Location, UFortAthenaAIBotCustomizationData* CustomizationData)
{
    static auto PawnClassOffset = CustomizationData->GetOffset("PawnClass");
    auto PawnClass = CustomizationData->Get<UClass*>(PawnClassOffset);

    if (!PawnClass)
    {
        LOG_INFO(LogAI, "Invalid PawnClass for AI!");
        return nullptr;
    }

    auto Pawn = GetWorld()->SpawnActor<AFortPlayerPawn>(PawnClass, Location);

    if (!Pawn)
    {
        LOG_INFO(LogAI, "Failed to spawn pawn!");
        return nullptr;
    }

    auto Controller = Pawn->GetController();

    if (!Controller)
    {
        LOG_INFO(LogAI, "No controller!");
        Pawn->K2_DestroyActor();
        return nullptr;
    }

    auto PlayerState = Controller->GetPlayerState();

    if (!PlayerState)
    {
        LOG_INFO(LogAI, "No PlayerState!");
        Controller->K2_DestroyActor();
        Pawn->K2_DestroyActor();
        return nullptr;
    }

    static auto CharacterCustomizationOffset = CustomizationData->GetOffset("CharacterCustomization");
    auto CharacterCustomization = CustomizationData->Get(CharacterCustomizationOffset);
    auto CharacterCustomizationLoadoutOffset = CharacterCustomization->GetOffset("CustomizationLoadout");
    auto CharacterCustomizationLoadout = CharacterCustomization->GetPtr<FFortAthenaLoadout>(CharacterCustomizationLoadoutOffset);
    auto CharacterToApply = CharacterCustomizationLoadout->GetCharacter();

    ApplyCID(Pawn, CharacterToApply, true); // bruhh

    struct FItemAndCount
    {
        int                                                Count;                                                    // 0x0000(0x0004) (Edit, BlueprintVisible, ZeroConstructor, IsPlainOldData, NoDestructor, HasGetValueTypeHash, NativeAccessSpecifierPublic)
        unsigned char                                      UnknownData00[0x4];                                       // 0x0004(0x0004) MISSED OFFSET
        UFortItemDefinition* Item;                                                     // 0x0008(0x0008) (Edit, BlueprintVisible, ZeroConstructor, IsPlainOldData, NoDestructor, HasGetValueTypeHash, NativeAccessSpecifierPublic)
    };

    static auto StartupInventoryOffset = CustomizationData->GetOffset("StartupInventory");
    auto StartupInventory = CustomizationData->Get(StartupInventoryOffset);
    static auto StartupInventoryItemsOffset = StartupInventory->GetOffset("Items");

    std::vector<std::pair<UFortItemDefinition*, int>> ItemsToGrant;

    if (Fortnite_Version < 13)
    {
        auto& StartupInventoryItems = StartupInventory->Get<TArray<UFortItemDefinition*>>(StartupInventoryItemsOffset);

        for (int i = 0; i < StartupInventoryItems.Num(); ++i)
        {
            ItemsToGrant.push_back({ StartupInventoryItems.at(i), 1 });
        }
    }
    else
    {
        auto& StartupInventoryItems = StartupInventory->Get<TArray<FItemAndCount>>(StartupInventoryItemsOffset);

        for (int i = 0; i < StartupInventoryItems.Num(); ++i)
        {
            ItemsToGrant.push_back({ StartupInventoryItems.at(i).Item, StartupInventoryItems.at(i).Count });
        }
    }

    static auto InventoryOffset = Controller->GetOffset("Inventory");
    auto Inventory = Controller->Get<AFortInventory*>(InventoryOffset);

    if (Inventory)
    {
        for (int i = 0; i < ItemsToGrant.size(); ++i)
        {
            auto pair = Inventory->AddItem(ItemsToGrant.at(i).first, nullptr, ItemsToGrant.at(i).second);

            LOG_INFO(LogDev, "pair.first.size(): {}", pair.first.size());

            if (pair.first.size() > 0)
            {
                if (auto weaponDef = Cast<UFortWeaponItemDefinition>(ItemsToGrant.at(i).first))
                    Pawn->EquipWeaponDefinition(weaponDef, pair.first.at(0)->GetItemEntry()->GetItemGuid());
            }
        }

        Inventory->Update();
    }

    static auto BotNameSettingsOffset = CustomizationData->GetOffset("BotNameSettings");
    auto BotNameSettings = CustomizationData->Get<UFortBotNameSettings*>(BotNameSettingsOffset);

    FString Name;

    if (BotNameSettings)
    {
        static int CurrentId = 0; // scuffed!
        static auto DisplayNameOffset = FindOffsetStruct("/Script/FortniteGame.FortItemDefinition", "DisplayName");

        switch (BotNameSettings->GetNamingMode())
        {
        case EBotNamingMode::Custom:
            Name = UKismetTextLibrary::Conv_TextToString(BotNameSettings->GetOverrideName());
            break;
        case EBotNamingMode::SkinName:
            Name = CharacterToApply ? UKismetTextLibrary::Conv_TextToString(*(FText*)(__int64(CharacterCustomizationLoadout->GetCharacter()) + DisplayNameOffset)) : L"InvalidCharacter";
            Name.Set((std::wstring(Name.Data.Data) += std::to_wstring(CurrentId++)).c_str());
            break;
        default:
            Name = L"Unknown";
            break;
        }
    }

    if (Name.Data.Data && Name.Data.Num() > 0)
    {
        Controller->ServerChangeName(Name);
    }

    return Pawn;
}
