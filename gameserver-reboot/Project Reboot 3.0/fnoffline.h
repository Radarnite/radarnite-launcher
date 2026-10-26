#pragma once

// fnoffline: configure Reboot from environment variables (all optional).
// Included from dllmain.cpp after gui.h; FnOffline::ApplyEnv() runs right after
// InitLogger(), before any global below is read.
//
//   FNOFFLINE_PLAYLIST              -> PlaylistName (full object path)
//   FNOFFLINE_BOTS                  -> AmountOfBotsToSpawn (0..99), enables bot tick
//   FNOFFLINE_BOTS_AI               -> 1 = spawn bots through the game's bot mutator (Phoebe AI, S11+, experimental)
//   FNOFFLINE_INFINITE_MATS         -> Globals::bInfiniteMaterials, and every player is kept at 999 wood/stone/metal
//                                      (the client only lets you build when it sees materials in the inventory)
//   FNOFFLINE_INFINITE_AMMO         -> Globals::bInfiniteAmmo
//   FNOFFLINE_LATEGAME              -> Globals::bLateGame (via SetIsLategame)
//   FNOFFLINE_CREATIVE              -> Globals::bCreative
//   FNOFFLINE_AUTOSTART_SECONDS     -> >0: start the bus N s after enough players connected (0 = manual, GUI)
//   FNOFFLINE_AUTOSTART_MINPLAYERS  -> connected clients required before the timer starts (default 1)
//   FNOFFLINE_TEST_DEATHS           -> developer, >0: every N s in the warmup, a bot is teleported 5 m in front of the camera of the
//                                      first connected player and killed 2.5 s later as that player's elimination
//                                      (background test runs of the death visuals; never set by the launcher)
//   FNOFFLINE_BOT_SKILL             -> 1-10: the game AI bots' skill (bots.h BotSkillValue); 0 or unset = the game's own
//   FNOFFLINE_BOT_DIAG              -> developer, 1: every 10 s each bot's position, movement, health and weapon
//   FNOFFLINE_TEST_END              -> developer, >0: the first player is eliminated N s after the bus left (test runs)
//   FNOFFLINE_CORPSE_SECONDS        -> >0: life span of an eliminated pawn, so the body stays for the client's ragdoll
//   FNOFFLINE_RESULTS               -> folder for the match result files (default <offline>\results, from LOCALAPPDATA)
//   FNOFFLINE_NOMCP                 -> Globals::bNoMCP (default 0 here: the server reads each player's athena profile
//                                      from the local backend, so the locker loadout and styles reach the match;
//                                      upstream's default 1 gives everyone the default soldier)

#include <Windows.h>
#include <string>
#include <cstdlib>

#include "globals.h"
#include "gui.h"
#include "World.h"
#include "NetDriver.h"
#include "FortGameModeAthena.h"
#include "FortGameStateAthena.h"
#include "FortPlayerController.h"
#include "FortInventory.h"
#include "log.h"
#include "bots.h"
#include "botbuild.h"
#include "GameplayStatics.h"
#include <cmath>
#include <algorithm>
#include <vector>
#include <ctime>
#include <cstdio>

namespace FnOffline
{
	inline int AutoStartSeconds = 0;
	inline int AutoStartMinPlayers = 1;
	inline int TestDeathsSeconds = 0;
	inline int CorpseSeconds = 0;
	inline int TestEndSeconds = 0;
	inline bool BotDiag = false;

	static inline bool GetEnv(const char* Name, std::string& Out)
	{
		char Buf[1024];
		DWORD Len = GetEnvironmentVariableA(Name, Buf, sizeof(Buf));
		if (Len == 0 || Len >= sizeof(Buf))
			return false;
		Out.assign(Buf, Len);
		return true;
	}

	static inline bool GetEnvInt(const char* Name, int& Out)
	{
		std::string S;
		if (!GetEnv(Name, S))
			return false;
		char* End = nullptr;
		long V = std::strtol(S.c_str(), &End, 10);
		if (End == S.c_str())
			return false;
		Out = (int)V;
		return true;
	}

	static inline bool GetEnvBool(const char* Name, bool& Out)
	{
		int V = 0;
		if (!GetEnvInt(Name, V))
			return false;
		Out = V != 0;
		return true;
	}

	static inline void ApplyEnv()
	{
		std::string S;
		int I = 0;
		bool B = false;

		if (GetEnv("FNOFFLINE_PLAYLIST", S) && !S.empty())
			PlaylistName = S;

		if (GetEnvInt("FNOFFLINE_BOTS", I))
		{
			AmountOfBotsToSpawn = I < 0 ? 0 : I > 99 ? 99 : I;
			bEnableBotTick = AmountOfBotsToSpawn > 0;
		}

		if (GetEnvBool("FNOFFLINE_BOTS_AI", B))
			Globals::bFnOfflineBotsAI = B;

		if (GetEnvInt("FNOFFLINE_BOT_SKILL", I))
			Globals::FnOfflineBotSkill = I < 0 ? 0 : I > 10 ? 10 : I;

		if (GetEnvBool("FNOFFLINE_INFINITE_MATS", B))
			Globals::bInfiniteMaterials = B;

		if (GetEnvBool("FNOFFLINE_INFINITE_AMMO", B))
			Globals::bInfiniteAmmo = B;

		if (GetEnvBool("FNOFFLINE_LATEGAME", B))
			SetIsLategame(B);

		if (GetEnvBool("FNOFFLINE_CREATIVE", B))
			Globals::bCreative = B;

		Globals::bNoMCP = false;
		if (GetEnvBool("FNOFFLINE_NOMCP", B))
			Globals::bNoMCP = B;

		if (GetEnvInt("FNOFFLINE_AUTOSTART_SECONDS", I))
			AutoStartSeconds = I < 0 ? 0 : I;

		if (GetEnvInt("FNOFFLINE_AUTOSTART_MINPLAYERS", I))
			AutoStartMinPlayers = I < 1 ? 1 : I;

		if (GetEnvInt("FNOFFLINE_TEST_DEATHS", I))
			TestDeathsSeconds = I < 0 ? 0 : I;

		if (GetEnvBool("FNOFFLINE_BOT_DIAG", B))
			BotDiag = B;

		if (GetEnvInt("FNOFFLINE_TEST_END", I))
			TestEndSeconds = I < 0 ? 0 : I;

		if (GetEnvInt("FNOFFLINE_CORPSE_SECONDS", I))
			CorpseSeconds = I < 0 ? 0 : I > 60 ? 60 : I;

		LOG_INFO(LogInit, "[fnoffline] Playlist={} Bots={} BotsAI={} InfMats={} InfAmmo={} LateGame={} Creative={} AutoStart={}s MinPlayers={} NoMCP={} TestDeaths={}s Corpse={}s",
			PlaylistName, AmountOfBotsToSpawn, Globals::bFnOfflineBotsAI, Globals::bInfiniteMaterials, Globals::bInfiniteAmmo,
			Globals::bLateGame.load(), Globals::bCreative, AutoStartSeconds, AutoStartMinPlayers, Globals::bNoMCP, TestDeathsSeconds, CorpseSeconds);
	}

