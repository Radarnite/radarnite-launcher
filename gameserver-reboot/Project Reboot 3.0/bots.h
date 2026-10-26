#pragma once

#include "FortGameModeAthena.h"
#include "OnlineReplStructs.h"
#include "FortAthenaAIBotController.h"
#include "BuildingContainer.h"
#include "botnames.h"
#include "FortWeaponMeleeItemDefinition.h"
#include "hooking.h"
#include <map>
#include <algorithm>

// fnoffline: start a game AI bot's behavior tree (BT_Phoebe) when its controller has none running: the blackboard
// first (AIController.UseBlackboard), then AIController.RunBehaviorTree; logs what each step returned. True = running.
inline bool FnOfflineStartBrain(UObject* AIC, const char* When)
{
	static auto UseBBFn = FindObject<UFunction>(L"/Script/AIModule.AIController.UseBlackboard");
	static auto RunBTFn = FindObject<UFunction>(L"/Script/AIModule.AIController.RunBehaviorTree");

	if (!AIC || !UseBBFn || !RunBTFn)
		return false;

	auto Obj = [](UObject* O, const char* N) -> UObject* { int Off = O ? O->GetOffset(N, false) : -1; return Off != -1 ? O->Get<UObject*>(Off) : nullptr; };
	auto Name = [](UObject* O) { return O ? O->GetName() : std::string("none"); };

	if (Obj(AIC, "BrainComponent"))
		return true;

	auto BT = Obj(AIC, "BehaviorTree");
	auto BBAsset = Obj(BT, "BlackboardAsset");
	auto BBComp = Obj(AIC, "Blackboard");
	auto CompAsset = Obj(BBComp, "BlackboardAsset");

	if (!BT)
		return false;

	struct { UObject* BlackboardAsset; UObject* BlackboardComponent; bool ReturnValue; } UB{ BBAsset, nullptr, false };
	AIC->ProcessEvent(UseBBFn, &UB);
	struct { UObject* BTAsset; bool ReturnValue; } RB{ BT, false };
	AIC->ProcessEvent(RunBTFn, &RB);
	bool Running = Obj(AIC, "BrainComponent") != nullptr;

	LOG_INFO(LogBots, "[fnoffline] {} {}: tree {} with blackboard {} (component {} had {}): UseBlackboard {} ({} now has {}), RunBehaviorTree {}, brain {}", When, Name(AIC),
		Name(BT), Name(BBAsset), Name(BBComp), Name(CompAsset), UB.ReturnValue, Name(UB.BlackboardComponent), Name(Obj(UB.BlackboardComponent, "BlackboardAsset")), RB.ReturnValue,
		Name(Obj(AIC, "BrainComponent")));
	return Running;
}

class BotPOI
{
	FVector CenterLocation;
	FVector Range; // this just has to be FVector2D
};

class BotPOIEncounter
{
public:
	int NumChestsSearched;
	int NumAmmoBoxesSearched;
	int NumPlayersEncountered;
};

class PlayerBot
{
public:
	static inline UClass* PawnClass = nullptr;
	static inline UClass* ControllerClass = nullptr;

	AController* Controller = nullptr; // This can be 1. AFortAthenaAIBotController OR AFortPlayerControllerAthena
	bool bIsAthenaController = false;
	AFortPlayerPawnAthena* Pawn = nullptr;
	AFortPlayerStateAthena* PlayerState = nullptr;
	BotPOIEncounter currentBotEncounter;
	int TotalPlayersEncountered;
	std::vector<BotPOI> POIsTraveled;
	float NextJumpTime = 1.0f;

	void OnPlayerEncountered()
	{
		currentBotEncounter.NumPlayersEncountered++;
		TotalPlayersEncountered++;
	}

	void MoveToNewPOI()
	{

	}

	static bool ShouldUseAIBotController()
	{
		if (!Globals::bFnOfflineBotsAI) // fnoffline: upstream hard-disables this path
			return false;
		return Fortnite_Version >= 11 && Engine_Version < 500;
	}

	static void InitializeBotClasses()
	{
		static auto BlueprintGeneratedClassClass = FindObject<UClass>(L"/Script/Engine.BlueprintGeneratedClass");

		if (!ShouldUseAIBotController())
		{
			PawnClass = FindObject<UClass>(L"/Game/Athena/PlayerPawn_Athena.PlayerPawn_Athena_C");
			ControllerClass = AFortPlayerControllerAthena::StaticClass();
		}
		else
		{
			PawnClass = LoadObject<UClass>(L"/Game/Athena/AI/Phoebe/BP_PlayerPawn_Athena_Phoebe.BP_PlayerPawn_Athena_Phoebe_C", BlueprintGeneratedClassClass);
			// ControllerClass = PawnClass->CreateDefaultObject()->GetAIControllerClass();
		}

		if (/* !ControllerClass
			|| */ !PawnClass
			)
		{
			LOG_ERROR(LogBots, "Failed to find a class for the bots!");
			return;
		}
	}

	static UObject* GetObj(UObject* O, const char* Name)
	{
		int Off = O ? O->GetOffset(Name, false) : -1;
		return Off != -1 ? O->Get<UObject*>(Off) : nullptr;
	}

	static void SetObj(UObject* O, const char* Name, UObject* V)
	{
		int Off = O ? O->GetOffset(Name, false) : -1;
		if (Off != -1)
			O->Get<UObject*>(Off) = V;
	}

	static std::string NameOf(UObject* O) { return O ? O->GetName() : std::string("none"); }

	// This match server runs the client build, where UWorld::SpawnActor refuses actors that do not "need load for
	// client" ("Unable to spawn class 'AthenaNavMesh' due to client/server context"): the navigation mesh and the AI
	// director say no (their virtual 0x1B is `xor al, al; ret` in 14.30). Their class default objects, which SpawnActor
	// asks, get a copy of the virtual table with that entry answering yes; spawned actors keep the original table.
	static bool NeedsLoadForClientYes(void*) { return true; }

	static void AllowSpawnOnClientBuild(const wchar_t* ClassPath)
	{
		auto Cls = FindObject<UClass>(ClassPath);
		auto Cdo = Cls ? Cls->CreateDefaultObject() : nullptr;

		if (!Cdo)
			return;

		auto VT = *(void***)Cdo;
		auto Fn = (const uint8_t*)VT[0x1B];

		if (VT[0x1B] == (void*)&NeedsLoadForClientYes)
			return;

		if (!(Fn[0] == 0x32 && Fn[1] == 0xC0 && Fn[2] == 0xC3))   // not the expected "return false": leave it
		{
			LOG_WARN(LogBots, "[fnoffline] {}: virtual 0x1B is not the expected one, not changed", Cls->GetName());
			return;
		}

		constexpr int Entries = 0x300;
		auto Copy = new void*[Entries];
		memcpy(Copy, VT, sizeof(void*) * Entries);
		Copy[0x1B] = (void*)&NeedsLoadForClientYes;
		*(void***)Cdo = Copy;
		LOG_INFO(LogBots, "[fnoffline] {} can be spawned by this server", Cls->GetName());
	}

	// Before the map loads (the navigation system reads its config and starts collecting the level's geometry while
	// it loads): the 14.30 navigation config keeps the navigation octree locked during the load when no prebuilt
	// mesh is preloaded, and geometry registered while it is locked is dropped, so the tiles later built around the
	// bots came out empty. The config's class default gets the octree unlocked and invokers on; the level's config
	// object reads every value it does not override from it.
	static void BoundsOf(AActor* A, FVector& Origin, FVector& Extent)
	{
		static auto BoundsFn = FindObject<UFunction>(L"/Script/Engine.Actor.GetActorBounds");
		struct { bool bOnlyColliding; uint8_t Pad[3]; FVector Origin; FVector Extent; bool bChildren; uint8_t Pad2[3]; } P{ false };
		if (A && BoundsFn)
			A->ProcessEvent(BoundsFn, &P);
		Origin = P.Origin;
		Extent = P.Extent;
	}

	// developer (FNOFFLINE_NAV_FULL=1): the whole island instead of tiles around the bots
	static bool NavFullBuild()
	{
		static bool b = [] { char V[8] = {}; return GetEnvironmentVariableA("FNOFFLINE_NAV_FULL", V, sizeof(V)) && V[0] == '1'; }();
		return b;
	}

	// The lobby's team pedestals (FortTeamMemberPedestal, FortniteUI) sometimes crash the match start on the server:
	// their BeginPlay (virtual slot 0x66) writes through an object pointer at +0xbe8 that is still empty there
	// (EXCEPTION_ACCESS_VIOLATION writing 0xba0, seen in about one match in eight). They only dress the lobby, so on
	// the match server their BeginPlay is skipped while that pointer is empty.
	static inline void (*PedestalBeginPlayOriginal)(UObject*) = nullptr;
	static inline int PedestalSkips = 0;

	// the pointer can also go empty inside the begin play itself (a match crashed with it set on entry), so the call
	// is also guarded: an access violation in it ends that pedestal's begin play instead of the match
	static bool PedestalBeginPlayGuarded(UObject* Self)
	{
		__try
		{
			PedestalBeginPlayOriginal(Self);
			return true;
		}
		__except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
		{
			return false;
		}
	}

	static void PedestalBeginPlayHook(UObject* Self)
	{
		if (Self && !*(void**)((uint8_t*)Self + 0xbe8))
		{
			if (PedestalSkips++ < 4)
				LOG_INFO(LogBots, "[fnoffline] pedestal {}: begin play skipped (nothing to show it on)", Self->GetName());
			return;
		}

		if (!PedestalBeginPlayGuarded(Self) && PedestalSkips++ < 4)
			LOG_INFO(LogBots, "[fnoffline] pedestal {}: begin play stopped by an access violation", Self->GetName());
	}

	static void HookPedestals()
	{
		static bool bDone = false;
		if (Fortnite_Version < 14 || Fortnite_Version >= 15)   // the slot and offset are 14.30's
			return;
		auto Cls = bDone ? nullptr : FindObject<UClass>(L"/Script/FortniteUI.FortTeamMemberPedestal");
		auto Cdo = Cls ? Cls->CreateDefaultObject() : nullptr;

		if (!Cdo)
			return;

		bDone = true;
		auto Fn = Cdo->VFTable[0x66];
		auto Exe = __int64(GetModuleHandleW(nullptr));
		bool Ok = Hooking::MinHook::Hook(Fn, (void*)&PedestalBeginPlayHook, (void**)&PedestalBeginPlayOriginal);
		LOG_INFO(LogBots, "[fnoffline] pedestal begin play exe+0x{:x}: guarded {}", __int64(Fn) - Exe, Ok);
	}

