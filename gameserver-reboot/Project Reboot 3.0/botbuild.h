#pragma once

// fnoffline: building and editing for the game AI bots. Pieces go on Fortnite's building grid (cells of 512 x 512
// x 384 units, asked to the game's structural support system), checked with the same native test the players'
// builds go through (CantBuild), paid for from the bot's materials and owned by its team.

#include "bots.h"
#include "BuildingSMActor.h"
#include "BuildingStructuralSupportSystem.h"
#include "FortKismetLibrary.h"

namespace BotBuild
{
	enum class EPiece : uint8_t { Wall, Floor, Ramp, Cone };

	struct FCell { FVector Min{}; FVector Max{}; bool bValid = false; };

	inline const char* PieceName(EPiece P)
	{
		switch (P)
		{
		case EPiece::Wall: return "wall";
		case EPiece::Floor: return "floor";
		case EPiece::Ramp: return "ramp";
		default: return "cone";
		}
	}

	// the wood piece classes, from the game state's player-buildable classes (W1 = wood level 1)
	inline UClass* PieceClass(EPiece P)
	{
		static UClass* Classes[4] = {};
		static bool bLooked = false;

		if (!bLooked)
		{
			bLooked = true;
			auto GameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());
			static int AllOff = GameState ? GameState->GetOffset("AllPlayerBuildableClasses", false) : -1;
			std::string Names;

			if (AllOff != -1)
			{
				auto& All = GameState->Get<TArray<UClass*>>(AllOff);

				for (int i = 0; i < All.Num(); i++)
				{
					auto C = All.at(i);
					if (!C)
						continue;
					auto N = C->GetName();
					Names += " " + N;
					if (N == "PBWA_W1_Solid_C") Classes[0] = C;
					else if (N == "PBWA_W1_Floor_C") Classes[1] = C;
					else if (N == "PBWA_W1_StairW_C") Classes[2] = C;
					else if (N == "PBWA_W1_RoofC_C") Classes[3] = C;
				}
			}

			LOG_INFO(LogBots, "[fnoffline] buildable classes:{}", Names);
		}