	// Infinite materials, client side: Reboot only skips the server-side cost (DoesBuildFree), but the client refuses to
	// place a piece with 0 materials. Keep every connected player at 999 of each (same AddItem calls as the GUI's
	// late-game loadout), checked every 3 s so it also covers the warmup island, the bus and respawns.
	static inline DWORD WINAPI MaterialsThread(LPVOID)
	{
		while (!Globals::bStartedListening)
			Sleep(500);

		const int Target = 999;

		while (true)
		{
			Sleep(3000);

			if (!Globals::bInfiniteMaterials)
				continue;

			auto World = GetWorld();
			auto NetDriver = World ? World->GetNetDriver() : nullptr;

			if (!NetDriver)
				continue;

			static auto WoodItemData = FindObject<UFortItemDefinition>(L"/Game/Items/ResourcePickups/WoodItemData.WoodItemData");
			static auto StoneItemData = FindObject<UFortItemDefinition>(L"/Game/Items/ResourcePickups/StoneItemData.StoneItemData");
			static auto MetalItemData = FindObject<UFortItemDefinition>(L"/Game/Items/ResourcePickups/MetalItemData.MetalItemData");
			UFortItemDefinition* Mats[] = { WoodItemData, StoneItemData, MetalItemData };

			auto& ClientConnections = NetDriver->GetClientConnections();

			for (int z = 0; z < ClientConnections.Num(); ++z)
			{
				auto ClientConnection = ClientConnections.at(z);
				auto FortPC = ClientConnection ? Cast<AFortPlayerController>(ClientConnection->GetPlayerController()) : nullptr;

				if (!FortPC || !FortPC->GetMyFortPawn())
					continue;

				auto WorldInventory = FortPC->GetWorldInventory();

				if (!WorldInventory)
					continue;

				bool bChanged = false;

				for (auto Mat : Mats)
				{
					if (!Mat)
						continue;

					auto Instance = WorldInventory->FindItemInstance(Mat);
					int Count = Instance ? Instance->GetItemEntry()->GetCount() : 0;

					if (Count < Target)
					{
						WorldInventory->AddItem(Mat, nullptr, Target - Count);
						bChanged = true;
					}
				}

				if (bChanged)
					WorldInventory->Update();
			}
		}

		return 0;
	}

	// Same action as the GUI "Start Bus" button (gui.h MainUI), run from its own thread like the GUI does.
	static inline DWORD WINAPI AutoStartThread(LPVOID)
	{
		while (!Globals::bStartedListening)
			Sleep(500);

		LOG_INFO(LogGame, "[fnoffline] Auto-start armed: waiting for {} client(s).", AutoStartMinPlayers);

		while (true)
		{
			auto World = GetWorld();
			auto NetDriver = World ? World->GetNetDriver() : nullptr;

			if (NetDriver && NetDriver->GetClientConnections().Num() >= AutoStartMinPlayers)
				break;

			Sleep(500);
		}

		LOG_INFO(LogGame, "[fnoffline] Starting the bus in {} s.", AutoStartSeconds);
		Sleep(AutoStartSeconds * 1000);

		if (bStartedBus)
			return 0;

		if (!Globals::bLateGame.load() && Fortnite_Version < 11)
		{
			LOG_WARN(LogGame, "[fnoffline] Auto-start only implemented for S11+ or lategame; use the GUI.");
			return 0;
		}

		auto GameMode = (AFortGameModeAthena*)GetWorld()->GetGameMode();
		auto GameState = GameMode ? Cast<AFortGameStateAthena>(GameMode->GetGameState()) : nullptr;

		if (!GameState)
			return 0;

		bStartedBus = true;
		AmountOfPlayersWhenBusStart = GameState->GetPlayersLeft();

		if (Globals::bLateGame.load())
			CreateThread(0, 0, LateGameThread, 0, 0, 0);
		else
			GameMode->StartAircraftPhase();

		LOG_INFO(LogGame, "[fnoffline] Bus started.");
		return 0;
	}

	// The first connected player's controller and pawn (nullptr when none has a pawn yet).
	static inline AFortPlayerController* FirstPlayer(AFortPawn** OutPawn)
	{
		auto World = GetWorld();
		auto NetDriver = World ? World->GetNetDriver() : nullptr;

		if (!NetDriver)
			return nullptr;

		auto& ClientConnections = NetDriver->GetClientConnections();

		for (int z = 0; z < ClientConnections.Num(); ++z)
		{
			auto ClientConnection = ClientConnections.at(z);
			auto FortPC = ClientConnection ? Cast<AFortPlayerController>(ClientConnection->GetPlayerController()) : nullptr;
			auto Pawn = FortPC ? FortPC->GetMyFortPawn() : nullptr;

			if (Pawn && !Pawn->IsActorBeingDestroyed())
			{
				*OutPawn = Pawn;
				return FortPC;
			}
		}

		return nullptr;
	}