	static void PrepareNavigationBeforeMapLoad()
	{
		HookPedestals();

		char V[8] = {};
		if (!(GetEnvironmentVariableA("FNOFFLINE_BOTS_AI", V, sizeof(V)) && V[0] == '1') || Fortnite_Version < 14 || Fortnite_Version >= 15)
			return;

		std::string Out;
		auto Set = [&](const wchar_t* ClassPath, const char* Name, bool Value) {
			auto Cls = FindObject<UClass>(ClassPath);
			auto Cdo = Cls ? Cls->CreateDefaultObject() : nullptr;
			auto Prop = Cdo ? Cdo->GetProperty(Name, false) : nullptr;
			if (!Prop) { Out += std::string(" ") + Name + " missing"; return; }
			int Off = Cdo->GetOffset(Name);
			auto Mask = GetFieldMask(Prop);
			Out += std::string(" ") + Name + " " + (Cdo->ReadBitfieldValue(Off, Mask) ? "1" : "0") + "->" + (Value ? "1" : "0");
			Cdo->SetBitfieldValue(Off, Mask, Value);
		};
		Set(L"/Script/FortniteGame.AthenaNavSystemConfig", "bNavOctreeUnlockedByDefaultWhenNotPreloadingNavData", true);
		Set(L"/Script/FortniteGame.AthenaNavSystemConfig", "bUseNavigationInvokers", true);
		Set(L"/Script/FortniteGame.AthenaNavSystemConfig", "bAutoSpawnMissingNavData", true);
		Set(L"/Script/FortniteGame.AthenaNavSystem", "bGenerateNavigationOnlyAroundNavigationInvokers", !NavFullBuild());
		AllowSpawnOnClientBuild(L"/Script/FortniteGame.AthenaNavMesh");

		// geometry gathered when it registers (Instant), not when a tile is built (Lazy): the lazily gathered tiles
		// all came out empty ("GenerateCompressedLayers: empty tile")
		auto SetByte = [&](const wchar_t* ClassPath, const char* Name, uint8_t Value) {
			auto Cls = FindObject<UClass>(ClassPath);
			auto Cdo = Cls ? Cls->CreateDefaultObject() : nullptr;
			int Off = Cdo ? Cdo->GetOffset(Name, false) : -1;
			if (Off == -1) { Out += std::string(" ") + Name + " missing"; return; }
			Out += std::string(" ") + Name + " " + std::to_string(Cdo->Get<uint8_t>(Off)) + "->" + std::to_string(Value);
			Cdo->Get<uint8_t>(Off) = Value;
		};
		SetByte(L"/Script/FortniteGame.AthenaNavSystem", "DataGatheringMode", 1);   // ENavDataGatheringModeConfig::Instant
		SetByte(L"/Script/FortniteGame.AthenaNavMesh", "DataGatheringMode", 1);     // ENavDataGatheringMode::Instant
		LOG_INFO(LogBots, "[fnoffline] navigation before the map:{}", Out);
	}

	// Navigation for the game AI bots. The client's Apollo_Nav_Gameplay has the bounds and a config (agent "Phoebe") but
	// no navigation mesh: it is generated here, only around the bots (navigation invokers, the UE way for large worlds;
	// Athena wants them as AthenaNavInvokerBox actors). The nav system spawns the missing AthenaNavMesh when a bounds
	// volume is updated (UNavigationSystemV1::PerformNavigationBoundsUpdate with bAutoCreateNavigationData), made
	// dynamic so it builds tiles at run time, a few at a time.
	static void SetupNavigation()
	{
		auto World = GetWorld();
		auto Nav = GetObj(World, "NavigationSystem");

		if (!Nav)
		{
			LOG_WARN(LogBots, "[fnoffline] navigation: no navigation system");
			return;
		}

		if (Fortnite_Version >= 14 && Fortnite_Version < 15)
		{
			AllowSpawnOnClientBuild(L"/Script/FortniteGame.AthenaNavMesh");
		}

		auto SetBit = [](UObject* O, const char* Name, bool V) {
			auto Prop = O->GetProperty(Name, false);
			if (!Prop)
				return std::string(Name) + " missing";
			int Off = O->GetOffset(Name);
			auto Mask = GetFieldMask(Prop);
			bool Was = O->ReadBitfieldValue(Off, Mask);
			O->SetBitfieldValue(Off, Mask, V);
			return std::string(Name) + " " + (Was ? "1" : "0") + "->" + (V ? "1" : "0");
		};

		std::string Bits = SetBit(Nav, "bAutoCreateNavigationData", true) + ", " + SetBit(Nav, "bGenerateNavigationOnlyAroundNavigationInvokers", !NavFullBuild()) + ", " +
			SetBit(Nav, "bInitialBuildingLocked", false) + ", " + SetBit(Nav, "bAllowClientSideNavigation", true);

		// this match server is the client build: an actor spawns only if it "needs load for client", which navigation
		// data does when the navigation system allows client side navigation and the actor loads on clients
		// ("Unable to spawn class 'AthenaNavMesh' due to client/server context")
		for (auto Path : { L"/Script/FortniteGame.AthenaNavSystem", L"/Script/FortniteGame.FortNavSystem", L"/Script/NavigationSystem.NavigationSystemV1" })
			if (auto Cls = FindObject<UClass>(Path))
				SetBit(Cls->CreateDefaultObject(), "bAllowClientSideNavigation", true);

		// the navmesh class the agent uses: dynamic generation (ERuntimeGenerationType::Dynamic = 2)
		std::string Gen;
		if (auto MeshClass = FindObject<UClass>(L"/Script/FortniteGame.AthenaNavMesh"))
			if (auto Cdo = MeshClass->CreateDefaultObject())
			{
				int Off = Cdo->GetOffset("RuntimeGeneration", false);
				if (Off != -1)
				{
					Gen = "RuntimeGeneration " + std::to_string(Cdo->Get<uint8_t>(Off)) + "->2";
					Cdo->Get<uint8_t>(Off) = 2;
				}
				Gen += ", " + SetBit(Cdo, "bNetLoadOnClient", true);

				// tiles only around the bots, from a fixed pool big enough for about 100 of them: an invoker's 50 m
				// reach is a square of (2 * 5000 / tile size)^2 tiles; bots far apart share none
				int TileSizeOff = Cdo->GetOffset("TileSizeUU", false), PoolOff = Cdo->GetOffset("TilePoolSize", false);
				if (TileSizeOff != -1 && PoolOff != -1)
				{
					float Tile = Cdo->Get<float>(TileSizeOff);
					int PerBot = Tile > 0 ? (int)std::pow(std::ceil(10000.f / Tile) + 1, 2) : 144;
					int Pool = 1;
					while (Pool < PerBot * 100) Pool <<= 1;   // a power of two, as Recast wants
					Gen += ", tile " + std::to_string((int)Tile) + " pool " + std::to_string(Cdo->Get<int>(PoolOff)) + "->" + std::to_string(Pool);
					Cdo->Get<int>(PoolOff) = Pool;
					Gen += ", " + SetBit(Cdo, "bFixedTilePoolSize", true);
				}

			}

		// at most two tiles built at once: the match server shares the machine with the game
		if (auto JobsFn = FindObject<UFunction>(L"/Script/NavigationSystem.NavigationSystemV1.SetMaxSimultaneousTileGenerationJobsCount"))
		{
			struct { int32 MaxNumberOfJobs; } P{ 2 };
			Nav->ProcessEvent(JobsFn, &P);
		}

		int Volumes = 0;
		if (auto BoundsUpdatedFn = FindObject<UFunction>(L"/Script/NavigationSystem.NavigationSystemV1.OnNavigationBoundsUpdated"))
		{
			auto All = UGameplayStatics::GetAllActorsOfClass(World, FindObject<UClass>(L"/Script/NavigationSystem.NavMeshBoundsVolume"));
			for (int i = 0; i < All.Num(); i++)
			{
				struct { AActor* NavVolume; } P{ All.at(i) };
				Nav->ProcessEvent(BoundsUpdatedFn, &P);
				Volumes++;
			}
			All.Free();
		}

		auto Agents = Nav->GetOffset("SupportedAgents", false);
		LOG_INFO(LogBots, "[fnoffline] navigation: {} ({}), {}; {} supported agents, {} bounds volumes updated", Nav->GetName(), Bits, Gen,
			Agents != -1 ? Nav->Get<TArray<uint8_t>>(Agents).Num() : -1, Volumes);
	}

	// an AthenaNavInvokerBox that follows the bot: the navigation mesh is built around it
	static void AddNavInvoker(AActor* Pawn)
	{
		static auto BoxClass = FindObject<UClass>(L"/Script/FortniteGame.AthenaNavInvokerBox");
		static auto AttachFn = FindObject<UFunction>(L"/Script/Engine.Actor.K2_AttachToActor");
		static auto EnableFn = FindObject<UFunction>(L"/Script/FortniteGame.AthenaNavInvokerBox.SetInvokerEnabled");

		if (!Pawn || !BoxClass)
			return;

		auto Box = GetWorld()->SpawnActor<AActor>(BoxClass, Pawn->GetActorLocation());

		if (!Box)
			return;

		int RangeOff = Box->GetOffset("GenerationRange", false);
		float Range = RangeOff != -1 ? Box->Get<float>(RangeOff) : -1;

		if (AttachFn)
		{
			struct { AActor* Parent; FName SocketName; uint8_t LocationRule, RotationRule, ScaleRule; bool bWeldSimulatedBodies; bool ReturnValue; } P{ Pawn, FName(), 2, 1, 1, false, false };
			Box->ProcessEvent(AttachFn, &P);
		}

		if (EnableFn)
		{
			struct { bool bEnable; } P{ true };
			Box->ProcessEvent(EnableFn, &P);
		}

		// the playlist's navigation config has invokers off (bUseNavigationInvokers 0: the real servers load a prebuilt
		// mesh this build does not have), so Athena's box invokers never reach the tile generator; the pawn is also
		// registered as an engine invoker, which the generator builds tiles around (50 m, kept up to 70 m)
		static auto RegisterFn = FindObject<UFunction>(L"/Script/NavigationSystem.NavigationSystemV1.RegisterNavigationInvoker");
		static auto NavV1 = FindObject<UClass>(L"/Script/NavigationSystem.NavigationSystemV1");

		if (RegisterFn && NavV1)
		{
			alignas(16) uint8_t P[32] = {};
			*(AActor**)(P + RegisterFn->GetOffsetFunc("Invoker")) = Pawn;
			*(float*)(P + RegisterFn->GetOffsetFunc("TileGenerationRadius")) = 5000;
			*(float*)(P + RegisterFn->GetOffsetFunc("TileRemovalRadius")) = 7000;
			NavV1->CreateDefaultObject()->ProcessEvent(RegisterFn, P);
		}

		static int Logged = 0;
		if (Logged++ < 2)
			LOG_INFO(LogBots, "[fnoffline] navigation invoker {} on {}, range {:.0f}, component {}, engine invoker {}", Box->GetName(), Pawn->GetName(), Range,
				NameOf(GetObj(Box, "InvokerComp")), RegisterFn ? "registered" : "no function");
	}