		return Classes[(int)P];
	}

	inline UObject* SupportSystem()
	{
		auto GameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());
		return GameState ? (UObject*)GameState->GetStructuralSupportSystem() : nullptr;
	}

	// the grid cell around a point
	inline FCell CellAt(const FVector& At)
	{
		static auto BoxFn = FindObject<UFunction>(L"/Script/FortniteGame.BuildingStructuralSupportSystem.K2_GetGridBoxFromWorldLoc");
		FCell Cell;
		auto SSS = SupportSystem();

		if (!SSS || !BoxFn)
			return Cell;

		struct { FVector WorldLocation; FVector Min; FVector Max; uint8_t IsValid; uint8_t Pad[3]; } P{ At };
		SSS->ProcessEvent(BoxFn, &P);
		Cell.Min = P.Min;
		Cell.Max = P.Max;
		Cell.bValid = P.IsValid != 0;
		return Cell;
	}

	// 0 when the piece fits there (the players' native check), else why not
	inline int CantBuildAt(UClass* Cls, const FVector& At, float Yaw)
	{
		if (!Cls || !Addresses::CantBuild)
			return -1;

		TArray<ABuildingSMActor*> Existing;
		char Idk = 0;
		static __int64 (*CantBuild)(UObject*, UObject*, FVector, FRotator, char, TArray<ABuildingSMActor*>*, char*) = decltype(CantBuild)(Addresses::CantBuild);
		FRotator Rot{};
		Rot.Yaw = Yaw;
		auto R = CantBuild(GetWorld(), Cls, At, Rot, false, &Existing, &Idk);
		Existing.Free();
		return (int)R;
	}

	inline ABuildingSMActor* Spawn(AController* Bot, EPiece P, const FVector& At, float Yaw)
	{
		auto Cls = PieceClass(P);

		if (!Cls || CantBuildAt(Cls, At, Yaw) != 0)
			return nullptr;

		FTransform T{};
		T.Translation = At;
		FRotator Rot{};
		Rot.Yaw = Yaw;
		T.Rotation = Rot.Quaternion();
		T.Scale3D = { 1, 1, 1 };
		auto Actor = GetWorld()->SpawnActor<ABuildingSMActor>(Cls, T);

		if (!Actor)
			return nullptr;

		Actor->SetPlayerPlaced(true);
		Actor->InitializeBuildingActor(Bot, Actor, true);

		if (auto PS = Bot ? Cast<AFortPlayerStateAthena>(Bot->GetPlayerState()) : nullptr)
			Actor->SetTeam(PS->GetTeamIndex());

		return Actor;
	}

	inline void ActorBounds(AActor* A, FVector& Origin, FVector& Extent)
	{
		static auto BoundsFn = FindObject<UFunction>(L"/Script/Engine.Actor.GetActorBounds");
		struct { bool bOnlyColliding; uint8_t Pad[3]; FVector Origin; FVector Extent; bool bChildren; uint8_t Pad2[3]; } P{ true };

		if (A && BoundsFn)
			A->ProcessEvent(BoundsFn, &P);

		Origin = P.Origin;
		Extent = P.Extent;
	}

	// ---- placement on the grid, from the probes (FNOFFLINE_BUILD_PROBE): the grid box is the building cell (512 x
	// 512 x 384, centred on multiples of 512); every piece's actor sits at the middle of one bottom edge of its cell,
	// on the cell's floor height. A wall lies along its local X through that point; a floor, ramp or cone fills the
	// cell on its local +Y side (bounds +254, +248 and +256 off the pivot at yaw 0). Local +Y at yaw t is
	// (-sin t, cos t). Edges/directions: 0 = +X, 1 = +Y, 2 = -X, 3 = -Y.

	inline FVector DirOf(int D)
	{
		static const float V[4][2] = { { 1, 0 }, { 0, 1 }, { -1, 0 }, { 0, -1 } };
		return FVector{ V[D & 3][0], V[D & 3][1], 0 };
	}

	// the yaw whose local +Y is direction D
	inline float YawFacing(int D)
	{
		static const float Y[4] = { 270, 0, 90, 180 };
		return Y[D & 3];
	}

	// the direction (of the four) nearest to a vector
	inline int NearestDir(const FVector& V)
	{
		return std::abs(V.X) >= std::abs(V.Y) ? (V.X >= 0 ? 0 : 2) : (V.Y >= 0 ? 1 : 3);
	}

	// a cell: its centre on its floor height
	inline bool CellCentre(const FVector& At, FVector& Out)
	{
		auto C = CellAt(At);
		if (!C.bValid)
			return false;
		Out = FVector{ (C.Min.X + C.Max.X) / 2, (C.Min.Y + C.Max.Y) / 2, C.Min.Z };
		return true;
	}

	inline ABuildingSMActor* WallOnEdge(AController* Bot, const FVector& Cell, int D)
	{
		auto Dir = DirOf(D);
		return Spawn(Bot, EPiece::Wall, FVector{ Cell.X + Dir.X * 256, Cell.Y + Dir.Y * 256, Cell.Z }, (D & 1) ? 0.f : 90.f);
	}

	// a floor, ramp (rising toward D) or cone filling the cell
	inline ABuildingSMActor* PieceInCell(AController* Bot, EPiece P, const FVector& Cell, int D)
	{
		auto Dir = DirOf(D);
		return Spawn(Bot, P, FVector{ Cell.X - Dir.X * 256, Cell.Y - Dir.Y * 256, Cell.Z }, YawFacing(D));
	}

	// edits: a wall replaced by another wall shape (the players' edit goes through the same native)
	inline UClass* ClassNamed(const std::string& Name)
	{
		auto GameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());
		static int AllOff = GameState ? GameState->GetOffset("AllPlayerBuildableClasses", false) : -1;
		if (AllOff == -1)
			return nullptr;
		auto& All = GameState->Get<TArray<UClass*>>(AllOff);
		for (int i = 0; i < All.Num(); i++)
			if (All.at(i) && All.at(i)->GetName() == Name)
				return All.at(i);
		return nullptr;
	}

	inline ABuildingSMActor* EditTo(ABuildingSMActor* Piece, UClass* NewClass)
	{
		if (!Piece || !NewClass || !Addresses::ReplaceBuildingActor || !PlayerBot::LiveObj(Piece) || Piece->IsDestroyed() || Piece->IsActorBeingDestroyed())
			return nullptr;
		static ABuildingSMActor* (*Replace)(ABuildingSMActor*, __int64, UClass*, int, int, uint8_t, AFortPlayerController*) = decltype(Replace)(Addresses::ReplaceBuildingActor);
		auto New = Replace(Piece, 1, NewClass, Piece->GetCurrentBuildingLevel(), 0, 0, nullptr);
		if (New)
			New->SetPlayerPlaced(true);
		return New;
	}

	// ---- building in fights (difficulty 4 and up; 1-3 never build). Each bot has 30 pieces (300 wood) per match.
	//  - shot (health or shield went down) with an enemy known: a wall on its cell's edge toward the enemy
	//    (cooldown 4 s at difficulty 4 down to 1 s at 10);
	//  - difficulty 6+, under half health: boxed in (four walls and a cone) and holding still for 4 s;
	//  - difficulty 7+, enemy within 25 m and over 2.5 m higher: ramps toward it, one per cell as it climbs;
	//  - difficulty 8+, behind its own wall toward the enemy while it fires: the wall edited to a window, and back to
	//    a solid wall 2 s later.
	// Tuned by hand, not from the game's skill tables: not verified against them.
	struct FBuildState
	{
		int Budget = 30;
		double NextBuild = 0, EditBackAt = 0;
		float LastHealth = -1;
		ABuildingSMActor* Wall = nullptr;   // its last wall toward an enemy
		ABuildingSMActor* Window = nullptr; // that wall edited
		FVector LastRampCell{};
	};

	inline void CombatTick()
	{
		static double Next = 0;
		auto World = GetWorld();
		double Now = UGameplayStatics::GetTimeSeconds(World);
		int Difficulty = Globals::FnOfflineBotSkill > 0 ? Globals::FnOfflineBotSkill : 6;

		if (!Globals::bFnOfflineBotsAI || Difficulty < 4 || Now < Next)
			return;

		Next = Now + 0.25;
		static std::map<AController*, FBuildState> States;
		static int Logged = 0;
		float Cooldown = 4.f - 3.f * (Difficulty - 4) / 6.f;
		auto Log = [&](AFortPawn* Pawn, const char* What) { if (Logged++ < 12) LOG_INFO(LogBots, "[fnoffline] bot build: {} {}", Pawn->GetName(), What); };

		for (auto& Bot : AllPlayerBotsToTick)
		{
			auto C = PlayerBot::LiveObj(Bot.Controller) ? Bot.Controller : nullptr;
			auto Pawn = C && !C->IsActorBeingDestroyed() && PlayerBot::LiveObj(C->GetPawn()) ? Cast<AFortPawn>(C->GetPawn()) : nullptr;

			if (!Pawn || Pawn->IsActorBeingDestroyed() || Pawn->GetHealth() <= 0 || !PlayerBot::LandedBots()[C])
				continue;

			auto It = PlayerBot::SteerStates().find(C);
			if (It == PlayerBot::SteerStates().end())
				continue;

			auto& S = It->second;
			auto& B = States[C];
			float Health = Pawn->GetHealth() + Pawn->GetShield();
			bool bHit = B.LastHealth >= 0 && Health < B.LastHealth - 1;
			B.LastHealth = Health;

			// edit back to a solid wall
			if (B.Window && Now >= B.EditBackAt)
			{
				if (auto Solid = EditTo(B.Window, PieceClass(EPiece::Wall)))
					B.Wall = Solid;
				B.Window = nullptr;
			}

			auto Enemy = S.Enemy && PlayerBot::LiveObj(S.Enemy) ? S.Enemy : nullptr;

			if (!Enemy || B.Budget <= 0 || Now < B.NextBuild)
				continue;

			auto L = Pawn->GetActorLocation(), E = Enemy->GetActorLocation();
			FVector Cell;

			if (!CellCentre(L, Cell))
				continue;

			int D = NearestDir(FVector{ E.X - L.X, E.Y - L.Y, 0 });
			float Dist = PlayerBot::Dist2D(L, E);

			// ramps toward a higher enemy
			if (Difficulty >= 7 && Dist < 2500 && E.Z > L.Z + 250)
			{
				auto Dir = DirOf(D);
				FVector Ahead{ Cell.X + Dir.X * 512, Cell.Y + Dir.Y * 512, Cell.Z };
				if (PlayerBot::Dist2D(Ahead, B.LastRampCell) > 1 || std::abs(Ahead.Z - B.LastRampCell.Z) > 1)
					if (PieceInCell(C, EPiece::Ramp, Ahead, D))
					{
						B.LastRampCell = Ahead, B.Budget--, B.NextBuild = Now + 0.5;
						Log(Pawn, "ramp toward a higher enemy");
						continue;
					}
			}

			// boxed in under half health
			if (Difficulty >= 6 && Health < 50 && bHit)
			{
				int Built = 0;
				for (int K = 0; K < 4 && B.Budget > 0; K++)
					if (auto W = WallOnEdge(C, Cell, K))
					{
						Built++, B.Budget--;
						if (K == D)
							B.Wall = W;
					}
				if (B.Budget > 0 && PieceInCell(C, EPiece::Cone, FVector{ Cell.X, Cell.Y, Cell.Z + 384 }, D))
					Built++, B.Budget--;
				if (Built)
				{
					S.HoldUntil = Now + 4;
					B.NextBuild = Now + Cooldown + 2;
					Log(Pawn, "boxed in");
					continue;
				}
			}

			// a wall toward the enemy when shot
			if (bHit)
				if (auto W = WallOnEdge(C, Cell, D))
				{
					B.Wall = W, B.Budget--, B.NextBuild = Now + Cooldown;
					Log(Pawn, "wall toward the enemy");
					continue;
				}

			// a window in its own wall to shoot through
			if (Difficulty >= 8 && S.bFiring && B.Wall && !B.Window && PlayerBot::LiveObj(B.Wall) && !B.Wall->IsActorBeingDestroyed() &&
				PlayerBot::Dist2D(B.Wall->GetActorLocation(), L) < 400)
			{
				static auto WindowClass = ClassNamed("PBWA_W1_WindowC_C");
				if (auto Win = EditTo(B.Wall, WindowClass))
				{
					B.Window = Win, B.Wall = nullptr, B.EditBackAt = Now + 2, B.NextBuild = Now + 1;
					Log(Pawn, "edited its wall to a window");
				}
				else
				{
					B.Wall = nullptr;
					Log(Pawn, WindowClass ? "could not edit its wall" : "no window class");
				}
			}
		}
	}

	// developer (FNOFFLINE_BUILD_PROBE=1): where each piece fits in the cell under the first bot that landed, and the
	// bounds of what is built there; the placement rules below come from this
	inline void ProbeTick()
	{
		static bool bProbe = [] { char V[8] = {}; return GetEnvironmentVariableA("FNOFFLINE_BUILD_PROBE", V, sizeof(V)) && V[0] == '1'; }();
		static bool bDone = false;

		if (!bProbe || bDone)
			return;

		for (auto& Bot : AllPlayerBotsToTick)
		{
			auto C = PlayerBot::LiveObj(Bot.Controller) ? Bot.Controller : nullptr;
			auto Pawn = C && !C->IsActorBeingDestroyed() ? C->GetPawn() : nullptr;

			if (!Pawn || !PlayerBot::LandedBots()[C] || PlayerBot::ReadBool(Pawn, "bIsSkydiving") || PlayerBot::ReadBool(Pawn, "bIsParachuteOpen"))
				continue;

			bDone = true;
			auto L = Pawn->GetActorLocation();
			auto Cell = CellAt(L);
			LOG_INFO(LogBots, "[fnoffline] build probe at {:.0f} {:.0f} {:.0f}: cell {:.0f} {:.0f} {:.0f} - {:.0f} {:.0f} {:.0f} valid {}", L.X, L.Y, L.Z, Cell.Min.X, Cell.Min.Y, Cell.Min.Z,
				Cell.Max.X, Cell.Max.Y, Cell.Max.Z, Cell.bValid);

			if (!Cell.bValid)
				return;

			// cells two over, off the bot, one per piece. The first probe (cell corners) showed CantBuild checks
			// overlaps and support but not the grid: the grid box is centred on multiples of 512, and a wall's actor sits
			// at the middle of its bottom edge (bounds -0 +1 +196, extent 256 24 196). Here: offsets from the cell's
			// centre at its floor height, the results by code (0 fits, 1 existing piece, 3 no structural support)
			const float Offs[][2] = { {0, 0}, {-256, -256}, {0, -256}, {-256, 0}, {256, 0}, {0, 256} };
			float Shift = 1024;

			for (auto P : { EPiece::Wall, EPiece::Floor, EPiece::Ramp, EPiece::Cone })
			{
				auto PC = CellAt(FVector{ L.X + Shift, L.Y, L.Z });
				Shift += 1024;
				FVector Centre{ (PC.Min.X + PC.Max.X) / 2, (PC.Min.Y + PC.Max.Y) / 2, PC.Min.Z };
				std::string Line;
				bool bBuilt = false;

				for (float Yaw : { 0.f, 90.f, 180.f, 270.f })
					for (auto& O : Offs)
					{
						FVector At{ Centre.X + O[0], Centre.Y + O[1], Centre.Z };
						int R = CantBuildAt(PieceClass(P), At, Yaw);
						Line += " " + std::to_string((int)O[0]) + "," + std::to_string((int)O[1]) + "@" + std::to_string((int)Yaw) + "=" + std::to_string(R);

						if (R == 0 && !bBuilt)
							if (auto A = Spawn(C, P, At, Yaw))
							{
								bBuilt = true;
								FVector Origin, Extent;
								ActorBounds(A, Origin, Extent);
								LOG_INFO(LogBots, "[fnoffline] build probe {} at centre {:+.0f} {:+.0f} yaw {:.0f}: bounds centre {:+.0f} {:+.0f} {:+.0f} extent {:.0f} {:.0f} {:.0f} (cell centre {:.0f} {:.0f} {:.0f})",
									PieceName(P), O[0], O[1], Yaw, Origin.X - Centre.X, Origin.Y - Centre.Y, Origin.Z - Centre.Z, Extent.X, Extent.Y, Extent.Z, Centre.X, Centre.Y, Centre.Z);

								// the wall edited to a window and back, as the bots do in fights
								if (P == EPiece::Wall)
								{
									auto WindowClass = ClassNamed("PBWA_W1_WindowC_C");
									auto Win = EditTo(A, WindowClass);
									auto Back = Win ? EditTo(Win, PieceClass(EPiece::Wall)) : nullptr;
									LOG_INFO(LogBots, "[fnoffline] build probe edit: window class {}, wall -> {} -> {}", WindowClass ? WindowClass->GetName() : std::string("missing"),
										Win ? Win->GetName() : std::string("failed"), Back ? Back->GetName() : std::string("failed"));
								}
							}
					}

				LOG_INFO(LogBots, "[fnoffline] build probe {}:{}", PieceName(P), Line);
			}

			return;
		}
	}
}