	// FNOFFLINE_TEST_DEATHS: on the game thread (TickFlushHook). A bot is brought in front of the player, then killed
	// with the game's own ForceKill (the path ServerSuicide takes) credited to the player, so the client sees an ordinary
	// elimination: death effects, kill feed, the player's elimination count.
	static inline void TestDeathsTick()
	{
		static double Next = -1, KillAt = 0;
		static AController* PendingController = nullptr;
		static int Count = 0;

		if (TestDeathsSeconds <= 0 || !Globals::bStartedListening)
			return;

		AFortPawn* PlayerPawn = nullptr;
		auto PlayerController = FirstPlayer(&PlayerPawn);

		if (!PlayerController)
			return;

		auto World = GetWorld();
		double Now = UGameplayStatics::GetTimeSeconds(World);

		if (Next < 0)
		{
			Next = Now + 25; // the client finishes loading the warmup island first
			LOG_INFO(LogGame, "[fnoffline] test deaths: player {} seen, first one in 25 s", PlayerPawn->GetName());
			return;
		}

		if (PendingController)
		{
			if (Now < KillAt)
				return;

			auto BotPawn = Cast<AFortPawn>(PendingController->GetPawn());
			PendingController = nullptr;
			Next = Now + TestDeathsSeconds;

			if (!BotPawn || BotPawn->IsActorBeingDestroyed())
				return;

			static auto ForceKillFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPawn.ForceKill");
			struct { FGameplayTag DeathReason; AController* KillerController; AActor* KillerActor; } Params{ FGameplayTag(), PlayerController, PlayerPawn };
			auto Loc = BotPawn->GetActorLocation();
			BotPawn->ProcessEvent(ForceKillFn, &Params);
			LOG_INFO(LogGame, "[fnoffline] test death {}: {} killed at {:.0f} {:.0f} {:.0f}", ++Count, BotPawn->GetName(), Loc.X, Loc.Y, Loc.Z);
			return;
		}

		if (Now < Next)
			return;

		auto GameState = Cast<AFortGameStateAthena>(World->GetGameState());

		if (!GameState || GameState->GetGamePhase() != EAthenaGamePhase::Warmup)
			return;

		for (auto& Bot : AllPlayerBotsToTick)
		{
			auto Controller = Bot.Controller;

			if (!Controller || Controller->IsActorBeingDestroyed())
				continue;

			auto BotPawn = Cast<AFortPawn>(Controller->GetPawn());

			if (!BotPawn || BotPawn->IsActorBeingDestroyed() || BotPawn->GetHealth() <= 0)
				continue;

			// 5 m out where the camera looks (the control rotation), so the player's own model does not hide the body
			auto From = PlayerPawn->GetActorLocation();
			static auto ControlRotationOffset = PlayerController->GetOffset("ControlRotation");
			float CamYaw = PlayerController->Get<FRotator>(ControlRotationOffset).Yaw * 3.14159265f / 180.f;
			FVector Forward{ std::cos(CamYaw), std::sin(CamYaw), 0 };
			FVector To = From + Forward * 500;
			To.Z += 30;
			FRotator Facing{};
			Facing.Yaw = (float)(std::atan2(-Forward.Y, -Forward.X) * 180.0 / 3.14159265358979);
			BotPawn->TeleportTo(To, Facing);
			PendingController = Controller;
			KillAt = Now + 2.5;
			LOG_INFO(LogGame, "[fnoffline] test death: {} brought to {:.0f} {:.0f} {:.0f}", BotPawn->GetName(), To.X, To.Y, To.Z);
			return;
		}

		Next = Now + TestDeathsSeconds; // no bot left alive: try again later
	}

	// Match results for the launcher's progression (backend/progress.mjs): when the first connected player is eliminated
	// after the bus left, or is the last one standing, one JSON file goes to the offline folder's results\ (the
	// server's LOCALAPPDATA is <offline>\sandbox-server\LocalAppData; FNOFFLINE_RESULTS overrides the folder):
	//   {"id","season","playlist","place","players","kills","seconds","won"}
	// The backend applies each id once. Warmup deaths (respawns) do not count.
	static inline std::string ResultsDir()
	{
		std::string Dir;
		if (GetEnv("FNOFFLINE_RESULTS", Dir))
			return Dir;
		std::string Lad;
		if (!GetEnv("LOCALAPPDATA", Lad))
			return "";
		auto Cut = [](std::string& P) { auto I = P.find_last_of("\\/"); if (I == std::string::npos) return false; P.resize(I); return true; };
		if (!Cut(Lad) || !Cut(Lad)) // LocalAppData, sandbox-server
			return "";
		return Lad + "\\results";
	}