	static void SetupPhoebe(AFortGameModeAthena* GameMode, UObject* BotManager)
	{
		SetupNavigation();

		auto World = GetWorld();
		auto GameState = GameMode->GetGameState();
		static auto BGAClass = FindObject<UClass>(L"/Script/Engine.BlueprintGeneratedClass");

		// the playlist's own mutator (GameplayMod_Phoebe_* spawns BP_Phoebe_Mutator_DM_Solo and the like), else ours
		auto Mutators = UGameplayStatics::GetAllActorsOfClass(World, FindObject<UClass>(L"/Script/FortniteGame.FortAthenaMutator_Bots"));
		UObject* Mutator = nullptr;
		std::string MutatorNames;

		for (int i = 0; i < Mutators.Num(); i++)
		{
			auto M = Mutators.at(i);
			MutatorNames += " " + NameOf(M);
			if (!Mutator || NameOf(Mutator).find("_DM_") == std::string::npos && NameOf(M).find("_DM_") != std::string::npos)
				Mutator = M;
		}

		Mutators.Free();

		if (!Mutator)
		{
			auto Cls = LoadObject<UClass>(L"/Game/Athena/AI/Phoebe/BP_Phoebe_Mutator.BP_Phoebe_Mutator_C", BGAClass);
			Mutator = Cls ? World->SpawnActor<AActor>(Cls) : nullptr;
			MutatorNames += " (spawned " + NameOf(Mutator) + ")";
		}

		SetObj(BotManager, "CachedBotMutator", Mutator);

		// AFortAthenaAIBotController::RunBehaviorTree refuses while its bot manager's byte at +0x420 is 0 (read from the
		// 14.30 code; a native flag of UFortServerBotManagerAthena, set by the game's own bot setup that this server skips)
		if (Fortnite_Version >= 14 && Fortnite_Version < 15)
		{
			LOG_INFO(LogBots, "[fnoffline] bot manager ready flag (+0x420): {} -> 1", (int)*(uint8_t*)(__int64(BotManager) + 0x420));
			*(uint8_t*)(__int64(BotManager) + 0x420) = 1;
		}
		SetObj(Mutator, "CachedGameMode", GameMode);
		SetObj(Mutator, "CachedGameState", GameState);

		// the playlist's AI settings, with the player bots service
		auto GameStateAthena = Cast<AFortGameStateAthena>(GameState);
		auto Playlist = GameStateAthena ? GameStateAthena->GetCurrentPlaylist() : nullptr;
		auto AISettings = GetObj(GameMode, "AISettings");

		if (!AISettings && Playlist)
		{
			AISettings = GetObj(Playlist, "AISettings");
			SetObj(GameMode, "AISettings", AISettings);
		}

		std::string Services;

		if (AISettings)
		{
			int ServicesOff = AISettings->GetOffset("AIServices", false);

			if (ServicesOff != -1)
			{
				auto& List = AISettings->Get<TArray<UClass*>>(ServicesOff);
				auto PlayerBots = FindObject<UClass>(L"/Script/FortniteGame.AthenaAIServicePlayerBots");
				bool Has = false;

				for (int i = 0; i < List.Num(); i++)
				{
					Services += " " + NameOf(List.at(i));
					Has |= List.at(i) == PlayerBots;
				}

				if (!Has && PlayerBots)
				{
					List.AddPtr(&PlayerBots);
					Services += " +AthenaAIServicePlayerBots";
				}
			}
		}

		// the AI system's population tracker in the bot manager
		static int AISystemOff = World->GetOffset("AISystem", false);
		auto AISystem = AISystemOff != -1 ? World->Get<UObject*>(AISystemOff) : nullptr;

		if (!GetObj(BotManager, "CachedAIPopulationTracker"))
			SetObj(BotManager, "CachedAIPopulationTracker", GetObj(AISystem, "AIPopulationTracker"));

		// the goal manager and the spawning policy manager when the game made none. Not the AI director: once it
		// could be spawned here, activating it crashed some matches at their start (a write near address 0xba0)

		if (!GetObj(GameMode, "AIGoalManager"))
			if (auto Cls = FindObject<UClass>(L"/Script/FortniteGame.FortAIGoalManager"))
				SetObj(GameMode, "AIGoalManager", World->SpawnActor<AActor>(Cls));

		if (GameMode->GetOffset("SpawningPolicyManager", false) != -1 && !GetObj(GameMode, "SpawningPolicyManager"))
			if (auto Cls = FindObject<UClass>(L"/Script/FortniteGame.FortAthenaSpawningPolicyManager"))
				if (auto Policy = World->SpawnActor<AActor>(Cls))
				{
					SetObj(GameMode, "SpawningPolicyManager", Policy);
					SetObj(Policy, "GameModeAthena", GameMode);
					SetObj(Policy, "GameStateAthena", GameState);
				}

		auto Count = [&](const wchar_t* Path) {
			auto Cls = FindObject<UClass>(Path);
			if (!Cls)
				return -1;
			auto All = UGameplayStatics::GetAllActorsOfClass(World, Cls);
			int N = All.Num();
			All.Free();
			return N;
		};

		LOG_INFO(LogBots, "[fnoffline] Phoebe setup: mutators{}, used {}; AISettings {}, services{}; tracker {}; director {}, goal manager {}, spawning policy {}",
			MutatorNames, NameOf(Mutator), NameOf(AISettings), Services, NameOf(GetObj(BotManager, "CachedAIPopulationTracker")), NameOf(GetObj(GameMode, "AIDirector")),
			NameOf(GetObj(GameMode, "AIGoalManager")), NameOf(GetObj(GameMode, "SpawningPolicyManager")));
		LOG_INFO(LogBots, "[fnoffline] navigation actors: RecastNavMesh {}, AthenaNavMesh {}, NavMeshBoundsVolume {}", Count(L"/Script/NavigationSystem.RecastNavMesh"),
			Count(L"/Script/FortniteGame.AthenaNavMesh"), Count(L"/Script/NavigationSystem.NavMeshBoundsVolume"));
	}

	// Difficulty 1-10 -> the Phoebe skill value its curve tables (Phoebe_SkillSets) are keyed by: 0-3000, keys at 800,
	// 1100, 1250, 1400, 1800, 2200, 2400 and 3000. The game's own bots spawn at 1000-1400 (PhoebeMMRSpawning) and the
	// spawner data's default is 1400, here difficulty 6. Reaction, aim tracking, building, evasion all follow it.
	static float BotSkillValue(int Difficulty)
	{
		static const float Values[10] = { 400, 800, 1000, 1100, 1250, 1400, 1800, 2200, 2400, 3000 };
		return Difficulty >= 1 && Difficulty <= 10 ? Values[Difficulty - 1] : 0;
	}

	// UFortServerBotManagerAthena::SpawnAI(location, rotation, component list of BP_AISpawnerData_Phoebe), or null
	static APawn* SpawnFromSpawnerData(const FTransform& SpawnTransform)
	{
		static auto BGA = FindObject<UClass>(L"/Script/Engine.BlueprintGeneratedClass");
		static auto SpawnerDataClass = LoadObject<UClass>(L"/Game/Athena/AI/Phoebe/BP_AISpawnerData_Phoebe.BP_AISpawnerData_Phoebe_C", BGA);
		static auto SpawnerDataBase = FindObject<UClass>(L"/Script/FortniteGame.FortAthenaAISpawnerData");
		static auto CreateListFn = FindObject<UFunction>(L"/Script/FortniteGame.FortAthenaAISpawnerData.CreateComponentListFromClass");
		static auto SpawnAIFn = FindObject<UFunction>(L"/Script/FortniteGame.FortServerBotManagerAthena.SpawnAI");
		static bool bLogged = false;

		if (!bLogged)
		{
			bLogged = true;
			LOG_INFO(LogBots, "[fnoffline] AI spawner data {}, CreateComponentListFromClass {}, SpawnAI {}", bool(SpawnerDataClass), bool(CreateListFn), bool(SpawnAIFn));
		}

		auto GameMode = Cast<AFortGameModeAthena>(GetWorld()->GetGameMode());
		auto BotManager = GameMode ? GameMode->GetServerBotManager() : nullptr;

		if (!SpawnerDataClass || !SpawnerDataBase || !CreateListFn || !SpawnAIFn || !BotManager)
			return nullptr;

		// Once, before the first bot: what the game's own bot setup leaves in place on a real server (the playlist's
		// Phoebe mutator in the bot manager, the playlist's AI settings, the population tracker, the AI director and
		// goal manager), so the spawner data components find the bot manager, the skill set providers and start BT_Phoebe.
		static bool bPhoebeReady = false;

		if (!bPhoebeReady)
		{
			bPhoebeReady = true;
			SetupPhoebe(GameMode, BotManager);
		}

		static int ListClassOff = CreateListFn->GetOffsetFunc("AISpawnerDataClass");
		static int ListOuterOff = CreateListFn->GetOffsetFunc("OuterObject");
		static int ListRetOff = CreateListFn->GetOffsetFunc("ReturnValue");
		static int LocOff = SpawnAIFn->GetOffsetFunc("InSpawnLocation");
		static int RotOff = SpawnAIFn->GetOffsetFunc("InSpawnRotation");
		static int AIListOff = SpawnAIFn->GetOffsetFunc("AISpawnerComponentList");
		static int AIRetOff = SpawnAIFn->GetOffsetFunc("ReturnValue");

		if (ListClassOff < 0 || ListOuterOff < 0 || ListRetOff < 0 || LocOff < 0 || RotOff < 0 || AIListOff < 0 || AIRetOff < 0)
		{
			LOG_WARN(LogBots, "[fnoffline] SpawnAI parameters not found ({} {} {} / {} {} {} {})", ListClassOff, ListOuterOff, ListRetOff, LocOff, RotOff, AIListOff, AIRetOff);
			return nullptr;
		}

		// the difficulty: the skill set component the list is made from (its class default) carries the skill
		static bool bSkillSet = false;

		if (!bSkillSet && Globals::FnOfflineBotSkill > 0)
		{
			bSkillSet = true;
			auto SkillCompClass = LoadObject<UClass>(L"/Game/Athena/AI/Phoebe/AISpawnerComponent/BP_AISpawnerComp_SkillSet_Phoebe.BP_AISpawnerComp_SkillSet_Phoebe_C", BGA);
			auto SkillComp = SkillCompClass ? SkillCompClass->CreateDefaultObject() : nullptr;
			int SkillOff = SkillComp ? SkillComp->GetOffset("Skill", false) : -1;

			if (SkillOff != -1)
			{
				float Old = SkillComp->Get<float>(SkillOff);
				SkillComp->Get<float>(SkillOff) = BotSkillValue(Globals::FnOfflineBotSkill);
				LOG_INFO(LogBots, "[fnoffline] bot difficulty {}: skill {:.0f} (was {:.0f})", Globals::FnOfflineBotSkill, BotSkillValue(Globals::FnOfflineBotSkill), Old);
			}
			else
				LOG_WARN(LogBots, "[fnoffline] bot difficulty {}: no skill set component", Globals::FnOfflineBotSkill);
		}

		alignas(16) uint8_t ListParams[64] = {};
		*(UClass**)(ListParams + ListClassOff) = SpawnerDataClass;
		*(UObject**)(ListParams + ListOuterOff) = GetWorld();
		SpawnerDataBase->CreateDefaultObject()->ProcessEvent(CreateListFn, ListParams);
		auto List = *(UObject**)(ListParams + ListRetOff);

		if (!List)
		{
			LOG_WARN(LogBots, "[fnoffline] CreateComponentListFromClass returned nothing");
			return nullptr;
		}

		alignas(16) uint8_t AIParams[96] = {};
		*(FVector*)(AIParams + LocOff) = SpawnTransform.Translation;
		*(FRotator*)(AIParams + RotOff) = SpawnTransform.Rotation.Rotator();
		*(UObject**)(AIParams + AIListOff) = List;
		BotManager->ProcessEvent(SpawnAIFn, AIParams);
		auto NewPawn = *(APawn**)(AIParams + AIRetOff);
		LOG_INFO(LogBots, "[fnoffline] SpawnAI: {}", NewPawn ? NewPawn->GetFullName() : std::string("nothing"));

		// the difficulty also on the controller (the mutator's MMR spawning sets 1000-1300 there)
		if (auto AIC = NewPawn && Globals::FnOfflineBotSkill > 0 ? NewPawn->GetController() : nullptr)
		{
			static int CSkillOff = AIC->GetOffset("Skill", false);
			if (CSkillOff != -1)
				AIC->Get<float>(CSkillOff) = BotSkillValue(Globals::FnOfflineBotSkill);
		}

		AddNavInvoker(NewPawn);

		// the controller has BT_Phoebe and its blackboard but no brain: nothing started the tree
		FnOfflineStartBrain(NewPawn ? NewPawn->GetController() : nullptr, "at spawn");

		return NewPawn;
	}

