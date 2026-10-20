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

namespace FnOffline
{
	inline int AutoStartSeconds = 0;
	inline int AutoStartMinPlayers = 1;

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

		LOG_INFO(LogInit, "[fnoffline] Playlist={} Bots={} BotsAI={} InfMats={} InfAmmo={} LateGame={} Creative={} AutoStart={}s MinPlayers={} NoMCP={}",
			PlaylistName, AmountOfBotsToSpawn, Globals::bFnOfflineBotsAI, Globals::bInfiniteMaterials, Globals::bInfiniteAmmo,
			Globals::bLateGame.load(), Globals::bCreative, AutoStartSeconds, AutoStartMinPlayers, Globals::bNoMCP);
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
}