	static inline void ResultsTick()
	{
		static double NextCheck = 0, BusAt = -1;
		static bool Reported = false;
		static int Players = 0;
		static std::string MatchId;

		if (!Globals::bStartedListening || Reported)
			return;

		auto World = GetWorld();
		double Now = UGameplayStatics::GetTimeSeconds(World);

		if (Now < NextCheck)
			return;

		NextCheck = Now + 0.5;

		auto GameState = World ? Cast<AFortGameStateAthena>(World->GetGameState()) : nullptr;
		auto NetDriver = World ? World->GetNetDriver() : nullptr;

		if (!GameState || !NetDriver || Globals::bCreative)
			return;

		if (GameState->GetGamePhase() <= EAthenaGamePhase::Warmup)
			return;

		if (BusAt < 0)
		{
			BusAt = Now;
			Players = GameState->GetPlayersLeft();

			if (Globals::bFnOfflineBotsAI)
				Players += (int)AllPlayerBotsToTick.size();
			char Id[24];
			snprintf(Id, sizeof(Id), "%08lx%08lx", (unsigned long)time(nullptr), (unsigned long)GetCurrentProcessId() * 2654435761ul ^ (unsigned long)GetTickCount());
			MatchId = Id;
			LOG_INFO(LogGame, "[fnoffline] results: bus left with {} players, match {}", Players, MatchId);
		}

		auto& ClientConnections = NetDriver->GetClientConnections();
		AFortPlayerController* PC = nullptr;

		for (int z = 0; z < ClientConnections.Num() && !PC; ++z)
		{
			auto ClientConnection = ClientConnections.at(z);
			PC = ClientConnection ? Cast<AFortPlayerController>(ClientConnection->GetPlayerController()) : nullptr;
		}

		auto PlayerState = PC ? Cast<AFortPlayerStateAthena>(PC->GetPlayerState()) : nullptr;

		if (!PlayerState)
		{
			static double NextNoPS = 0;
			if (BotDiag && Now >= NextNoPS)
			{
				NextNoPS = Now + 10;
				LOG_INFO(LogGame, "[fnoffline] results: waiting; player controller {}, no player state", PC ? "yes" : "no");
			}
			return;
		}

		// the bots the player eliminated, by name (for their ratings): a bot gone or dead while the player's
		// elimination count went up
		static std::vector<std::string> LiveBots, Eliminated;
		static int LastKills = 0;
		{
			int KillsNow = MemberOffsets::FortPlayerStateAthena::KillScore != -1 ? PlayerState->Get<int>(MemberOffsets::FortPlayerStateAthena::KillScore) : 0;
			std::vector<std::string> Live;

			for (auto& Bot : AllPlayerBotsToTick)
			{
				auto BotPawn = Bot.Controller && !Bot.Controller->IsActorBeingDestroyed() ? Cast<AFortPawn>(Bot.Controller->GetPawn()) : nullptr;
				auto BotState = Bot.Controller && !Bot.Controller->IsActorBeingDestroyed() ? Bot.Controller->GetPlayerState() : nullptr;

				if (BotPawn && BotState && !BotPawn->IsActorBeingDestroyed() && BotPawn->GetHealth() > 0)
					Live.push_back(BotState->GetPlayerName().ToString());
			}

			for (auto& Name : LiveBots)
				if (KillsNow > LastKills && std::find(Live.begin(), Live.end(), Name) == Live.end())
				{
					Eliminated.push_back(Name);
					LastKills++;
				}

			LastKills = KillsNow;
			LiveBots = Live;
		}

		// on the bus the player has no pawn: only a pawn seen alive after the bus and then gone or dead counts
		static bool SawAlive = false;

		if (PlayerState->IsInAircraft())
		{
			static double NextAir = 0;
			if (BotDiag && Now >= NextAir)
			{
				NextAir = Now + 10;
				LOG_INFO(LogGame, "[fnoffline] results: waiting; the player is in the bus");
			}
			return;
		}

		auto Pawn = PC->GetMyFortPawn();
		bool Alive = Pawn && !Pawn->IsActorBeingDestroyed() && Pawn->GetHealth() > 0;
		int Place = PlayerState->GetPlace();
		int Left = GameState->GetPlayersLeft();

		// bots on the game's AI are not in PlayersLeft: count the living ones
		if (Globals::bFnOfflineBotsAI)
			for (auto& Bot : AllPlayerBotsToTick)
				if (auto BotPawn = Bot.Controller && !Bot.Controller->IsActorBeingDestroyed() ? Cast<AFortPawn>(Bot.Controller->GetPawn()) : nullptr)
					if (!BotPawn->IsActorBeingDestroyed() && BotPawn->GetHealth() > 0)
						Left++;

		bool Won = Alive && Left <= 1;

		if (Alive)
			SawAlive = true;

		// developer (FNOFFLINE_BOT_DIAG): why no result was written yet, every 10 s
		static double NextWhy = 0;
		if (BotDiag && Now >= NextWhy)
		{
			NextWhy = Now + 10;
			LOG_INFO(LogGame, "[fnoffline] results: waiting; pawn {}, alive {}, seen alive {}, left {}, place {}", Pawn ? Pawn->GetName() : std::string("none"), Alive, SawAlive, Left, Place);
		}

		if ((Alive && !Won) || (!Alive && !SawAlive))
			return;

		if (!Alive && Place <= 0)
			Place = Left + 1; // eliminated, the game has not written the place yet

		if (Won)
			Place = 1;

		int Kills = MemberOffsets::FortPlayerStateAthena::KillScore != -1 ? PlayerState->Get<int>(MemberOffsets::FortPlayerStateAthena::KillScore) : 0;
		int Seconds = (int)(Now - BusAt);
		Reported = true;

		auto Quote = [](const std::string& S) {
			std::string O = "\"";
			for (unsigned char C : S)
				if (C == '"' || C == '\\') { O += '\\'; O += (char)C; }
				else if (C >= 0x20) O += (char)C;
			return O + "\"";
		};

		// bots' ratings (backend/progress.mjs): the game AI bots' difficulty, the bots the player eliminated and the
		// one that eliminated the player
		std::string Bots = "";

		if (Globals::bFnOfflineBotsAI)
		{
			Bots = ",\"botSkill\":" + std::to_string(Globals::FnOfflineBotSkill > 0 ? Globals::FnOfflineBotSkill : 6) + ",\"eliminated\":[";
			for (size_t i = 0; i < Eliminated.size(); i++)
				Bots += (i ? "," : "") + Quote(Eliminated[i]);
			Bots += "]";
			if (!Alive && !Globals::FnOfflineKilledBy.empty() && std::find(Eliminated.begin(), Eliminated.end(), Globals::FnOfflineKilledBy) == Eliminated.end())
				Bots += ",\"killedBy\":" + Quote(Globals::FnOfflineKilledBy);
		}

		char Head[512];
		snprintf(Head, sizeof(Head), "{\"id\":\"%s\",\"season\":%d,\"playlist\":\"%s\",\"place\":%d,\"players\":%d,\"kills\":%d,\"seconds\":%d,\"won\":%s",
			MatchId.c_str(), (int)std::floor(Fortnite_Version), PlaylistName.substr(PlaylistName.find_last_of('.') + 1).c_str(), Place, Players, Kills, Seconds, Won ? "true" : "false");
		std::string JsonStr = std::string(Head) + Bots + "}";
		const char* Json = JsonStr.c_str();
		LOG_INFO(LogGame, "[fnoffline] result {}", Json);

		auto Dir = ResultsDir();

		if (Dir.empty())
			return;

		CreateDirectoryA(Dir.c_str(), nullptr);
		auto Tmp = Dir + "\\" + MatchId + ".tmp", Final = Dir + "\\" + MatchId + ".json";

		FILE* F = nullptr;
		if (fopen_s(&F, Tmp.c_str(), "wb") == 0 && F)
		{
			fputs(Json, F);
			fclose(F);
			MoveFileExA(Tmp.c_str(), Final.c_str(), MOVEFILE_REPLACE_EXISTING);
		}
	}

	// Dead bodies: the game removes an eliminated pawn about 3 s after its death, while the client's ragdoll (fnmods.dll
	// fnragdoll.c) wants it for a few seconds more. FNOFFLINE_CORPSE_SECONDS=<s> sets each dying pawn's life span to s
	// (0 = untouched). Every death is logged with what the game had set (life span, torn off) and when the pawn goes.
	struct FTrackedPawn { UObject* Pawn; int32 Index; double DiedAt; int Logged; };
	inline std::vector<FTrackedPawn> TrackedPawns;