	// Blackboard keys of BT_Phoebe by name (UBlackboardComponent.SetValueAs* / GetValueAs*); nothing happens when
	// the blackboard has no such key.
	static void BBCall(UObject* BB, const char* FnName, const wchar_t* Key, const char* ValueParam, void* Value, int Size, bool bGet)
	{
		auto Fn = BB ? FindObject<UFunction>((std::wstring(L"/Script/AIModule.BlackboardComponent.") + std::wstring(FnName, FnName + strlen(FnName))).c_str()) : nullptr;

		if (!Fn)
			return;

		alignas(16) uint8_t Params[64] = {};
		*(FName*)(Params + Fn->GetOffsetFunc("KeyName")) = UKismetStringLibrary::Conv_StringToName(Key);
		int ValueOff = Fn->GetOffsetFunc(ValueParam);

		if (!bGet)
			memcpy(Params + ValueOff, Value, Size);

		BB->ProcessEvent(Fn, Params);

		if (bGet)
			memcpy(Value, Params + ValueOff, Size);
	}

	static void BBSetVector(UObject* BB, const wchar_t* Key, FVector V) { BBCall(BB, "SetValueAsVector", Key, "VectorValue", &V, sizeof(V), false); }
	static void BBSetEnum(UObject* BB, const wchar_t* Key, uint8_t V) { BBCall(BB, "SetValueAsEnum", Key, "EnumValue", &V, 1, false); }
	static void BBSetBool(UObject* BB, const wchar_t* Key, bool V) { BBCall(BB, "SetValueAsBool", Key, "BoolValue", &V, 1, false); }
	static FVector BBGetVector(UObject* BB, const wchar_t* Key) { FVector V{}; BBCall(BB, "GetValueAsVector", Key, "ReturnValue", &V, sizeof(V), true); return V; }
	static uint8_t BBGetEnum(UObject* BB, const wchar_t* Key) { uint8_t V = 0; BBCall(BB, "GetValueAsEnum", Key, "ReturnValue", &V, 1, true); return V; }
	static bool BBGetBool(UObject* BB, const wchar_t* Key) { bool V = false; BBCall(BB, "GetValueAsBool", Key, "ReturnValue", &V, 1, true); return V; }

	static bool ReadBool(UObject* O, const char* Name)
	{
		auto Prop = O ? O->GetProperty(Name, false) : nullptr;
		return Prop && O->ReadBitfieldValue(O->GetOffset(Name), GetFieldMask(Prop));
	}

	static void WriteBool(UObject* O, const char* Name, bool V)
	{
		if (auto Prop = O ? O->GetProperty(Name, false) : nullptr)
			O->SetBitfieldValue(O->GetOffset(Name), GetFieldMask(Prop), V);
	}

	// an unset blackboard vector reads as FLT_MAX (FAISystem::InvalidLocation)
	static bool IsSetLocation(const FVector& V) { return std::abs(V.X) < 1e30f && (V.X != 0 || V.Y != 0); }

	static AActor* FirstAircraft()
	{
		static auto AircraftClass = FindObject<UClass>(L"/Script/FortniteGame.FortAthenaAircraft");
		auto All = UGameplayStatics::GetAllActorsOfClass(GetWorld(), AircraftClass);
		return All.Num() > 0 ? All.at(0) : nullptr;
	}

	// The game's own bots ride the bus too: the game mode only boards player controllers, so the AI bots are marked
	// as on the bus when it starts (their pawns wait where they are), and each jumps at a random time while the bus
	// is over the island (all of them by the time it leaves): moved under the bus and set skydiving, with the
	// blackboard's dive and glide destination at its landing spot.
	struct BusRide { double JumpAt = 0; bool bJumped = false; };

	static void BusTick(AController* C, APawn* Pawn, AFortPlayerStateAthena* PS, UObject* BB, AFortGameStateAthena* GameState, double Now, const FVector& Dest)
	{
		static std::map<AController*, BusRide> Rides;
		auto& Ride = Rides[C];
		auto Phase = GameState->GetGamePhase();
		auto Step = (uint8_t)GameState->GetGamePhaseStep();

		if (Ride.bJumped || Phase != EAthenaGamePhase::Aircraft)
		{
			if (Phase > EAthenaGamePhase::Aircraft && !Ride.bJumped && PS->IsInAircraft())
				Ride.JumpAt = Now;   // the bus left: everyone out
			else
				return;
		}

		if (!PS->IsInAircraft())
		{
			WriteBool(PS, "bInAircraft", true);
			Ride.JumpAt = 0;
		}

		if (Ride.JumpAt == 0 && Step == 5)   // EAthenaGamePhaseStep::BusFlying: over the island
			Ride.JumpAt = Now + 2 + std::rand() % 20;

		if (Ride.JumpAt == 0 || Now < Ride.JumpAt)
			return;

		auto Aircraft = FirstAircraft();

		if (!Aircraft)
			return;

		static auto SkydiveFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerPawn.BeginSkydiving");
		auto At = Aircraft->GetActorLocation();
		At.X += (float)(std::rand() % 1000 - 500);
		At.Y += (float)(std::rand() % 1000 - 500);
		At.Z -= 700;

		// the bots glide all the way down (about 5 m/s down, 20 m/s ahead): they leave the bus only as high as
		// they need to reach their landing spot, or they would land minutes after the players
		float Ahead = (float)std::sqrt(std::pow(Dest.X - At.X, 2) + std::pow(Dest.Y - At.Y, 2));
		At.Z = (std::min)(At.Z, Dest.Z + (std::max)(12000.f, Ahead * 0.3f));
		Pawn->TeleportTo(At, FRotator());
		WriteBool(PS, "bInAircraft", false);
		bool bFromBus = true;

		if (SkydiveFn)
			Pawn->ProcessEvent(SkydiveFn, &bFromBus);

		BBSetBool(BB, L"AIEvaluator_Global_IsInBus", false);
		BBSetVector(BB, L"AIEvaluator_Dive_Destination", Dest);
		BBSetVector(BB, L"AIEvaluator_Glide_Destination", Dest);
		BBSetEnum(BB, L"AIEvaluator_Dive_ExecutionStatus", 4);   // EExecutionStatus::ExecutionAllowed
		BBSetEnum(BB, L"AIEvaluator_Glide_ExecutionStatus", 1);  // ExecutionDenied
		BBSetBool(BB, L"AIEvaluator_Global_HasEverJumpedFromBusKey", true);
		BBSetBool(BB, L"AIEvaluator_Global_HasEverJumpedFromBusAndLandedKey", false);
		Ride.bJumped = true;

		static int Logged = 0;
		if (Logged++ < 2)
		{
			auto D = BBGetVector(BB, L"AIEvaluator_Dive_Destination");
			LOG_INFO(LogBots, "[fnoffline] {} jumped at {:.0f} {:.0f} {:.0f} towards {:.0f} {:.0f} {:.0f} (blackboard {:.0f} {:.0f}), skydiving {}{}", NameOf(C), At.X, At.Y, At.Z,
				Dest.X, Dest.Y, Dest.Z, D.X, D.Y, ReadBool(Pawn, "bIsSkydiving"), SkydiveFn ? "" : " (no BeginSkydiving)");
		}
	}

	// A landing spot on the island: a random building foundation (a named place or a house) above the sea.
	static FVector RandomLandingSpot()
	{
		static std::vector<FVector> Spots;

		if (Spots.empty())
		{
			static auto FoundationClass = FindObject<UClass>(L"/Script/FortniteGame.BuildingFoundation");
			auto All = UGameplayStatics::GetAllActorsOfClass(GetWorld(), FoundationClass);

			for (int i = 0; i < All.Num(); i++)
			{
				auto L = All.at(i) ? All.at(i)->GetActorLocation() : FVector();
				if (All.at(i) && L.Z > -500 && std::abs(L.X) < 140000 && std::abs(L.Y) < 140000 && (L.X != 0 || L.Y != 0))
					Spots.push_back(L);
			}

			LOG_INFO(LogBots, "[fnoffline] {} landing spots from {} foundations", Spots.size(), All.Num());
		}

		return Spots.empty() ? FVector() : Spots[std::rand() % Spots.size()];
	}

	// After landing: the weapon back in hand (skydiving holsters it). For the first bot, developer checks of the ground
	// movement: is it on the navigation mesh, and does a plain move order 15 m away get a path.
	static void OnLanded(AController* C, APawn* Pawn, UObject* BB, UObject* MainNav)
	{
		static auto InventoryOffset = C->GetOffset("Inventory", false);
		auto Inventory = InventoryOffset != -1 ? C->Get<AFortInventory*>(InventoryOffset) : nullptr;
		auto FortPawn = Cast<AFortPawn>(Pawn);
		std::string InHand = "nothing";

		if (Inventory && FortPawn)
		{
			auto& Items = Inventory->GetItemList().GetItemInstances();
			UFortItem* Best = nullptr;

			// a gun, else the pickaxe (never the edit tool or a building piece, which are weapons to the game too)
			for (int i = 0; i < Items.Num(); i++)
			{
				auto Item = Items.at(i);
				auto Def = Item ? Item->GetItemEntry()->GetItemDefinition() : nullptr;
				if (IsRanged(Def))
				{
					Best = Item;
					break;
				}
				if (!Best && Def && Def->IsA(UFortWeaponMeleeItemDefinition::StaticClass()))
					Best = Item;
			}

			if (Best)
				if (auto W = FortPawn->EquipWeaponDefinition((UFortWeaponItemDefinition*)Best->GetItemEntry()->GetItemDefinition(), Best->GetItemEntry()->GetItemGuid()))
					InHand = W->GetName();
		}

		static int Count = 0;

		if (Count++ > 0)
			return;

		auto L = Pawn->GetActorLocation();
		static auto ProjectFn = FindObject<UFunction>(L"/Script/NavigationSystem.NavigationSystemV1.K2_ProjectPointToNavigation");
		static auto NavClass = FindObject<UClass>(L"/Script/NavigationSystem.NavigationSystemV1");
		std::string OnMesh = "no function";

		if (ProjectFn && NavClass)
		{
			alignas(16) uint8_t P[96] = {};
			*(UObject**)(P + ProjectFn->GetOffsetFunc("WorldContextObject")) = Pawn;
			*(FVector*)(P + ProjectFn->GetOffsetFunc("Point")) = L;
			*(UObject**)(P + ProjectFn->GetOffsetFunc("NavData")) = MainNav;
			*(FVector*)(P + ProjectFn->GetOffsetFunc("QueryExtent")) = FVector{ 200, 200, 400 };
			NavClass->CreateDefaultObject()->ProcessEvent(ProjectFn, P);
			auto Proj = *(FVector*)(P + ProjectFn->GetOffsetFunc("ProjectedLocation"));
			OnMesh = *(bool*)(P + ProjectFn->GetOffsetFunc("ReturnValue")) ? "yes at " + std::to_string((int)Proj.Z) : "no";
		}

		static auto ActivateFn = FindObject<UFunction>(L"/Script/Engine.ActorComponent.Activate");
		auto PFC = GetObj(C, "PathFollowingComponent");
		bool bReset = true;
		if (PFC && ActivateFn)
			PFC->ProcessEvent(ActivateFn, &bReset);

		static auto MoveFn = FindObject<UFunction>(L"/Script/AIModule.AIController.MoveToLocation");
		static auto StatusFn = FindObject<UFunction>(L"/Script/AIModule.AIController.GetMoveStatus");
		int Result = -1, Status = -1;

		if (MoveFn)
		{
			alignas(16) uint8_t P[64] = {};
			*(FVector*)(P + MoveFn->GetOffsetFunc("Dest")) = FVector{ L.X + 1500, L.Y, L.Z };
			*(float*)(P + MoveFn->GetOffsetFunc("AcceptanceRadius")) = 50;
			*(bool*)(P + MoveFn->GetOffsetFunc("bUsePathfinding")) = true;
			*(bool*)(P + MoveFn->GetOffsetFunc("bProjectDestinationToNavigation")) = true;
			*(bool*)(P + MoveFn->GetOffsetFunc("bAllowPartialPath")) = true;
			C->ProcessEvent(MoveFn, P);
			Result = *(uint8_t*)(P + MoveFn->GetOffsetFunc("ReturnValue"));
		}

		if (StatusFn)
		{
			uint8_t S[8] = {};
			C->ProcessEvent(StatusFn, S);
			Status = S[0];
		}

		LOG_INFO(LogBots, "[fnoffline] {} landed at {:.0f} {:.0f} {:.0f}: in hand {}, on the navigation mesh {}, move 15 m: request {} (0 failed, 1 there, 2 ok), status {} (0 idle, 1 waiting, 2 paused, 3 moving)",
			NameOf(C), L.X, L.Y, L.Z, InHand, OnMesh, Result, Status);
	}

	// The start of the native function that logs a given text (its string reference, then the function's unwind
	// entry in the executable's exception table, following chained entries to the primary one); null if not found.
	static void* NativeThatLogs(const wchar_t* Text)
	{
		auto Ref = Memcury::Scanner::FindStringRef(Text, false, 0).Get();
		return Ref ? FunctionStartOf((void*)Ref) : nullptr;
	}

	// the start of the native function holding an address: its unwind entry in the executable's exception table,
	// following chained entries to the primary one
	static void* FunctionStartOf(void* Ref)
	{
		DWORD64 Base = 0;
		auto Entry = RtlLookupFunctionEntry((DWORD64)Ref, &Base, nullptr);

		for (int Guard = 0; Entry && Guard < 8; Guard++)
		{
			auto Info = (uint8_t*)(Base + Entry->UnwindData);
			if (!((Info[0] >> 3) & 4))   // UNW_FLAG_CHAININFO
				break;
			int Codes = Info[2];
			Entry = (PRUNTIME_FUNCTION)(Info + 4 + ((Codes + 1) & ~1) * 2);
		}

		return Entry ? (void*)(Base + Entry->BeginAddress) : nullptr;
	}

	// The match start leaves the navigation system's tile building and its octree locked (the log shows the lock
	// removed and put back in the same frame, by code meant for the prebuilt mesh the real servers load), so no tile
	// is ever built around the bots ("being built: no", no reachable point near any bot). UNavigationSystemV1::RemoveNavigationBuildLock(all, Rebuild) and
	// SetNavigationOctreeLock(false), found by the texts they log.
	static void UnlockNavigation(UObject* Nav)
	{
		static auto RemoveLock = (void(*)(UObject*, uint8_t, uint8_t))NativeThatLogs(L"UNavigationSystemV1::RemoveNavigationBuildLock WasLocked=%s IsLocked=%s");
		static auto OctreeLock = (void(*)(UObject*, bool))NativeThatLogs(L"UNavigationSystemV1::SetNavigationOctreeLock IsLocked=%s");
		static bool bLogged = false;

		if (!bLogged)
		{
			bLogged = true;
			auto Exe = __int64(GetModuleHandleW(nullptr));
			auto Hex = [](void* F) { std::string H; char B[4]; for (int k = 0; F && k < 16; k++) { snprintf(B, sizeof(B), "%02x", ((uint8_t*)F)[k]); H += B; } return H; };
			LOG_INFO(LogBots, "[fnoffline] navigation unlock: RemoveNavigationBuildLock exe+0x{:x} {}, SetNavigationOctreeLock exe+0x{:x} {}", RemoveLock ? __int64(RemoveLock) - Exe : 0,
				Hex((void*)RemoveLock), OctreeLock ? __int64(OctreeLock) - Exe : 0, Hex((void*)OctreeLock));
		}

		if (!Nav)
			return;

		if (OctreeLock)
			OctreeLock(Nav, false);

		if (RemoveLock)
			RemoveLock(Nav, 0xFF, 0);   // every lock flag, ELockRemovalRebuildAction::Rebuild
	}

	// BT_Phoebe reads the match from its blackboard: the game phase, whether the bot is on the bus, where to jump
	// to and whether it has jumped and landed. In the real game the bot manager and the AI director keep those up
	// to date; this server skips that setup, so they are written here once a second (as the 17.30 servers that run
	// the game's bots do). Also points each bot's path following at the navigation mesh once there is one.
	static std::map<AController*, bool>& LandedBots()
	{
		static std::map<AController*, bool> Landed;
		return Landed;
	}

	// Ground movement for the game AI bots. The 14.30 client build cannot build navigation tiles at run time (every
	// tile it rasterizes comes out empty), so the behaviour tree's own move orders all fail and the bots stand
	// still after landing. Until a navigation mesh exists, landed bots are steered directly (as the 17.30 servers
	// do with their own move tasks): toward an enemy in sight within 60 m (stopping at 8 m with a gun, 1.5 m with
	// the pickaxe, facing it), else toward a building area within 150 m; a jump when blocked, a side step when
	// still blocked, a new goal when stuck. The game's own tree keeps choosing to aim and fire.
	struct SteerState
	{
		FVector Goal{}, LastLoc{}, Side{};
		bool bHasGoal = false;
		AActor* Enemy = nullptr;
		double NextThink = 0, SideUntil = 0, GoalSince = 0;
		int Blocked = 0;
		bool bMelee = false, bFiring = false;
		double EngagedSince = 0, FireUntil = 0, NextBurst = 0, NextAimRoll = 0;
		FVector AimOffset{};
		void* LootTarget = nullptr;
		double LootSince = 0;
		std::vector<void*> LootSkip;   // pickups it could not reach in 20 s
		double HoldUntil = 0;          // building: stand still (boxed in) until then
	};

	// Firing for the game AI bots. The tree aims but never pulls the trigger: its aiming and range-attack skill sets
	// find no data on this build ("couldn't find data for this weapon", "Couldn't find a valid AI Runtime Parameters
	// provider") and fall back to defaults that never fire. Until those load, a bot that sees an enemy fires in
	// bursts. The difficulty (1-10) sets the reaction time (1.5 s to 0.3 s), the aim error (7 to 1 degrees, rolled
	// again every 0.4 s) and the bursts (0.3 to 0.8 s, pauses 0.9 to 0.3 s). Not from the game's skill tables:
	// not verified against them.
	struct FireSkill { float Reaction, SpreadDeg, Burst, Pause; };

	static FireSkill FireSkillFor(int Difficulty)
	{
		float t = ((Difficulty > 0 ? Difficulty : 6) - 1) / 9.f;
		return { 1.5f - 1.2f * t, 7.f - 6.f * t, 0.3f + 0.5f * t, 0.9f - 0.6f * t };
	}

	static float Rand01() { return (std::rand() % 10001) / 10000.f; }

	// an object still in the object array and not pending kill: dead bots' controllers and pawns are freed while
	// the bot list and the steering still hold them
	static bool LiveObj(UObject* O)
	{
		if (!O || !O->IsValidLowLevel())
			return false;
		auto Item = ChunkedObjects ? ChunkedObjects->GetItemByIndex(O->InternalIndex) : UnchunkedObjects ? UnchunkedObjects->GetItemByIndex(O->InternalIndex) : nullptr;
		return Item && !Item->IsPendingKill();
	}

	// Looting for the game AI bots (the tree's looting moves need the navigation mesh too): a bot without a usable
	// gun walks to the nearest weapon or ammo on the ground within 50 m and takes it when it stands on it, as the
	// players' pickup does (into its inventory, the pickup removed); a gun it takes goes in its hand.
	static AFortInventory* InventoryOf(AController* C)
	{
		static int Off = C ? C->GetOffset("Inventory", false) : -1;
		return Off != -1 ? C->Get<AFortInventory*>(Off) : nullptr;
	}

	// a gun: a ranged weapon fed by separate ammo (thrown items such as grenades are their own ammo)
	static bool IsRanged(UFortItemDefinition* Def)
	{
		static auto RangedClass = FindObject<UClass>(L"/Script/FortniteGame.FortWeaponRangedItemDefinition");
		if (!Def || !RangedClass || !Def->IsA(RangedClass))
			return false;
		auto Ammo = ((UFortWeaponItemDefinition*)Def)->GetAmmoData();
		return Ammo && Ammo != Def;
	}

	static int CountOf(AFortInventory* Inv, UFortItemDefinition* Def)
	{
		int N = 0;
		auto& Items = Inv->GetItemList().GetItemInstances();
		for (int i = 0; Def && i < Items.Num(); i++)
			if (Items.at(i) && Items.at(i)->GetItemEntry()->GetItemDefinition() == Def)
				N += Items.at(i)->GetItemEntry()->GetCount();
		return N;
	}

	// a ranged weapon with a loaded or carried round
	static bool HasUsableGun(AFortInventory* Inv)
	{
		auto& Items = Inv->GetItemList().GetItemInstances();
		for (int i = 0; i < Items.Num(); i++)
		{
			auto E = Items.at(i) ? Items.at(i)->GetItemEntry() : nullptr;
			auto Def = E ? E->GetItemDefinition() : nullptr;
			if (!IsRanged(Def))
				continue;
			auto Ammo = ((UFortWeaponItemDefinition*)Def)->GetAmmoData();
			if (E->GetLoadedAmmo() > 0 || CountOf(Inv, Ammo) > 0)
				return true;
		}
		return false;
	}

	// what a bot without a usable gun goes for: a ranged weapon, or ammo for a gun it carries
	static bool WantsPickup(AFortInventory* Inv, UFortItemDefinition* Def)
	{
		if (!Def)
			return false;
		if (IsRanged(Def))
			return true;
		auto& Items = Inv->GetItemList().GetItemInstances();
		for (int i = 0; i < Items.Num(); i++)
		{
			auto D = Items.at(i) ? Items.at(i)->GetItemEntry()->GetItemDefinition() : nullptr;
			if (IsRanged(D) && ((UFortWeaponItemDefinition*)D)->GetAmmoData() == Def)
				return true;
		}
		return false;
	}

	static void TakePickup(AController* C, AFortPawn* Pawn, AFortInventory* Inv, AFortPickup* Pickup)
	{
		static auto DestroyFn = FindObject<UFunction>(L"/Script/Engine.Actor.K2_DestroyActor");
		auto E = Pickup->GetPrimaryPickupItemEntry();
		auto Def = E ? E->GetItemDefinition() : nullptr;

		if (!Def || E->GetCount() <= 0)
			return;

		auto Added = Inv->AddItem(Def, nullptr, E->GetCount(), E->GetLoadedAmmo());
		Inv->Update();
		static int Logged = 0;
		if (Logged++ < 6)
			LOG_INFO(LogBots, "[fnoffline] loot: {} took {} x{}", Pawn->GetName(), Def->GetName(), E->GetCount());

		if (DestroyFn)
			Pickup->ProcessEvent(DestroyFn, nullptr);

		if (IsRanged(Def) && !Added.first.empty() && Added.first[0])
			Pawn->EquipWeaponDefinition((UFortWeaponItemDefinition*)Def, Added.first[0]->GetItemEntry()->GetItemGuid());
	}

	static float Dist2D(const FVector& A, const FVector& B) { return std::sqrt((A.X - B.X) * (A.X - B.X) + (A.Y - B.Y) * (A.Y - B.Y)); }

	static FVector NearbyLandingSpot(const FVector& From, float Radius)
	{
		for (int Try = 0; Try < 24; Try++)
		{
			auto Spot = RandomLandingSpot();
			auto D = Dist2D(Spot, From);
			if (D > 1500 && D < Radius)
				return Spot;
		}

		// none near: a point 50-150 m away in a random direction
		float A = (std::rand() % 3600) * 3.14159265f / 1800.f, R = 5000.f + std::rand() % 10000;
		return FVector{ From.X + std::cos(A) * R, From.Y + std::sin(A) * R, From.Z };
	}