	static inline void CorpseTick()
	{
		static double NextScan = 0;
		auto World = GetWorld();

		if (!World || !Globals::bStartedListening)
			return;

		double Now = UGameplayStatics::GetTimeSeconds(World);
		static auto LifeSpanGetFn = FindObject<UFunction>(L"/Script/Engine.Actor.GetLifeSpan");
		static auto LifeSpanSetFn = FindObject<UFunction>(L"/Script/Engine.Actor.SetLifeSpan");
		auto LifeSpanOf = [&](UObject* A) { struct { float Ret; } P{ -1.f }; if (LifeSpanGetFn) A->ProcessEvent(LifeSpanGetFn, &P); return P.Ret; };

		// new pawns: the bots' and the players', every 0.5 s
		if (Now >= NextScan)
		{
			NextScan = Now + 0.5;
			auto Track = [&](AFortPawn* P) {
				if (!P || P->IsActorBeingDestroyed())
					return;
				for (auto& T : TrackedPawns)
					if (T.Pawn == P)
						return;
				TrackedPawns.push_back({ P, P->InternalIndex, -1, 0 });
			};

			for (auto& Bot : AllPlayerBotsToTick)
				if (Bot.Controller && !Bot.Controller->IsActorBeingDestroyed())
					Track(Cast<AFortPawn>(Bot.Controller->GetPawn()));

			if (auto NetDriver = World->GetNetDriver())
			{
				auto& ClientConnections = NetDriver->GetClientConnections();
				for (int z = 0; z < ClientConnections.Num(); ++z)
				{
					auto FortPC = ClientConnections.at(z) ? Cast<AFortPlayerController>(ClientConnections.at(z)->GetPlayerController()) : nullptr;
					if (FortPC)
						Track(FortPC->GetMyFortPawn());
				}
			}
		}

		for (size_t i = 0; i < TrackedPawns.size();)
		{
			auto& T = TrackedPawns[i];

			if (GetObjectByIndex(T.Index) != T.Pawn)
			{
				if (T.DiedAt >= 0)
					LOG_INFO(LogGame, "[fnoffline] corpse: pawn gone {:.1f} s after its death", Now - T.DiedAt);
				TrackedPawns.erase(TrackedPawns.begin() + i);
				continue;
			}

			auto Pawn = (AFortPawn*)T.Pawn;
			static int DyingOffset = -2;
			static uint8_t DyingMask = 0;
			if (DyingOffset == -2) { DyingOffset = Pawn->GetOffset("bIsDying"); DyingMask = GetFieldMask(Pawn->GetProperty("bIsDying")); }

			if (T.DiedAt < 0)
			{
				if (DyingOffset >= 0 && Pawn->ReadBitfieldValue(DyingOffset, DyingMask))
				{
					T.DiedAt = Now;
					float Was = LifeSpanOf(Pawn);
					if (CorpseSeconds > 0 && LifeSpanSetFn)
					{
						struct { float InLifespan; } P{ (float)CorpseSeconds };
						Pawn->ProcessEvent(LifeSpanSetFn, &P);
					}
					LOG_INFO(LogGame, "[fnoffline] corpse: {} died: life span {:.1f} (set to {:.1f}), torn off {}, being destroyed {}",
						Pawn->GetName(), Was, LifeSpanOf(Pawn), Pawn->IsTearOff(), Pawn->IsActorBeingDestroyed());
				}
			}
			else if (T.Logged < 2 && Now - T.DiedAt >= (T.Logged == 0 ? 1.0 : 2.8))
			{
				T.Logged++;
				static int HiddenOffset = Pawn->GetOffset("bIsHiddenForDeath");
				static uint8_t HiddenMask = GetFieldMask(Pawn->GetProperty("bIsHiddenForDeath"));
				LOG_INFO(LogGame, "[fnoffline] corpse: {} +{:.1f} s: life span {:.1f}, torn off {}, hidden for death {}, being destroyed {}",
					Pawn->GetName(), Now - T.DiedAt, LifeSpanOf(Pawn), Pawn->IsTearOff(), Pawn->ReadBitfieldValue(HiddenOffset, HiddenMask), Pawn->IsActorBeingDestroyed());
			}

			i++;
		}
	}

	// FNOFFLINE_TEST_END=<s> (developer): s seconds after the bus left, the first player is eliminated (ForceKill), so a
	// background test run ends with a match result file.
	static inline void TestEndTick()
	{
		static double BusAt = -1;
		static bool Done = false;

		if (TestEndSeconds <= 0 || Done || !Globals::bStartedListening)
			return;

		auto World = GetWorld();
		auto GameState = World ? Cast<AFortGameStateAthena>(World->GetGameState()) : nullptr;

		if (!GameState || GameState->GetGamePhase() <= EAthenaGamePhase::Warmup)
			return;

		double Now = UGameplayStatics::GetTimeSeconds(World);

		if (BusAt < 0)
			BusAt = Now;

		if (Now - BusAt < TestEndSeconds)
			return;

		AFortPawn* PlayerPawn = nullptr;
		auto PlayerController = FirstPlayer(&PlayerPawn);

		if (!PlayerController)
			return;

		Done = true;
		static auto ForceKillFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPawn.ForceKill");
		struct { FGameplayTag DeathReason; AController* KillerController; AActor* KillerActor; } Params{ FGameplayTag(), nullptr, nullptr };
		PlayerPawn->ProcessEvent(ForceKillFn, &Params);
		LOG_INFO(LogGame, "[fnoffline] test end: player eliminated {:.0f} s after the bus left", Now - BusAt);
	}

	// FNOFFLINE_BOT_DIAG=1 (developer): every 10 s, each bot's position, distance moved, health and weapon, to tell
	// whether the game's AI moves, loots and fights.
	// the properties of an object's classes whose names contain one of the words (developer)
	static inline void DumpProps(UObject* Obj, const char* What, std::initializer_list<const char*> Words)
	{
		if (!Obj)
			return;
		std::string Out;
		for (auto Cls = Obj->ClassPrivate; Cls; Cls = *(UClass**)(__int64(Cls) + Offsets::SuperStruct))
			for (void* Prop = *(void**)(__int64(Cls) + Offsets::Children); Prop; Prop = GetNext(Prop))
			{
				auto Name = GetFNameOfProp(Prop)->ToString();
				for (auto W : Words)
					if (Name.find(W) != std::string::npos)
					{
						Out += " " + Cls->GetName() + "." + Name + "@" + std::to_string(*(int*)(__int64(Prop) + Offsets::Offset_Internal));
						break;
					}
			}
		LOG_INFO(LogGame, "[fnoffline] {} {}:{}", What, Obj->GetName(), Out);
	}