	static std::map<AController*, SteerState>& SteerStates()
	{
		static std::map<AController*, SteerState> States;
		return States;
	}

	// developer: what the steering of one bot is doing
	static std::string SteerInfo(AController* C, APawn* Pawn)
	{
		auto It = SteerStates().find(C);
		std::string Mode = "?";
		if (auto CMC = GetObj(Pawn, "CharacterMovement"); CMC && CMC->GetOffset("MovementMode", false) != -1)
			Mode = std::to_string(CMC->Get<uint8_t>(CMC->GetOffset("MovementMode", false)));
		if (It == SteerStates().end())
			return "landed " + std::to_string(LandedBots()[C]) + ", not steered, movement mode " + Mode;
		auto& S = It->second;
		auto L = Pawn->GetActorLocation();
		char B[200];
		snprintf(B, sizeof(B), "goal %s %.0f m, enemy %s, loot %s, blocked %d, firing %d, movement mode %s", S.bHasGoal ? "yes" : "no", Dist2D(S.Goal, L) / 100.f,
			S.Enemy ? "yes" : "no", S.LootTarget ? "yes" : "no", S.Blocked, (int)S.bFiring, Mode.c_str());
		return B;
	}

	static void SteerTick(std::vector<PlayerBot>& Bots)
	{
		auto World = GetWorld();
		double Now = UGameplayStatics::GetTimeSeconds(World);
		auto& States = SteerStates();
		static std::vector<AFortPawn*> Pawns;
		static double NextPawns = 0;
		static auto PawnClassAthena = FindObject<UClass>(L"/Script/FortniteGame.FortPlayerPawnAthena");
		static auto MoveFn = FindObject<UFunction>(L"/Script/Engine.Pawn.AddMovementInput");
		static auto JumpFn = FindObject<UFunction>(L"/Script/Engine.Character.Jump");
		static auto FocusFn = FindObject<UFunction>(L"/Script/AIModule.AIController.K2_SetFocus");
		static auto FocalPointFn = FindObject<UFunction>(L"/Script/AIModule.AIController.K2_SetFocalPoint");
		static auto SightFn = FindObject<UFunction>(L"/Script/Engine.Controller.LineOfSightTo");

		if (!MoveFn)
			return;

		static std::vector<AFortPickup*> Pickups;
		static double NextPickups = 0;

		if (Now >= NextPickups)
		{
			NextPickups = Now + 3;
			Pickups.clear();
			auto All = UGameplayStatics::GetAllActorsOfClass(World, AFortPickup::StaticClass());
			for (int i = 0; i < All.Num(); i++)
				if (auto P = LiveObj(All.at(i)) ? Cast<AFortPickup>(All.at(i)) : nullptr; P && !P->IsActorBeingDestroyed())
					Pickups.push_back(P);
			All.Free();
			static int Logged = 0;
			if (Logged++ < 2)
				LOG_INFO(LogBots, "[fnoffline] loot: {} pickups on the ground", Pickups.size());
		}

		if (Now >= NextPawns)
		{
			NextPawns = Now + 1;
			Pawns.clear();
			auto All = UGameplayStatics::GetAllActorsOfClass(World, PawnClassAthena);
			for (int i = 0; i < All.Num(); i++)
				if (auto P = LiveObj(All.at(i)) ? Cast<AFortPawn>(All.at(i)) : nullptr; P && !P->IsActorBeingDestroyed() && P->GetHealth() > 0)
					Pawns.push_back(P);
			All.Free();
		}

		auto& Landed = LandedBots();

		for (auto& Bot : Bots)
		{
			auto C = Bot.Controller;

			if (!LiveObj(C) || C->IsActorBeingDestroyed() || !Landed[C])
				continue;

			auto Pawn = LiveObj(C->GetPawn()) ? Cast<AFortPawn>(C->GetPawn()) : nullptr;

			if (!Pawn || Pawn->IsActorBeingDestroyed() || Pawn->GetHealth() <= 0 || ReadBool(Pawn, "bIsSkydiving") || ReadBool(Pawn, "bIsParachuteOpen"))
				continue;

			auto& S = States[C];
			auto Loc = Pawn->GetActorLocation();

			if (Now >= S.NextThink)
			{
				S.NextThink = Now + 0.5;

				// blocked: less than 30 cm in half a second while walking
				bool bBlocked = S.bHasGoal && Dist2D(Loc, S.LastLoc) < 30;
				S.Blocked = bBlocked ? S.Blocked + 1 : 0;
				S.LastLoc = Loc;

				if (S.Blocked == 1 && JumpFn)
					Pawn->ProcessEvent(JumpFn, nullptr);
				else if (S.Blocked == 3)
				{
					float A = (std::rand() % 2 ? 1.f : -1.f);
					auto D = S.Goal - Loc;
					S.Side = FVector{ -D.Y * A, D.X * A, 0 };
					S.SideUntil = Now + 1.2;
				}
				else if (S.Blocked >= 12)
					S.bHasGoal = false, S.Blocked = 0;

				// an enemy in sight within 60 m
				auto MyPS = Cast<AFortPlayerStateAthena>(C->GetPlayerState());
				AFortPawn* Best = nullptr;
				float BestD = 6000;

				for (auto Other : Pawns)
				{
					if (Other == Pawn || !LiveObj(Other) || Other->IsActorBeingDestroyed() || Other->GetHealth() <= 0)
						continue;
					auto OtherPS = Cast<AFortPlayerStateAthena>(Other->GetPlayerState());
					if (MyPS && OtherPS && OtherPS->GetTeamIndex() == MyPS->GetTeamIndex())
						continue;
					auto OL = Other->GetActorLocation();
					float D = std::sqrt((OL.X - Loc.X) * (OL.X - Loc.X) + (OL.Y - Loc.Y) * (OL.Y - Loc.Y) + (OL.Z - Loc.Z) * (OL.Z - Loc.Z));
					if (D >= BestD)
						continue;
					if (SightFn)
					{
						alignas(16) uint8_t P[64] = {};
						*(AActor**)(P + SightFn->GetOffsetFunc("Other")) = Other;
						C->ProcessEvent(SightFn, P);
						if (!*(bool*)(P + SightFn->GetOffsetFunc("ReturnValue")))
							continue;
					}
					Best = Other, BestD = D;
				}

				if (Best != S.Enemy)
				{
					S.Enemy = Best;
					S.EngagedSince = 0;
				}

				if (S.Enemy)
				{
					auto Weapon = Pawn->GetCurrentWeapon();
					auto Data = Weapon ? Weapon->GetWeaponData<UObject>() : nullptr;
					bool bMelee = Data && Data->IsA(UFortWeaponMeleeItemDefinition::StaticClass());
					S.bMelee = bMelee;
					float Keep = bMelee ? 150.f : 800.f;
					auto E = S.Enemy->GetActorLocation();
					S.Goal = Dist2D(E, Loc) > Keep ? E : Loc;
					S.bHasGoal = Dist2D(E, Loc) > Keep;
					S.GoalSince = Now;
				}
				else
				{
					// loot: the nearest wanted pickup within 50 m while the bot has no usable gun; taken when on it
					auto Inv = InventoryOf(C);
					AFortPickup* Want = nullptr;

					if (Inv && !HasUsableGun(Inv))
					{
						float BestP = 5000;
						for (auto& P : Pickups)
						{
							if (!P || !LiveObj(P) || P->IsActorBeingDestroyed())
							{
								P = nullptr;
								continue;
							}
							auto PL = P->GetActorLocation();
							float D = Dist2D(PL, Loc);
							if (D < BestP && std::abs(PL.Z - Loc.Z) < 600 && std::find(S.LootSkip.begin(), S.LootSkip.end(), (void*)P) == S.LootSkip.end() &&
								WantsPickup(Inv, P->GetPrimaryPickupItemEntry()->GetItemDefinition()))
								Want = P, BestP = D;
						}

						if (Want && BestP < 200 && std::abs(Want->GetActorLocation().Z - Loc.Z) < 250)
						{
							TakePickup(C, Pawn, Inv, Want);
							for (auto& P : Pickups)
								if (P == Want)
									P = nullptr;
							Want = nullptr;
						}
					}

					if (Want != S.LootTarget)
						S.LootTarget = Want, S.LootSince = Now;
					else if (Want && Now - S.LootSince > 20)
					{
						S.LootSkip.push_back(Want);
						S.LootTarget = nullptr;
						Want = nullptr;
					}

					if (Want)
					{
						if (!S.bHasGoal || Dist2D(S.Goal, Want->GetActorLocation()) > 50)
							S.Blocked = 0, S.GoalSince = Now;
						S.Goal = Want->GetActorLocation();
						S.bHasGoal = true;
					}
					else if (!S.bHasGoal || Dist2D(S.Goal, Loc) < 300 || Now - S.GoalSince > 90)
					{
						S.Goal = NearbyLandingSpot(Loc, 15000);
						S.bHasGoal = true;
						S.GoalSince = Now;
						S.Blocked = 0;
					}

					if (FocalPointFn)
					{
						FVector Look{ S.Goal.X, S.Goal.Y, Loc.Z };
						C->ProcessEvent(FocalPointFn, &Look);
					}
				}
			}

			// aim at the enemy with the difficulty's error, fire in bursts once the reaction time has passed
			static auto StartFireFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPawn.PawnStartFire");
			static auto StopFireFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPawn.PawnStopFire");
			auto Skill = FireSkillFor(Globals::FnOfflineBotSkill);
			bool bWantFire = false;

			if (S.Enemy && !LiveObj(S.Enemy))
				S.Enemy = nullptr;

			if (S.Enemy && !S.Enemy->IsActorBeingDestroyed())
			{
				auto E = S.Enemy->GetActorLocation();
				float D = std::sqrt((E.X - Loc.X) * (E.X - Loc.X) + (E.Y - Loc.Y) * (E.Y - Loc.Y) + (E.Z - Loc.Z) * (E.Z - Loc.Z));

				if (Now >= S.NextAimRoll)
				{
					S.NextAimRoll = Now + 0.4;
					float Err = D * std::tan(Skill.SpreadDeg * 3.14159265f / 180.f) * Rand01();
					float A = Rand01() * 6.2831853f;
					S.AimOffset = FVector{ std::cos(A) * Err, std::sin(A) * Err, (Rand01() - 0.5f) * Err };
				}

				if (FocalPointFn)
				{
					FVector Aim{ E.X + S.AimOffset.X, E.Y + S.AimOffset.Y, E.Z + S.AimOffset.Z };
					C->ProcessEvent(FocalPointFn, &Aim);
				}

				if (S.EngagedSince == 0)
					S.EngagedSince = Now;

				bool bInRange = S.bMelee ? D < 250 : D < 6000;

				if (bInRange && Now - S.EngagedSince >= Skill.Reaction)
				{
					if (Now < S.FireUntil)
						bWantFire = true;
					else if (Now >= S.NextBurst)
					{
						S.FireUntil = Now + Skill.Burst * (0.7f + 0.6f * Rand01());
						S.NextBurst = S.FireUntil + Skill.Pause * (0.7f + 0.6f * Rand01());
						bWantFire = true;
					}
				}
			}
			else
				S.EngagedSince = 0;

			if (bWantFire != S.bFiring && StartFireFn && StopFireFn)
			{
				uint8_t Mode = 0;
				Pawn->ProcessEvent(bWantFire ? StartFireFn : StopFireFn, &Mode);
				S.bFiring = bWantFire;
			}

			if (!S.bHasGoal || Now < S.HoldUntil)
				continue;

			// walking input every frame (it is used up each movement tick)
			FVector Dir = Now < S.SideUntil ? S.Side : S.Goal - Loc;
			Dir.Z = 0;
			float Len = std::sqrt(Dir.X * Dir.X + Dir.Y * Dir.Y);

			if (Len < 1)
				continue;

			struct { FVector WorldDirection; float ScaleValue; bool bForce; } Input{ FVector{ Dir.X / Len, Dir.Y / Len, 0 }, 1.f, true };
			Pawn->ProcessEvent(MoveFn, &Input);
		}
	}

	static void PhoebeTick(std::vector<PlayerBot>& Bots)
	{
		static double Next = 0;
		auto World = GetWorld();
		double Now = UGameplayStatics::GetTimeSeconds(World);

		if (Now < Next)
			return;

		Next = Now + 1;
		auto GameState = Cast<AFortGameStateAthena>(World->GetGameState());
		static int NavOff = World->GetOffset("NavigationSystem", false);
		auto Nav = NavOff != -1 ? World->Get<UObject*>(NavOff) : nullptr;
		static double NextUnlock = 0;

		// once the navigation mesh exists (the game's own match-start release spawns it), and again every 15 s
		// in case the game locks again
		if (Nav && GetObj(Nav, "MainNavData") && Now >= NextUnlock)
		{
			NextUnlock = Now + 15;
			UnlockNavigation(Nav);
		}

		auto MainNav = GetObj(Nav, "MainNavData");
		static auto NavRegisteredFn = FindObject<UFunction>(L"/Script/AIModule.PathFollowingComponent.OnNavDataRegistered");
		static bool bKeysLogged = false;

		for (auto& Bot : Bots)
		{
			auto C = LiveObj(Bot.Controller) ? Bot.Controller : nullptr;
			auto Pawn = C && !C->IsActorBeingDestroyed() && LiveObj(C->GetPawn()) ? C->GetPawn() : nullptr;
			auto PS = C ? Cast<AFortPlayerStateAthena>(C->GetPlayerState()) : nullptr;
			auto BB = GetObj(C, "Blackboard");

			if (!Pawn || !PS || !BB)
				continue;

			if (!bKeysLogged)
			{
				bKeysLogged = true;
				std::string Names;
				static auto EntryStruct = FindObject<UStruct>(L"/Script/AIModule.BlackboardEntry");
				int EntrySize = EntryStruct ? EntryStruct->GetPropertiesSize() : 0;

				for (auto Asset = GetObj(BB, "BlackboardAsset"); Asset && EntrySize; Asset = GetObj(Asset, "Parent"))
				{
					auto& Keys = Asset->Get<TArray<uint8_t>>(Asset->GetOffset("Keys"));
					for (int i = 0; i < Keys.Num(); i++)
						Names += " " + ((FName*)(Keys.Data + i * EntrySize))->ToString();
				}

				LOG_INFO(LogBots, "[fnoffline] blackboard keys:{}", Names);
			}

			BBSetEnum(BB, L"AIEvaluator_Global_GamePhase", (uint8_t)GameState->GetGamePhase());
			BBSetEnum(BB, L"AIEvaluator_Global_GamePhaseStep", (uint8_t)GameState->GetGamePhaseStep());
			BBSetBool(BB, L"AIEvaluator_Global_IsMovementBlocked", false);

			auto Dest = BBGetVector(BB, L"AIEvaluator_JumpOffBus_Destination");

			if (!IsSetLocation(Dest))
			{
				Dest = RandomLandingSpot();
				BBSetVector(BB, L"AIEvaluator_JumpOffBus_Destination", Dest);
			}

			BusTick(C, Pawn, PS, BB, GameState, Now, Dest);
			bool bInBus = PS->IsInAircraft();
			bool bSkydiving = ReadBool(Pawn, "bIsSkydiving") || ReadBool(Pawn, "bIsParachuteOpen");
			BBSetBool(BB, L"AIEvaluator_Global_IsInBus", bInBus);

			// landed: the blackboard key starts out true, so whether this bot landed is kept here
			auto& Landed = LandedBots();
			bool bJumped = BBGetBool(BB, L"AIEvaluator_Global_HasEverJumpedFromBusKey");

			if (!bJumped)
				BBSetBool(BB, L"AIEvaluator_Global_HasEverJumpedFromBusAndLandedKey", false);
			else if (!bSkydiving && !bInBus && !Landed[C])
			{
				Landed[C] = true;
				BBSetBool(BB, L"AIEvaluator_Global_HasEverJumpedFromBusAndLandedKey", true);
				OnLanded(C, Pawn, BB, MainNav);
			}

			BBSetEnum(BB, L"AIEvaluator_RangeAttack_ExecutionStatus", 4);   // ExecutionAllowed, as the 17.30 servers do

			if (auto PFC = GetObj(C, "PathFollowingComponent"); PFC && MainNav && NavRegisteredFn && GetObj(PFC, "MyNavData") != MainNav)
				PFC->ProcessEvent(NavRegisteredFn, &MainNav);
		}
	}

	static bool IsReadyToSpawnBot()
	{
		return PawnClass;
	}

	void SetupInventory()
	{
		auto GameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());
		auto GameMode = Cast<AFortGameModeAthena>(GetWorld()->GetGameMode());

		if (!ShouldUseAIBotController()) // TODO REWRITE
		{
			AFortInventory** Inventory = nullptr;

			if (auto FortPlayerController = Cast<AFortPlayerController>(Controller))
			{
				Inventory = &FortPlayerController->GetWorldInventory();
			}
			else
			{
				if (auto FortAthenaAIBotController = Cast<AFortAthenaAIBotController>(Controller))
				{
					static auto InventoryOffset = Controller->GetOffset("Inventory");
					Inventory = Controller->GetPtr<AFortInventory*>(InventoryOffset);
				}
			}

			if (!Inventory)
			{
				LOG_ERROR(LogBots, "No inventory pointer!");

				Pawn->K2_DestroyActor();
				Controller->K2_DestroyActor();
				return;
			}

			static auto FortInventoryClass = FindObject<UClass>(L"/Script/FortniteGame.FortInventory"); // AFortInventory::StaticClass()
			*Inventory = GetWorld()->SpawnActor<AFortInventory>(FortInventoryClass, FTransform{}, CreateSpawnParameters(ESpawnActorCollisionHandlingMethod::AlwaysSpawn, false, Controller));

			if (!*Inventory)
			{
				LOG_ERROR(LogBots, "Failed to spawn Inventory!");

				Pawn->K2_DestroyActor();
				Controller->K2_DestroyActor();
				return;
			}

			(*Inventory)->GetInventoryType() = EFortInventoryType::World;

			if (auto FortPlayerController = Cast<AFortPlayerController>(Controller))
			{
				static auto bHasInitializedWorldInventoryOffset = FortPlayerController->GetOffset("bHasInitializedWorldInventory");
				FortPlayerController->Get<bool>(bHasInitializedWorldInventoryOffset) = true;
			}

			// if (false)
			{
				if (Inventory)
				{
					auto& StartingItems = GameMode->GetStartingItems();

					for (int i = 0; i < StartingItems.Num(); ++i)
					{
						auto& StartingItem = StartingItems.at(i, FItemAndCount::GetStructSize());

						// TODO: Check if it is FortSmartBuildingItemDefinition

						(*Inventory)->AddItem(StartingItem.GetItem(), nullptr, StartingItem.GetCount());
					}

					if (auto FortPlayerController = Cast<AFortPlayerController>(Controller))
					{
						UFortItem* PickaxeInstance = FortPlayerController->AddPickaxeToInventory();

						if (PickaxeInstance)
						{
							FortPlayerController->ServerExecuteInventoryItemHook(FortPlayerController, PickaxeInstance->GetItemEntry()->GetItemGuid());
						}
					}

					(*Inventory)->Update();
				}
			}
		}
		else if (Globals::bFnOfflineBotsAI) // fnoffline: the game AI bots get an inventory, the match's starting items and the pickaxe in hand
		{
			static auto InventoryOffset = Controller->GetOffset("Inventory", false);
			auto Inventory = InventoryOffset != -1 ? Controller->GetPtr<AFortInventory*>(InventoryOffset) : nullptr;
			static int Logged = 0;
			bool bHad = Inventory && *Inventory;

			if (Inventory && !*Inventory)
			{
				static auto FortInventoryClass = FindObject<UClass>(L"/Script/FortniteGame.FortInventory");
				*Inventory = GetWorld()->SpawnActor<AFortInventory>(FortInventoryClass, FTransform{}, CreateSpawnParameters(ESpawnActorCollisionHandlingMethod::AlwaysSpawn, false, Controller));
				if (*Inventory)
					(*Inventory)->GetInventoryType() = EFortInventoryType::World;
			}

			if (Inventory && *Inventory)
			{
				auto& StartingItems = GameMode->GetStartingItems();

				for (int i = 0; i < StartingItems.Num(); ++i)
				{
					auto& StartingItem = StartingItems.at(i, FItemAndCount::GetStructSize());
					(*Inventory)->AddItem(StartingItem.GetItem(), nullptr, StartingItem.GetCount());
				}

				static auto PickaxeDef = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Harvest_Pickaxe_Athena_C_T01.WID_Harvest_Pickaxe_Athena_C_T01");
				auto Added = PickaxeDef ? (*Inventory)->AddItem(PickaxeDef, nullptr, 1) : decltype((*Inventory)->AddItem(PickaxeDef, nullptr, 1)){};
				(*Inventory)->Update();

				AFortWeapon* Equipped = nullptr;

				if (!Added.first.empty() && Added.first[0])
					Equipped = Pawn->EquipWeaponDefinition((UFortWeaponItemDefinition*)PickaxeDef, Added.first[0]->GetItemEntry()->GetItemGuid());

				// developer (FNOFFLINE_BOT_TEST_GUN=1): a common assault rifle with ammo, to see the bots shoot without looting first
				static bool bTestGun = [] { char V[8] = {}; return GetEnvironmentVariableA("FNOFFLINE_BOT_TEST_GUN", V, sizeof(V)) && V[0] == '1'; }();
				if (bTestGun)
				{
					static auto GunDef = LoadObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Assault_Auto_Athena_C_Ore_T02.WID_Assault_Auto_Athena_C_Ore_T02");
					static auto AmmoDef = LoadObject<UFortItemDefinition>(L"/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsMedium.AthenaAmmoDataBulletsMedium");
					if (AmmoDef)
						(*Inventory)->AddItem(AmmoDef, nullptr, 300);
					auto Gun = GunDef ? (*Inventory)->AddItem(GunDef, nullptr, 1) : decltype(Added){};
					(*Inventory)->Update();
					if (!Gun.first.empty() && Gun.first[0])
						Equipped = Pawn->EquipWeaponDefinition((UFortWeaponItemDefinition*)GunDef, Gun.first[0]->GetItemEntry()->GetItemGuid());
					if (Logged == 0)
						LOG_INFO(LogBots, "[fnoffline] test gun {} ammo {}", GunDef ? GunDef->GetName() : std::string("missing"), AmmoDef ? AmmoDef->GetName() : std::string("missing"));
				}

				if (Logged++ < 2)
					LOG_INFO(LogBots, "[fnoffline] bot inventory: had one {}, now {}, pickaxe added {}, in hand {}", bHad, (*Inventory)->GetName(), Added.first.size(),
						Equipped ? Equipped->GetName() : std::string("nothing"));
			}
		}
	}

	void PickRandomLoadout()
	{
		auto AllHeroTypes = GetAllObjectsOfClass(FindObject<UClass>(L"/Script/FortniteGame.FortHeroType"));
		std::vector<UFortItemDefinition*> AthenaHeroTypes;

		UFortItemDefinition* HeroType = FindObject<UFortItemDefinition>(L"/Game/Athena/Heroes/HID_030_Athena_Commando_M_Halloween.HID_030_Athena_Commando_M_Halloween");

		for (int i = 0; i < AllHeroTypes.size(); ++i)
		{
			auto CurrentHeroType = (UFortItemDefinition*)AllHeroTypes.at(i);

			if (CurrentHeroType->GetPathName().starts_with("/Game/Athena/Heroes/"))
				AthenaHeroTypes.push_back(CurrentHeroType);
		}

		if (AthenaHeroTypes.size())
		{
			HeroType = AthenaHeroTypes.at(std::rand() % AthenaHeroTypes.size());
		}

		static auto HeroTypeOffset = PlayerState->GetOffset("HeroType");
		PlayerState->Get(HeroTypeOffset) = HeroType;
	}

	void ApplyCosmeticLoadout()
	{
		static auto HeroTypeOffset = PlayerState->GetOffset("HeroType");
		const auto CurrentHeroType = PlayerState->Get(HeroTypeOffset);

		if (!CurrentHeroType)
		{
			LOG_WARN(LogBots, "CurrentHeroType called with an invalid HeroType!");
			return;
		}

		ApplyHID(Pawn, CurrentHeroType, true);
	}

	void SetName(const FString& NewName)
	{
		if (// true ||
			Fortnite_Version < 9
			)
		{
			if (auto PlayerController = Cast<APlayerController>(Controller))
			{
				PlayerController->ServerChangeName(NewName);
			}
		}
		else
		{
			auto GameMode = Cast<AFortGameModeAthena>(GetWorld()->GetGameMode());
			GameMode->ChangeName(Controller, NewName, true);
		}

		PlayerState->OnRep_PlayerName(); // ?
	}

	FString GetRandomName()
	{
		static int CurrentBotNum = 1;
		std::wstring BotNumWStr;
		FString NewName;

		if (Fortnite_Version < 9)
		{
			BotNumWStr = std::to_wstring(CurrentBotNum++);
			NewName = (L"Bot" + BotNumWStr).c_str();
		}
		else
		{
			if (Fortnite_Version < 11 || PlayerBotNames.empty())
			{
				BotNumWStr = std::to_wstring(CurrentBotNum++ + 200);
				NewName = (std::format(L"Anonymous[{}]", BotNumWStr)).c_str();
			}
			else
			{
				NewName = PlayerBotNames.back();
				PlayerBotNames.pop_back();
			}
		}

		return NewName;
	}

	void Initialize(const FTransform& SpawnTransform, AActor* InSpawnLocator)
	{
		auto GameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());
		auto GameMode = Cast<AFortGameModeAthena>(GetWorld()->GetGameMode());

		if (!IsReadyToSpawnBot())
		{
			LOG_ERROR(LogBots, "We are not prepared to spawn a bot!");
			return;
		}

		if (!ShouldUseAIBotController())
		{
			Controller = GetWorld()->SpawnActor<AController>(ControllerClass);
			Pawn = GetWorld()->SpawnActor<AFortPlayerPawnAthena>(PawnClass, SpawnTransform, CreateSpawnParameters(ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn));
			PlayerState = Cast<AFortPlayerStateAthena>(Controller->GetPlayerState());
		}
		else
		{
			// fnoffline: the game's own way first (14.x+): the bot manager spawns from the Phoebe spawner data, which
			// brings the behavior tree, blackboard, skill sets and inventory; the mutator's SpawnBot alone left the AI
			// without them ("no valid AI Runtime Parameters provider", invalid blackboard keys every frame).
			Pawn = Cast<AFortPlayerPawnAthena>(SpawnFromSpawnerData(SpawnTransform));

			if (!Pawn && GameMode->GetServerBotManager()->GetCachedBotMutator())
				Pawn = GameMode->GetServerBotManager()->GetCachedBotMutator()->SpawnBot(PawnClass, InSpawnLocator, SpawnTransform.Translation, SpawnTransform.Rotation.Rotator(), false);

			if (!Pawn)
			{
				LOG_ERROR(LogBots, "[fnoffline] no bot pawn: neither the spawner data nor a bot mutator spawned one");
				return;
			}

			if (Fortnite_Version < 17)
				Controller = Cast<AFortAthenaAIBotController>(Pawn->GetController());
			else
				Controller = GetWorld()->SpawnActor<AFortAthenaAIBotController>(Pawn->GetAIControllerClass());

			PlayerState = Cast<AFortPlayerStateAthena>(Controller->GetPlayerState());
		}

		if (!Controller || !Pawn || !PlayerState)
		{
			LOG_ERROR(LogBots, "Failed to spawn controller, pawn or playerstate ({} {})!", bool(__int64(Controller)), bool(__int64(Pawn)), bool(__int64(Controller->GetPlayerState())));
			return;
		}

		PlayerState->SetIsBot(true);

		if (Controller->GetPawn() != Pawn)
		{
			Controller->Possess(Pawn);
		}

		FString BotNewName = GetRandomName();
		
		LOG_INFO(LogBots, "BotNewName: {}", BotNewName.ToString());
		SetName(BotNewName);

		PlayerState->GetTeamIndex() = GameMode->Athena_PickTeamHook(GameMode, 0, Controller);

		static auto SquadIdOffset = PlayerState->GetOffset("SquadId", false);

		if (SquadIdOffset != -1)
			PlayerState->GetSquadId() = PlayerState->GetTeamIndex() - NumToSubtractFromSquadId;

		GameState->AddPlayerStateToGameMemberInfo(PlayerState);

		Pawn->SetHealth(100);
		Pawn->SetMaxHealth(100);

		auto PlayerAbilitySet = GetPlayerAbilitySet();
		auto AbilitySystemComponent = PlayerState->GetAbilitySystemComponent();

		if (PlayerAbilitySet && AbilitySystemComponent)
		{
			PlayerAbilitySet->GiveToAbilitySystem(AbilitySystemComponent);
		}

		SetupInventory();
		PickRandomLoadout();
		ApplyCosmeticLoadout();

		if (!ShouldUseAIBotController())
		{
			++GameState->GetPlayersLeft();
			GameState->OnRep_PlayersLeft();
		}

		if (auto FortPlayerControllerAthena = Cast<AFortPlayerControllerAthena>(Controller))
		{
			GameMode->GetAlivePlayers().Add(FortPlayerControllerAthena);
		}

		LOG_INFO(LogDev, "Finished spawning bot!")
	}
};