	static inline void BotDiagTick()
	{
		static double Next = 0;
		static std::vector<std::pair<AFortPawn*, FVector>> Last;
		auto World = GetWorld();

		if (!BotDiag || !World || !Globals::bStartedListening)
			return;

		double Now = UGameplayStatics::GetTimeSeconds(World);

		if (Now < Next)
			return;

		Next = Now + 10;
		int Moved = 0, Armed = 0, Alive = 0;

		// once: which classes run the function of the intermittent match-start crash (exe+0x590c031, reached through
		// virtual slot 0x66 while actors begin play): every class default object whose slot 0x66 is that function
		static bool bCrashScan = false;

		if (!bCrashScan)
		{
			bCrashScan = true;
			auto Exe = __int64(GetModuleHandleW(nullptr));
			auto Fn = PlayerBot::FunctionStartOf((void*)(Exe + 0x590c031));
			std::string Hits;
			int Count = ChunkedObjects ? ChunkedObjects->Num() : UnchunkedObjects ? UnchunkedObjects->Num() : 0;

			for (int i = 0; Fn && i < Count; i++)
			{
				auto O = GetObjectByIndex(i);
				if (O && (O->ObjectFlags & 0x10) && O->VFTable && O->VFTable[0x66] == Fn)
					Hits += " " + O->GetFullName();
			}

			LOG_INFO(LogGame, "[fnoffline] crash function exe+0x{:x}, slot 0x66 in:{}", Fn ? __int64(Fn) - Exe : 0, Hits);
		}

		// a later try at the behavior tree, once, for every bot that has none (the spawn-time one failed)
		static int Retries = 0;
		if (Retries < 2 && !AllPlayerBotsToTick.empty())
		{
			Retries++;
			for (auto& Bot : AllPlayerBotsToTick)
				FnOfflineStartBrain(Bot.Controller, Retries == 1 ? "10 s later" : "20 s later");
		}
		static bool Dumped = false;

		if (!Dumped && !AllPlayerBotsToTick.empty() && AllPlayerBotsToTick[0].Controller)
		{
			Dumped = true;
			auto C = AllPlayerBotsToTick[0].Controller;
			static auto NavOff = World->GetOffset("NavigationSystem", false);
			auto Nav = NavOff != -1 ? World->Get<UObject*>(NavOff) : nullptr;
			LOG_INFO(LogGame, "[fnoffline] navigation system: {}", Nav ? Nav->GetFullName() : std::string("none"));
			if (Nav)
				DumpProps(Nav, "navigation", { "NavData", "MainNavData", "bInitial", "Agent", "Build" });
			auto Words = { "Skill", "MMR", "Difficulty", "Runtime", "BotData", "Spawner", "Behavior", "Brain", "Aim", "Provider", "Registered" };
			{
				int RPOff = C->GetOffset("CachedAIRuntimeParametersComponent", false);
				auto RP = RPOff != -1 ? C->Get<UObject*>(RPOff) : nullptr;
				DumpProps(RP, "runtime parameters component", { "" });
			}
			DumpProps(C, "bot controller", Words);
			{
				auto Obj = [&](UObject* O, const char* N) { int Off = O ? O->GetOffset(N, false) : -1; UObject* V = Off != -1 ? O->Get<UObject*>(Off) : nullptr; return V ? V->GetFullName() : std::string("none"); };
				int SkillOff = C->GetOffset("Skill", false);
				LOG_INFO(LogGame, "[fnoffline] bot controller values: BehaviorTree {}, BrainComponent {}, Blackboard {}, PathFollowing {}, AIRuntimeParameters {}, BotData {}, Skill {:.0f}",
					Obj(C, "BehaviorTree"), Obj(C, "BrainComponent"), Obj(C, "Blackboard"), Obj(C, "PathFollowingComponent"), Obj(C, "CachedAIRuntimeParametersComponent"), Obj(C, "BotData"),
					SkillOff != -1 ? C->Get<float>(SkillOff) : -1.f);
				if (Nav)
					LOG_INFO(LogGame, "[fnoffline] navigation values: MainNavData {}, AbstractNavData {}", Obj(Nav, "MainNavData"), Obj(Nav, "AbstractNavData"));
			}
			DumpProps(C->GetPawn(), "bot pawn", Words);
			DumpProps(C->GetPlayerState(), "bot player state", Words);
			for (auto Path : { L"/Script/FortniteGame.AthenaNavInvokerBox", L"/Script/FortniteGame.AthenaNavMesh", L"/Script/FortniteGame.AthenaNavSystemConfig",
				L"/Script/FortniteGame.AthenaNavSystem" })
				if (auto Cls = FindObject<UClass>(Path))
					DumpProps(Cls->CreateDefaultObject(), "class default", { "" });
				else
					LOG_INFO(LogGame, "[fnoffline] no class {}", std::string(Path, Path + wcslen(Path)));
			static int AISysOff = World->GetOffset("AISystem", false);
			DumpProps(AISysOff != -1 ? World->Get<UObject*>(AISysOff) : nullptr, "AI system", { "Service", "Population", "Spawner", "Director", "Settings", "Bot", "Manager" });
			if (auto GM = Cast<AFortGameModeAthena>(World->GetGameMode()))
				DumpProps(GM, "game mode", { "AISettings", "AIDirector", "AIGoal", "BotManager", "AIService" });
			if (auto GM = Cast<AFortGameModeAthena>(World->GetGameMode()))
				if (auto BM = GM->GetServerBotManager())
				{
					DumpProps(BM, "bot manager", Words);
					DumpProps(BM->GetCachedBotMutator(), "bot mutator", Words);
					DumpProps(BM, "bot manager all", { "" });
				}
		}

		// developer (FNOFFLINE_BOT_MEET=1): once the player and the first bot have both landed, that bot is brought 15 m
		// from the player (behind, same height), to see whether it fights
		static bool bMeet = [] { char V[8] = {}; return GetEnvironmentVariableA("FNOFFLINE_BOT_MEET", V, sizeof(V)) && V[0] == '1'; }();
		static bool bMet = false;

		// and two landed bots brought 14 m apart, their health logged, to see them fight each other: 10 m in front of the
		// player once the player stands on the ground (in the camera, so the client's screenshots catch the fight), or
		// where the first of them is if the player has not landed 150 s after the bots
		static AFortPawn* PairA = nullptr; static AFortPawn* PairB = nullptr;
		static double PairWaitSince = 0;
		if (bMeet && !PairA)
		{
			std::vector<AFortPawn*> L;
			for (auto& B : AllPlayerBotsToTick)
				if (auto P = B.Controller ? Cast<AFortPawn>(B.Controller->GetPawn()) : nullptr; P && PlayerBot::LandedBots()[B.Controller] && P->GetHealth() > 0)
					L.push_back(P);
			AFortPawn* PlayerPawn = nullptr;
			auto PlayerController = FirstPlayer(&PlayerPawn);
			auto PPS = PlayerController ? Cast<AFortPlayerStateAthena>(PlayerController->GetPlayerState()) : nullptr;
			bool bPlayerDown = PlayerPawn && PPS && !PPS->IsInAircraft() && !PlayerBot::ReadBool(PlayerPawn, "bIsSkydiving") && !PlayerBot::ReadBool(PlayerPawn, "bIsParachuteOpen") &&
				PlayerPawn->GetHealth() > 0;

			if (L.size() >= 3 && PairWaitSince == 0)
				PairWaitSince = Now;

			if (L.size() >= 3 && (bPlayerDown || Now - PairWaitSince > 150))
			{
				PairA = L[1], PairB = L[2];
				FVector A = PairA->GetActorLocation(), Fwd{ 1, 0, 0 };
				static auto RotFn = FindObject<UFunction>(L"/Script/Engine.Controller.GetControlRotation");
				if (bPlayerDown)
				{
					FRotator R{};
					if (RotFn)
						PlayerController->ProcessEvent(RotFn, &R);
					float Yaw = R.Yaw * 3.14159265f / 180.f;
					Fwd = FVector{ std::cos(Yaw), std::sin(Yaw), 0 };
					A = PlayerPawn->GetActorLocation();
					A = FVector{ A.X + Fwd.X * 1000, A.Y + Fwd.Y * 1000, A.Z + 50 };
				}
				FVector Right{ -Fwd.Y, Fwd.X, 0 };
				PairA->TeleportTo(FVector{ A.X + Right.X * 700, A.Y + Right.Y * 700, A.Z }, FRotator());
				PairB->TeleportTo(FVector{ A.X - Right.X * 700, A.Y - Right.Y * 700, A.Z }, FRotator());
				LOG_INFO(LogGame, "[fnoffline] meet: bots {} and {} brought 14 m apart at {:.0f} {:.0f} {:.0f} ({})", PairA->GetName(), PairB->GetName(), A.X, A.Y, A.Z,
					bPlayerDown ? "in front of the player" : "the player has not landed");
			}
		}
		if (PairA)
		{
			// a pawn still owned by a bot (a dead one is destroyed and freed later: never read it then)
			auto Live = [](AFortPawn* P) {
				for (auto& B : AllPlayerBotsToTick)
					if (B.Controller && B.Controller->GetPawn() == P)
						return !P->IsActorBeingDestroyed();
				return false;
			};
			float HA = Live(PairA) ? PairA->GetHealth() + PairA->GetShield() : -1.f, HB = Live(PairB) ? PairB->GetHealth() + PairB->GetShield() : -1.f;
			if (HA >= 0 || HB >= 0)
			{
				LOG_INFO(LogGame, "[fnoffline] meet: bots health+shield {:.0f} and {:.0f}", HA, HB);
			}
			else
				PairA = PairB = (AFortPawn*)1;   // both gone: stop, and never pick a new pair
		}

		if (bMet)
		{
			AFortPawn* PlayerPawn = nullptr;
			FirstPlayer(&PlayerPawn);
			static float LastHealth = -1;
			float H = PlayerPawn ? PlayerPawn->GetHealth() + PlayerPawn->GetShield() : -1;
			if (H != LastHealth)
				LOG_INFO(LogGame, "[fnoffline] meet: player health+shield {:.0f}", H);
			LastHealth = H;
		}

		if (bMeet && !bMet && !AllPlayerBotsToTick.empty())
		{
			AFortPawn* PlayerPawn = nullptr;
			auto PlayerController = FirstPlayer(&PlayerPawn);
			auto PPS = PlayerController ? Cast<AFortPlayerStateAthena>(PlayerController->GetPlayerState()) : nullptr;
			auto PL = PlayerPawn ? PlayerPawn->GetActorLocation() : FVector();
			LOG_INFO(LogGame, "[fnoffline] meet: waiting; player {} pawn {} at {:.0f} {:.0f} {:.0f}, bus {}, skydiving {}, parachute {}, health {:.0f}", PlayerController ? "yes" : "no",
				PlayerPawn ? "yes" : "no", PL.X, PL.Y, PL.Z, PPS && PPS->IsInAircraft(), PlayerPawn && PlayerBot::ReadBool(PlayerPawn, "bIsSkydiving"),
				PlayerPawn && PlayerBot::ReadBool(PlayerPawn, "bIsParachuteOpen"), PlayerPawn ? PlayerPawn->GetHealth() : -1.f);
			AController* C = nullptr;
			for (auto& B : AllPlayerBotsToTick)
				if (B.Controller && B.Controller->GetPawn() && PlayerBot::LandedBots()[B.Controller]) { C = B.Controller; break; }
			auto P = C ? C->GetPawn() : nullptr;

			if (PlayerPawn && PPS && !PPS->IsInAircraft() && !PlayerBot::ReadBool(PlayerPawn, "bIsSkydiving") && !PlayerBot::ReadBool(PlayerPawn, "bIsParachuteOpen") &&
				PlayerPawn->GetHealth() > 0 && P)
			{
				bMet = true;
				auto L = PlayerPawn->GetActorLocation();
				FVector To{ L.X + 1500, L.Y, L.Z + 50 };
				P->TeleportTo(To, FRotator());
				LOG_INFO(LogGame, "[fnoffline] meet: {} brought to {:.0f} {:.0f} {:.0f}, player at {:.0f} {:.0f} {:.0f}", P->GetName(), To.X, To.Y, To.Z, L.X, L.Y, L.Z);
			}
		}

		// navigation state: Athena's own lock, its box invokers and octree inclusion bounds, the configs' switches,
		// and whether the first bot stands on the navigation mesh
		{
			static auto LockFn = FindObject<UFunction>(L"/Script/FortniteGame.AthenaNavSystem.IsInitialNavigationLockActive");
			static auto NavSysClass = FindObject<UClass>(L"/Script/FortniteGame.AthenaNavSystem");
			static int NavOff2 = World->GetOffset("NavigationSystem", false);
			auto Nav = NavOff2 != -1 ? World->Get<UObject*>(NavOff2) : nullptr;
			struct { UObject* Ctx; bool Ret; } LP{ World, false };
			if (LockFn && NavSysClass)
				NavSysClass->CreateDefaultObject()->ProcessEvent(LockFn, &LP);
			auto Num = [](UObject* O, const char* N) { int Off = O ? O->GetOffset(N, false) : -1; return Off != -1 ? O->Get<TArray<uint8_t>>(Off).Num() : -1; };
			std::string Cfgs;
			static auto CfgClass = FindObject<UClass>(L"/Script/FortniteGame.AthenaNavSystemConfig");
			for (auto Cfg : GetAllObjectsOfClass(CfgClass))
				if (Cfg && !Cfg->GetName().starts_with("Default__"))
				{
					Cfgs += " " + Cfg->GetName() + "(";
					for (auto B : { "bUseNavigationInvokers", "bLazyOctree", "bUseNavOctTreeInclusionBounds", "bNavOctreeUnlockedByDefaultWhenNotPreloadingNavData", "bAutoSpawnMissingNavData", "bPrioritizeNavigationAroundSpawners" })
						Cfgs += std::string(PlayerBot::ReadBool(Cfg, B) ? " +" : " -") + B;
					Cfgs += ")";
				}
			std::string OnMesh = "?";
			if (!AllPlayerBotsToTick.empty() && AllPlayerBotsToTick[0].Controller && AllPlayerBotsToTick[0].Controller->GetPawn())
			{
				static auto ProjectFn = FindObject<UFunction>(L"/Script/NavigationSystem.NavigationSystemV1.K2_ProjectPointToNavigation");
				static auto NavV1 = FindObject<UClass>(L"/Script/NavigationSystem.NavigationSystemV1");
				alignas(16) uint8_t P[96] = {};
				*(UObject**)(P + ProjectFn->GetOffsetFunc("WorldContextObject")) = World;
				*(FVector*)(P + ProjectFn->GetOffsetFunc("Point")) = AllPlayerBotsToTick[0].Controller->GetPawn()->GetActorLocation();
				*(FVector*)(P + ProjectFn->GetOffsetFunc("QueryExtent")) = FVector{ 300, 300, 600 };
				NavV1->CreateDefaultObject()->ProcessEvent(ProjectFn, P);
				OnMesh = *(bool*)(P + ProjectFn->GetOffsetFunc("ReturnValue")) ? "yes" : "no";

				// a reachable point within 30 m, and whether tiles are being built
				static auto RandomFn = FindObject<UFunction>(L"/Script/NavigationSystem.NavigationSystemV1.K2_GetRandomReachablePointInRadius");
				static auto BuiltFn = FindObject<UFunction>(L"/Script/NavigationSystem.NavigationSystemV1.IsNavigationBeingBuilt");
				if (RandomFn)
				{
					alignas(16) uint8_t R[96] = {};
					*(UObject**)(R + RandomFn->GetOffsetFunc("WorldContextObject")) = World;
					*(FVector*)(R + RandomFn->GetOffsetFunc("Origin")) = AllPlayerBotsToTick[0].Controller->GetPawn()->GetActorLocation();
					*(float*)(R + RandomFn->GetOffsetFunc("Radius")) = 3000;
					NavV1->CreateDefaultObject()->ProcessEvent(RandomFn, R);
					OnMesh += std::string(", reachable point within 30 m ") + (*(bool*)(R + RandomFn->GetOffsetFunc("ReturnValue")) ? "yes" : "no");
				}
				if (BuiltFn)
				{
					struct { UObject* Ctx; bool Ret; } B{ World, false };
					NavV1->CreateDefaultObject()->ProcessEvent(BuiltFn, &B);
					OnMesh += std::string(", being built ") + (B.Ret ? "yes" : "no");
				}
			}
			LOG_INFO(LogGame, "[fnoffline] navigation state: initial lock {}, only around invokers {}, invoker boxes {}, octree inclusion bounds {}, bot 0 on the mesh {}; configs:{}",
				LP.Ret, PlayerBot::ReadBool(Nav, "bGenerateNavigationOnlyAroundNavigationInvokers"), Num(Nav, "BoxInvokers"), Num(Nav, "NavOctTreeInclusionBounds"), OnMesh, Cfgs);
		}

		for (int i = 0; i < 3 && i < (int)AllPlayerBotsToTick.size(); i++)
		{
			auto C = AllPlayerBotsToTick[i].Controller;
			auto P = C ? C->GetPawn() : nullptr;
			auto PS = C ? Cast<AFortPlayerStateAthena>(C->GetPlayerState()) : nullptr;
			auto BB = PlayerBot::GetObj(C, "Blackboard");
			if (!P || !PS)
				continue;
			auto L = P->GetActorLocation();
			auto D = PlayerBot::BBGetVector(BB, L"AIEvaluator_Dive_Destination");
			LOG_INFO(LogGame, "[fnoffline] bot {}: at {:.0f} {:.0f} {:.0f}, bus {}, skydiving {}, parachute {}, phase step {}, jumped {}, landed {}, dive to {:.0f} {:.0f}, path data {}",
				i, L.X, L.Y, L.Z, PS->IsInAircraft(), PlayerBot::ReadBool(P, "bIsSkydiving"), PlayerBot::ReadBool(P, "bIsParachuteOpen"),
				PlayerBot::BBGetEnum(BB, L"AIEvaluator_Global_GamePhaseStep"), PlayerBot::BBGetBool(BB, L"AIEvaluator_Global_HasEverJumpedFromBusKey"),
				PlayerBot::BBGetBool(BB, L"AIEvaluator_Global_HasEverJumpedFromBusAndLandedKey"), D.X, D.Y,
				PlayerBot::NameOf(PlayerBot::GetObj(PlayerBot::GetObj(C, "PathFollowingComponent"), "MyNavData")));
		}

		for (auto& Bot : AllPlayerBotsToTick)
		{
			auto Pawn = Bot.Controller && !Bot.Controller->IsActorBeingDestroyed() ? Cast<AFortPawn>(Bot.Controller->GetPawn()) : nullptr;

			if (!Pawn || Pawn->IsActorBeingDestroyed())
				continue;

			Alive++;
			auto Loc = Pawn->GetActorLocation();
			float Dist = -1;

			for (auto& L : Last)
				if (L.first == Pawn)
				{
					Dist = (float)std::sqrt(std::pow(Loc.X - L.second.X, 2) + std::pow(Loc.Y - L.second.Y, 2) + std::pow(Loc.Z - L.second.Z, 2));
					L.second = Loc;
				}

			if (Dist < 0)
				Last.push_back({ Pawn, Loc });
			else if (Dist > 100)
				Moved++;

			auto Weapon = Pawn->GetCurrentWeapon();

			if (Weapon)
				Armed++;

			LOG_INFO(LogGame, "[fnoffline] bot {}: at {:.0f} {:.0f} {:.0f}, moved {:.0f}, health {:.0f}, weapon {}; {}", Pawn->GetName(), Loc.X, Loc.Y, Loc.Z, Dist, Pawn->GetHealth(),
				Weapon ? Weapon->GetName() : std::string("none"), PlayerBot::SteerInfo(Bot.Controller, Pawn));
		}

		static int NavOff = World->GetOffset("NavigationSystem", false);
		auto NavSys = NavOff != -1 ? World->Get<UObject*>(NavOff) : nullptr;
		static int MainNavOff = NavSys ? NavSys->GetOffset("MainNavData", false) : -1;
		auto MainNav = NavSys && MainNavOff != -1 ? NavSys->Get<UObject*>(MainNavOff) : nullptr;
		LOG_INFO(LogGame, "[fnoffline] bots: {} alive, {} moved over 1 m in 10 s, {} holding a weapon; navigation mesh {}", Alive, Moved, Armed,
			MainNav ? MainNav->GetName() : std::string("none"));
	}

	// Game thread, every server tick (Globals::FnOfflineTick).
	static inline void Tick()
	{
		// a bot whose controller was freed (dead game AI bots) counts as gone: every loop over the bots skips an
		// empty controller, and none may read a freed one
		for (auto& Bot : AllPlayerBotsToTick)
			if (Bot.Controller && !PlayerBot::LiveObj(Bot.Controller))
				Bot.Controller = nullptr;

		BotDiagTick();
		BotBuild::ProbeTick();
		BotBuild::CombatTick();
		TestDeathsTick();
		TestEndTick();
		ResultsTick();
		CorpseTick();
	}
}