inline std::vector<PlayerBot> AllPlayerBotsToTick; // fnoffline: was 'static' (one copy per .cpp, so Tick never saw the bots)

namespace Bots
{
	static AController* SpawnBot(FTransform SpawnTransform, AActor* InSpawnLocator)
	{
		auto playerBot = PlayerBot();
		playerBot.Initialize(SpawnTransform, InSpawnLocator);
		AllPlayerBotsToTick.push_back(playerBot);
		return playerBot.Controller;
	}

	static void SpawnBotsAtPlayerStarts(int AmountOfBots)
	{
		if (AmountOfBots <= 0) // fnoffline: upstream had an unconditional 'return;' here
			return;

		auto GameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());
		auto GameMode = Cast<AFortGameModeAthena>(GetWorld()->GetGameMode());

		static auto FortPlayerStartCreativeClass = FindObject<UClass>(L"/Script/FortniteGame.FortPlayerStartCreative");
		static auto FortPlayerStartWarmupClass = FindObject<UClass>(L"/Script/FortniteGame.FortPlayerStartWarmup");
		TArray<AActor*> PlayerStarts = UGameplayStatics::GetAllActorsOfClass(GetWorld(), Globals::bCreative ? FortPlayerStartCreativeClass : FortPlayerStartWarmupClass);

		int ActorsNum = PlayerStarts.Num();

		// Actors.Free();

		if (ActorsNum == 0)
		{
			// LOG_INFO(LogDev, "No Actors!");
			return;
		}

		// Find playerstart (scuffed)

		for (int i = 0; i < AmountOfBots; ++i)
		{
			AActor* PlayerStart = PlayerStarts.at(std::rand() % PlayerStarts.Num()); // fnoffline: was % (size - 1), div by 0 with one start

			if (!PlayerStart)
			{
				return;
			}

			auto NewBot = SpawnBot(PlayerStart->GetTransform(), PlayerStart);
			if (NewBot) // fnoffline: null check; keep bots damageable
				NewBot->SetCanBeDamaged(true);
		}

		return;
	}

	static void Tick()
	{
		if (AllPlayerBotsToTick.size() == 0)
			return;

		if (PlayerBot::ShouldUseAIBotController() && Fortnite_Version >= 14 && Fortnite_Version < 15)
		{
			PlayerBot::PhoebeTick(AllPlayerBotsToTick);
			PlayerBot::SteerTick(AllPlayerBotsToTick);
		}

		auto GameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());
		auto GameMode = Cast<AFortGameModeAthena>(GetWorld()->GetGameMode());

		// auto AllBuildingContainers = UGameplayStatics::GetAllActorsOfClass(GetWorld(), ABuildingContainer::StaticClass());

		// for (int i = 0; i < GameMode->GetAlivePlayers().Num(); ++i)
		for (auto& PlayerBot : AllPlayerBotsToTick)
		{
			auto CurrentPlayer = PlayerBot.Controller;

			if (!PlayerBot::LiveObj(CurrentPlayer) || CurrentPlayer->IsActorBeingDestroyed()) // fnoffline: null checks (dead bots, freed ones)
				continue;

			auto CurrentPawn = CurrentPlayer->GetPawn();

			if (!PlayerBot::LiveObj(CurrentPawn) || CurrentPawn->IsActorBeingDestroyed())
				continue;

			auto CurrentPlayerState = Cast<AFortPlayerStateAthena>(CurrentPlayer->GetPlayerState());

			if (!CurrentPlayerState 
				// || !CurrentPlayerState->IsBot()
				)
				continue;

			if (GameState->GetGamePhase() == EAthenaGamePhase::Warmup)
			{
				/* if (!CurrentPlayer->IsPlayingEmote())
				{
					static auto AthenaDanceItemDefinitionClass = FindObject<UClass>("/Script/FortniteGame.AthenaDanceItemDefinition");
					auto RandomDanceID = GetRandomObjectOfClass(AthenaDanceItemDefinitionClass);

					CurrentPlayer->ServerPlayEmoteItemHook(CurrentPlayer, RandomDanceID);
				} */
			}	

			if (PlayerBot.bIsAthenaController && CurrentPlayerState->IsInAircraft() && !CurrentPlayerState->HasThankedBusDriver())
			{
				static auto ServerThankBusDriverFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerControllerAthena.ServerThankBusDriver");
				CurrentPlayer->ProcessEvent(ServerThankBusDriverFn);
			}

			if (CurrentPawn && !PlayerBot::ShouldUseAIBotController()) // fnoffline: the game AI bots move on their own
			{
				if (PlayerBot.NextJumpTime <= UGameplayStatics::GetTimeSeconds(GetWorld()))
				{
					static auto JumpFn = FindObject<UFunction>(L"/Script/Engine.Character.Jump");

					CurrentPawn->ProcessEvent(JumpFn);
					PlayerBot.NextJumpTime = UGameplayStatics::GetTimeSeconds(GetWorld()) + (rand() % 4 + 3);
				}
			}

			/* bool bShouldJumpFromBus = CurrentPlayerState->IsInAircraft(); // TODO (Milxnor) add a random percent thing

			if (bShouldJumpFromBus)
			{
				CurrentPlayer->ServerAttemptAircraftJumpHook(CurrentPlayer, FRotator());
			} */
		}

		// AllBuildingContainers.Free();
	}
}

namespace Bosses
{

}
