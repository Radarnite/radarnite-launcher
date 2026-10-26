#include "commands.h"

#include "FortAthenaAIBotSpawnerData.h"
#include "GameplayStatics.h"

#include <map>
#include <string>

std::map<std::string, FVector> Waypoints;

// Radarnite (fnmods.dll's cheats: tpto, tpground, tppoi, spawnbots): the pawn's capsule, a trace on the match world and a
// teleport that puts the pawn's feet on a point.
static float RnCapsule(APawn* Pawn, bool bRadius)
{
	static auto CapsuleOffset = Pawn->GetOffset("CapsuleComponent", false);
	static auto HalfHeightFn = FindObject<UFunction>(L"/Script/Engine.CapsuleComponent.GetScaledCapsuleHalfHeight");
	static auto RadiusFn = FindObject<UFunction>(L"/Script/Engine.CapsuleComponent.GetScaledCapsuleRadius");
	auto Capsule = CapsuleOffset != -1 ? Pawn->Get<UObject*>(CapsuleOffset) : nullptr;
	auto Fn = bRadius ? RadiusFn : HalfHeightFn;
	if (!Capsule || !Fn)
		return 0.f;
	struct { float ReturnValue; } Params{};
	Capsule->ProcessEvent(Fn, &Params);
	return Params.ReturnValue;
}

// UKismetSystemLibrary::LineTraceSingle on the visibility channel (TraceTypeQuery1); Ignore (if any) is the world context
// object and bIgnoreSelf leaves it out
static bool RnTrace(const FVector& Start, const FVector& End, UObject* Ignore, FVector* OutPoint)
{
	static auto Fn = FindObject<UFunction>(L"/Script/Engine.KismetSystemLibrary.LineTraceSingle");
	static auto Ksl = FindObject(L"/Script/Engine.Default__KismetSystemLibrary");
	if (!Fn || !Ksl)
		return false;
	static int OCtx = Fn->GetOffsetFunc("WorldContextObject"), OStart = Fn->GetOffsetFunc("Start"), OEnd = Fn->GetOffsetFunc("End"),
		OChannel = Fn->GetOffsetFunc("TraceChannel"), OHit = Fn->GetOffsetFunc("OutHit"), OSelf = Fn->GetOffsetFunc("bIgnoreSelf"),
		ORet = Fn->GetOffsetFunc("ReturnValue");
	static int OImpact = FindOffsetStruct("/Script/Engine.HitResult", "ImpactPoint");
	std::vector<uint8> Params(Fn->GetPropertiesSize() + 16, 0);
	auto P = Params.data();
	*(UObject**)(P + OCtx) = Ignore ? Ignore : GetWorld();
	*(FVector*)(P + OStart) = Start;
	*(FVector*)(P + OEnd) = End;
	*(uint8*)(P + OChannel) = 0;
	*(bool*)(P + OSelf) = Ignore != nullptr;
	Ksl->ProcessEvent(Fn, P);
	bool bHit = *(bool*)(P + ORet);
	if (bHit && OutPoint)
		*OutPoint = *(FVector*)(P + OHit + OImpact);
	return bHit;
}

// the ground under X Y: the highest surface within 1 km above and below Z, else a trace down through the whole world
// (HALF_WORLD_MAX, UE 4.26 EngineDefines.h)
static bool RnGround(float X, float Y, float Z, UObject* Ignore, FVector* OutPoint)
{
	constexpr float HalfWorldMax = 1048576.f, Range = 100000.f;
	return RnTrace(FVector(X, Y, Z + Range), FVector(X, Y, Z - Range), Ignore, OutPoint) ||
		RnTrace(FVector(X, Y, HalfWorldMax), FVector(X, Y, -HalfWorldMax), Ignore, OutPoint);
}

static void RnTeleportFeet(AFortPlayerController* PlayerController, APawn* Pawn, const FVector& Feet, const char* What)
{
	float Half = RnCapsule(Pawn, false);
	FVector Dest(Feet.X, Feet.Y, Feet.Z + Half);
	bool bOk = Pawn->TeleportTo(Dest, Pawn->GetActorRotation());
	auto Now = Pawn->GetActorLocation();
	LOG_INFO(LogDev, "[radarnite] teleport ({}): feet to {:.0f} {:.0f} {:.0f}, actor at {:.0f} {:.0f} {:.0f} (half height {:.0f}, {})", What, Feet.X, Feet.Y, Feet.Z,
		Now.X, Now.Y, Now.Z, Half, bOk ? "done" : "refused");
	SendMessageToConsole(PlayerController, bOk ? L"Teleported!" : L"Teleport refused (no room there)");
}

void ServerCheatHook(AFortPlayerControllerAthena* PlayerController, FString Msg)
{
	bool isMsgEmpty = !Msg.Data.Data || Msg.Data.Num() <= 0;
	// if (isMsgEmpty)
		// return;

	auto PlayerState = Cast<AFortPlayerStateAthena>(PlayerController->GetPlayerState());

	// std::cout << "aa!\n";

	if (!PlayerState || !IsOperator(PlayerState, PlayerController))
		return;

	std::vector<std::string> Arguments;
	std::string OldMsg = "";
	if (!isMsgEmpty)
		OldMsg = Msg.ToString();

	auto ReceivingController = PlayerController; // for now
	auto ReceivingPlayerState = PlayerState; // for now

	auto firstBackslash = OldMsg.find_first_of("\\");
	auto lastBackslash = OldMsg.find_last_of("\\");

	static auto World_NetDriverOffset = GetWorld()->GetOffset("NetDriver");
	auto WorldNetDriver = GetWorld()->Get<UNetDriver*>(World_NetDriverOffset);
	auto& ClientConnections = WorldNetDriver->GetClientConnections();

	if (firstBackslash != std::string::npos && lastBackslash != std::string::npos)
	{
		if (firstBackslash != lastBackslash)
		{
			std::string player = OldMsg;

			player = player.substr(firstBackslash + 1, lastBackslash - firstBackslash - 1);

			for (int i = 0; i < ClientConnections.Num(); ++i)
			{
				static auto PlayerControllerOffset = ClientConnections.at(i)->GetOffset("PlayerController");
				auto CurrentPlayerController = Cast<AFortPlayerControllerAthena>(ClientConnections.at(i)->Get(PlayerControllerOffset));

				if (!CurrentPlayerController)
					continue;

				auto CurrentPlayerState = Cast<AFortPlayerStateAthena>(CurrentPlayerController->GetPlayerState());

				if (!CurrentPlayerState)
					continue;

				FString PlayerName = CurrentPlayerState->GetPlayerName();

				if (PlayerName.ToString() == player) // hopefully we arent on adifferent thread
				{
					ReceivingController = CurrentPlayerController;
					ReceivingPlayerState = CurrentPlayerState;
					PlayerName.Free();
					break;
				}

				PlayerName.Free();
			}
		}
		else
		{
			// SendMessageToConsole(PlayerController, L"Warning: You have a backslash but no ending backslash, was this by mistake? Executing on you.");
		}
	}

	if (!ReceivingController || !ReceivingPlayerState)
	{
		SendMessageToConsole(PlayerController, L"Unable to find player!");
		return;
	}

	if (!isMsgEmpty)
	{
		auto Message = Msg.ToString();

		size_t start = Message.find('\\');

		while (start != std::string::npos) // remove the playername
		{
			size_t end = Message.find('\\', start + 1);

			if (end == std::string::npos)
				break;

			Message.replace(start, end - start + 2, "");
			start = Message.find('\\');
		}

		int zz = 0;

		// std::cout << "Message Before: " << Message << '\n';

		while (Message.find(" ") != std::string::npos)
		{
			auto arg = Message.substr(0, Message.find(' '));
			Arguments.push_back(arg);
			// std::cout << std::format("[{}] {}\n", zz, arg);
			Message.erase(0, Message.find(' ') + 1);
			zz++;
		}

		// if (zz == 0)
		{
			Arguments.push_back(Message);
			// std::cout << std::format("[{}] {}\n", zz, Message);
			zz++;
		}

		// std::cout << "Message After: " << Message << '\n';
	}

	auto NumArgs = Arguments.size() == 0 ? 0 : Arguments.size() - 1;

	// std::cout << "NumArgs: " << NumArgs << '\n';

	// return;

	bool bSendHelpMessage = isMsgEmpty;

	auto GameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());
	auto GameMode = Cast<AFortGameModeAthena>(GetWorld()->GetGameMode());

	if (Arguments.size() >= 1)
	{
		auto& Command = Arguments[0];
		std::transform(Command.begin(), Command.end(), Command.begin(), ::tolower);

		if (Command == "giveitem")
		{
			if (NumArgs < 1)
			{
				SendMessageToConsole(PlayerController, L"Please provide a WID!");
				return;
			}

			auto WorldInventory = ReceivingController->GetWorldInventory();

			if (!WorldInventory)
			{
				SendMessageToConsole(PlayerController, L"No world inventory!");
				return;
			}

			auto& weaponName = Arguments[1];
			int count = 1;

			try
			{
				if (NumArgs >= 2)
					count = std::stoi(Arguments[2]);
			}
			catch (...)
			{
			}

			// LOG_INFO(LogDev, "weaponName: {}", weaponName);

			auto WID = Cast<UFortWorldItemDefinition>(FindObject(weaponName, nullptr, ANY_PACKAGE));

			if (!WID)
			{
				SendMessageToConsole(PlayerController, L"Invalid WID!");
				return;
			}

			bool bShouldUpdate = false;
			WorldInventory->AddItem(WID, &bShouldUpdate, count);

			if (bShouldUpdate)
				WorldInventory->Update();

			SendMessageToConsole(PlayerController, L"Granted item!");
		}
		else if (Command == "printsimulatelootdrops")
		{
			if (NumArgs < 1)
			{
				SendMessageToConsole(PlayerController, L"Please provide a LootTierGroup!");
				return;
			}

			auto& lootTierGroup = Arguments[1];

			auto LootDrops = PickLootDrops(UKismetStringLibrary::Conv_StringToName(std::wstring(lootTierGroup.begin(), lootTierGroup.end()).c_str()), -1, true);

			for (int i = 0; i < LootDrops.size(); ++i)
			{

			}

			SendMessageToConsole(PlayerController, L"Printed!");
		}
		/* else if (Command == "debugattributes")
		{
			auto AbilitySystemComponent = ReceivingPlayerState->GetAbilitySystemComponent();

			if (!AbilitySystemComponent)
			{
				SendMessageToConsole(PlayerController, L"No AbilitySystemComponent!");
				return;
			}

			SendMessageToConsole(PlayerController, (L"AbilitySystemComponent->GetSpawnedAttributes().Num(): " + std::to_wstring(AbilitySystemComponent->GetSpawnedAttributes().Num())).c_str());

			for (int i = 0; i < AbilitySystemComponent->GetSpawnedAttributes().Num(); ++i)
			{
				auto CurrentAttributePathName = AbilitySystemComponent->GetSpawnedAttributes().at(i)->GetPathName();
				SendMessageToConsole(PlayerController, (L"SpawnedAttribute Name: " + std::wstring(CurrentAttributePathName.begin(), CurrentAttributePathName.end())).c_str());
			}
		}
		else if (Command == "debugcurrentitem")
		{
			auto Pawn = ReceivingController->GetMyFortPawn();

			if (!Pawn)
			{
				SendMessageToConsole(PlayerController, L"No pawn!");
				return;
			}

			auto CurrentWeapon = Pawn->GetCurrentWeapon();

			if (!CurrentWeapon)
			{
				SendMessageToConsole(PlayerController, L"No CurrentWeapon!");
				return;
			}

			auto WorldInventory = ReceivingController->GetWorldInventory();

			if (!CurrentWeapon)
			{
				SendMessageToConsole(PlayerController, L"No WorldInventory!");
				return;
			}

			auto ItemInstance = WorldInventory->FindItemInstance(CurrentWeapon->GetItemEntryGuid());
			auto ReplicatedEntry = WorldInventory->FindReplicatedEntry(CurrentWeapon->GetItemEntryGuid());

			if (!ItemInstance)
			{
				SendMessageToConsole(PlayerController, L"Failed to find ItemInstance!");
				return;
			}

			if (!ReplicatedEntry)
			{
				SendMessageToConsole(PlayerController, L"Failed to find ReplicatedEntry!");
				return;
			}

			SendMessageToConsole(PlayerController, (L"ReplicatedEntry->GetGenericAttributeValues().Num(): " + std::to_wstring(ReplicatedEntry->GetGenericAttributeValues().Num())).c_str());
			SendMessageToConsole(PlayerController, (L"ReplicatedEntry->GetStateValues().Num(): " + std::to_wstring(ReplicatedEntry->GetStateValues().Num())).c_str());

			for (int i = 0; i < ReplicatedEntry->GetStateValues().Num(); ++i)
			{
				SendMessageToConsole(PlayerController, (L"[{}] StateValue Type: "
					+ std::to_wstring((int)ReplicatedEntry->GetStateValues().at(i, FFortItemEntryStateValue::GetStructSize()).GetStateType())).c_str()
				);
			}
		} */
		else if (Command == "op")
		{
			if (ReceivingController == PlayerController)
			{
				SendMessageToConsole(PlayerController, L"You can't op yourself!");
				return;
			}

			if (IsOp(ReceivingController))
			{
				SendMessageToConsole(PlayerController, L"Player is already operator!");
				return;
			}

			Op(ReceivingController);
			SendMessageToConsole(PlayerController, L"Granted operator to player!");
		}
		else if (Command == "deop")
		{
			if (!IsOp(ReceivingController))
			{
				SendMessageToConsole(PlayerController, L"Player is not operator!");
				return;
			}

			Deop(ReceivingController);
			SendMessageToConsole(PlayerController, L"Removed operator from player!");
		}
		else if (Command == "setpickaxe")
		{
			if (NumArgs < 1)
			{
				SendMessageToConsole(PlayerController, L"Please provide a pickaxe!");
				return;
			}

			if (Fortnite_Version < 3) // Idk why but emptyslot kicks the player because of the validate.
			{
				SendMessageToConsole(PlayerController, L"Not supported on this version!");
				return;
			}

			auto WorldInventory = ReceivingController->GetWorldInventory();

			if (!WorldInventory)
			{
				SendMessageToConsole(PlayerController, L"No world inventory!");
				return;
			}

			auto& pickaxeName = Arguments[1];
			static auto AthenaPickaxeItemDefinitionClass = FindObject<UClass>(L"/Script/FortniteGame.AthenaPickaxeItemDefinition");

			auto Pickaxe1 = FindObject(pickaxeName + "." + pickaxeName, nullptr, ANY_PACKAGE);

			UFortWeaponMeleeItemDefinition* NewPickaxeItemDefinition = nullptr;

			if (Pickaxe1)
			{
				if (Pickaxe1->IsA(AthenaPickaxeItemDefinitionClass))
				{
					static auto WeaponDefinitionOffset = Pickaxe1->GetOffset("WeaponDefinition");
					NewPickaxeItemDefinition = Pickaxe1->Get<UFortWeaponMeleeItemDefinition*>(WeaponDefinitionOffset);
				}
				else
				{
					NewPickaxeItemDefinition = Cast<UFortWeaponMeleeItemDefinition>(Pickaxe1);
				}
			}

			if (!NewPickaxeItemDefinition)
			{
				SendMessageToConsole(PlayerController, L"Invalid pickaxe item definition!");
				return;
			}

			auto PickaxeInstance = WorldInventory->GetPickaxeInstance();

			if (PickaxeInstance)
			{
				WorldInventory->RemoveItem(PickaxeInstance->GetItemEntry()->GetItemGuid(), nullptr, PickaxeInstance->GetItemEntry()->GetCount(), true);
			}

			WorldInventory->AddItem(NewPickaxeItemDefinition, nullptr, 1);
			WorldInventory->Update();

			SendMessageToConsole(PlayerController, L"Successfully set pickaxe!");
		}
		else if (Command == "load")
		{
			if (!Globals::bCreative)
			{
				SendMessageToConsole(PlayerController, L"It is not creative!");
				return;
			}

			static auto CreativePlotLinkedVolumeOffset = ReceivingController->GetOffset("CreativePlotLinkedVolume", false);
			auto Volume = CreativePlotLinkedVolumeOffset == -1 ? nullptr : ReceivingController->GetCreativePlotLinkedVolume();

			if (Arguments.size() <= 1)
			{
				SendMessageToConsole(PlayerController, L"Please provide a filename!\n");
				return;
			}

			std::string FileName = "islandSave";

			try { FileName = Arguments[1]; }
			catch (...) {}

			float X{ -1 }, Y{ -1 }, Z{ -1 };

			if (Arguments.size() >= 4)
			{
				try { X = std::stof(Arguments[2]); }
				catch (...) {}
				try { Y = std::stof(Arguments[3]); }
				catch (...) {}
				try { Z = std::stof(Arguments[4]); }
				catch (...) {}
			}
			else
			{
				if (!Volume)
				{
					SendMessageToConsole(PlayerController, L"They do not have an island!");
					return;
				}
			}

			if (X != -1 && Y != -1 && Z != -1) // omg what if they want to spawn it at -1 -1 -1!!!
				Builder::LoadSave(FileName, FVector(X, Y, Z), FRotator());
			else
				Builder::LoadSave(FileName, Volume);

			SendMessageToConsole(PlayerController, L"Loaded!");
		}
		else if (Command == "spawnpickup")
		{
			if (NumArgs < 1)
			{
				SendMessageToConsole(PlayerController, L"Please provide a WID!");
				return;
			}

			auto Pawn = ReceivingController->GetMyFortPawn();

			if (!Pawn)
			{
				SendMessageToConsole(PlayerController, L"No pawn!");
				return;
			}

			auto& weaponName = Arguments[1];
			int count = 1;
			int amount = 1;

			try
			{
				if (NumArgs >= 2)
					count = std::stoi(Arguments[2]);
				if (NumArgs >= 3)
					amount = std::stoi(Arguments[3]);
			}
			catch (...)
			{
			}

			constexpr int Max = 100;

			if (amount > Max)
			{
				SendMessageToConsole(PlayerController, (std::wstring(L"You went over the limit! Only spawning ") + std::to_wstring(Max) + L".").c_str());
				amount = Max;
			}

			// LOG_INFO(LogDev, "weaponName: {}", weaponName);

			auto WID = Cast<UFortWorldItemDefinition>(FindObject(weaponName, nullptr, ANY_PACKAGE));

			if (!WID)
			{
				SendMessageToConsole(PlayerController, L"Invalid WID!");
				return;
			}

			auto Location = Pawn->GetActorLocation();

			auto GameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());

			PickupCreateData CreateData;
			CreateData.ItemEntry = FFortItemEntry::MakeItemEntry(WID, count, -1, MAX_DURABILITY, WID->GetFinalLevel(GameState->GetWorldLevel()));
			CreateData.SpawnLocation = Location;
			CreateData.bShouldFreeItemEntryWhenDeconstructed = true;

			for (int i = 0; i < amount; ++i)
			{
				AFortPickup::SpawnPickup(CreateData);
			}
		}
		else if (Command == "listplayers")
		{
			std::string PlayerNames;

			for (int i = 0; i < ClientConnections.Num(); i++)
			{
				static auto PlayerControllerOffset = ClientConnections.at(i)->GetOffset("PlayerController");
				auto CurrentPlayerController = Cast<AFortPlayerControllerAthena>(ClientConnections.at(i)->Get(PlayerControllerOffset));

				if (!CurrentPlayerController)
					continue;

				auto CurrentPlayerState = Cast<AFortPlayerStateAthena>(CurrentPlayerController->GetPlayerState());

				if (!CurrentPlayerState->IsValidLowLevel())
					continue;

				PlayerNames += "\"" + CurrentPlayerState->GetPlayerName().ToString() + "\" ";
			}

			SendMessageToConsole(PlayerController, std::wstring(PlayerNames.begin(), PlayerNames.end()).c_str());
		}
		else if (Command == "launch")
		{
			if (Arguments.size() <= 3)
			{
				SendMessageToConsole(PlayerController, L"Please provide X, Y, and Z!\n");
				return;
			}

			float X{}, Y{}, Z{};

			try { X = std::stof(Arguments[1]); }
			catch (...) {}
			try { Y = std::stof(Arguments[2]); }
			catch (...) {}
			try { Z = std::stof(Arguments[3]); }
			catch (...) {}

			auto Pawn = ReceivingController->GetMyFortPawn();

			if (!Pawn)
			{
				SendMessageToConsole(PlayerController, L"No pawn to teleport!");
				return;
			}

			static auto LaunchCharacterFn = FindObject<UFunction>(L"/Script/Engine.Character.LaunchCharacter");

			struct
			{
				FVector                                     LaunchVelocity;                                           // (Parm, ZeroConstructor, IsPlainOldData, NoDestructor, HasGetValueTypeHash, NativeAccessSpecifierPublic)
				bool                                               bXYOverride;                                              // (Parm, ZeroConstructor, IsPlainOldData, NoDestructor, HasGetValueTypeHash, NativeAccessSpecifierPublic)
				bool                                               bZOverride;                                               // (Parm, ZeroConstructor, IsPlainOldData, NoDestructor, HasGetValueTypeHash, NativeAccessSpecifierPublic)
			} ACharacter_LaunchCharacter_Params{ FVector(X, Y, Z), false, false };
			Pawn->ProcessEvent(LaunchCharacterFn, &ACharacter_LaunchCharacter_Params);

			SendMessageToConsole(PlayerController, L"Launched character!");
		}
		else if (Command == "setshield")
		{
			auto Pawn = ReceivingController->GetMyFortPawn();

			if (!Pawn)
			{
				SendMessageToConsole(PlayerController, L"No pawn!");
				return;
			}

			float Shield = 0.f;

			if (NumArgs >= 1)
			{
				try { Shield = std::stof(Arguments[1]); }
				catch (...) {}
			}

			Pawn->SetShield(Shield);
			SendMessageToConsole(PlayerController, L"Set shield!\n");
		}
		else if (Command == "god")
		{
			auto Pawn = ReceivingController->GetMyFortPawn();

			if (!Pawn)
			{
				SendMessageToConsole(PlayerController, L"No pawn!");
				return;
			}

			// Radarnite: "god on" / "god off" set it (fnmods.dll sends it again for every new pawn); no argument toggles
			bool bGod = !Pawn->CanBeDamaged();

			if (NumArgs >= 1)
				bGod = Arguments[1] == "on" || Arguments[1] == "1" || Arguments[1] == "true";

			Pawn->SetCanBeDamaged(!bGod);
			LOG_INFO(LogDev, "[radarnite] god {} for {}", bGod ? "on" : "off", (void*)Pawn);
			SendMessageToConsole(PlayerController, std::wstring(L"God set to " + std::to_wstring(!(bool)Pawn->CanBeDamaged())).c_str());
		}
		else if (Command == "applycid")
		{
			auto PlayerState = Cast<AFortPlayerState>(ReceivingController->GetPlayerState());

			if (!PlayerState) // ???
			{
				SendMessageToConsole(PlayerController, L"No playerstate!");
				return;
			}

			auto Pawn = Cast<AFortPlayerPawn>(ReceivingController->GetMyFortPawn());

			std::string CIDStr = Arguments[1];
			auto CIDDef = FindObject(CIDStr, nullptr, ANY_PACKAGE);
			// auto CIDDef = UObject::FindObject<UAthenaCharacterItemDefinition>(CIDStr);

			if (!CIDDef)
			{
				SendMessageToConsole(PlayerController, L"Invalid character item definition!");
				return;
			}

			LOG_INFO(LogDev, "Applying {}", CIDDef->GetFullName());

			if (!ApplyCID(Pawn, CIDDef))
			{
				SendMessageToConsole(PlayerController, L"Failed while applying skin! Please check the server log.");
				return;
			}

			SendMessageToConsole(PlayerController, L"Applied CID!");
		}
		else if (Command == "suicide")
		{
			static auto ServerSuicideFn = FindObject<UFunction>("/Script/FortniteGame.FortPlayerController.ServerSuicide");
			ReceivingController->ProcessEvent(ServerSuicideFn);
		}
		else if (Command == "summon")
		{
			if (Arguments.size() <= 1)
			{
				SendMessageToConsole(PlayerController, L"Please provide a class!\n");
				return;
			}

			auto& ClassName = Arguments[1];

			/* if (ClassName.contains("/Script/"))
			{
				SendMessageToConsole(PlayerController, L"For now, we don't allow non-blueprint classes.\n");
				return;
			} */

			auto Pawn = ReceivingController->GetPawn();

			if (!Pawn)
			{
				SendMessageToConsole(PlayerController, L"No pawn to spawn class at!");
				return;
			}

			int Count = 1;

			if (Arguments.size() >= 3)
			{
				try { Count = std::stod(Arguments[2]); }
				catch (...) {}
			}

			constexpr int Max = 100;

			if (Count > Max)
			{
				SendMessageToConsole(PlayerController, (std::wstring(L"You went over the limit! Only spawning ") + std::to_wstring(Max) + L".").c_str());
				Count = Max;
			}

			static auto BGAClass = FindObject<UClass>(L"/Script/Engine.BlueprintGeneratedClass");
			static auto ClassClass = FindObject<UClass>(L"/Script/CoreUObject.Class");
			auto ClassObj = ClassName.contains("/Script/") ? FindObject<UClass>(ClassName, ClassClass) : LoadObject<UClass>(ClassName, BGAClass); // scuffy

			if (ClassObj)
			{
				int AmountSpawned = 0;

				for (int i = 0; i < Count; i++)
				{
					auto Loc = Pawn->GetActorLocation();
					Loc.Z += 1000;
					auto NewActor = GetWorld()->SpawnActor<AActor>(ClassObj, Loc, FQuat(), FVector(1, 1, 1));

					if (!NewActor)
					{
						SendMessageToConsole(PlayerController, L"Failed to spawn an actor!");
					}
					else
					{
						AmountSpawned++;
					}
				}

				SendMessageToConsole(PlayerController, L"Summoned!");
			}
			else
			{
				SendMessageToConsole(PlayerController, L"Not a valid class!");
			}
		}
		else if (Command == "spawnbottest")
		{
			// /Game/Athena/AI/MANG/BotData/

			if (NumArgs < 1)
			{
				SendMessageToConsole(PlayerController, L"Please provide a customization object!");
				return;
			}

			auto Pawn = ReceivingController->GetPawn();

			if (!Pawn)
			{
				SendMessageToConsole(PlayerController, L"No pawn to spawn bot at!");
				return;
			}

			auto CustomizationData = LoadObject<UFortAthenaAIBotCustomizationData>(Arguments[1], UFortAthenaAIBotCustomizationData::StaticClass());

			if (!CustomizationData)
			{
				SendMessageToConsole(PlayerController, L"Invalid CustomizationData!");
				return;
			}

			auto NewPawn = SpawnAIFromCustomizationData(Pawn->GetActorLocation(), CustomizationData);

			if (NewPawn)
			{
				SendMessageToConsole(PlayerController, L"Spawned!");
			}
			else
			{
				SendMessageToConsole(PlayerController, L"Failed to spawn!");
			}
		}
		else if (Command == "spawnbottest2")
		{
			// FortniteGame/Plugins/GameFeatures/CosmosGameplay/Content/AI/NPCs/Cosmos/AISpawnerData/BP_AIBotSpawnerData_Cosmos
			// /Game/Athena/AI/NPCs/Base/AISpawnerData/BP_AIBotSpawnerData_NPC_Base.BP_AIBotSpawnerData_NPC_Base_C
			// /CosmosGameplay/AI/NPCs/Cosmos/AISpawnerData/BP_AIBotSpawnerData_Cosmos.BP_AIBotSpawnerData_Cosmos_C

			if (NumArgs < 1)
			{
				SendMessageToConsole(PlayerController, L"Please provide a customization object!");
				return;
			}

			auto Pawn = ReceivingController->GetPawn();

			if (!Pawn)
			{
				SendMessageToConsole(PlayerController, L"No pawn to spawn bot at!");
				return;
			}

			static auto BlueprintGeneratedClassClass = FindObject<UClass>(L"/Script/Engine.BlueprintGeneratedClass");
			auto SpawnerDataClass = LoadObject<UClass>(Arguments[1], BlueprintGeneratedClassClass);
			// auto SpawnerData = LoadObject<UFortAthenaAIBotSpawnerData>(Arguments[1], UFortAthenaAIBotSpawnerData::StaticClass());

			if (!SpawnerDataClass)
			{
				SendMessageToConsole(PlayerController, L"Invalid SpawnerDataClass!");
				return;
			}

			auto DefaultSpawnerData = Cast<UFortAthenaAIBotSpawnerData>(SpawnerDataClass->CreateDefaultObject());

			if (!SpawnerDataClass)
			{
				SendMessageToConsole(PlayerController, L"Invalid DefaultSpawnerData!");
				return;
			}

			auto NewPawn = SpawnAIFromSpawnerData(Pawn->GetActorLocation(), DefaultSpawnerData);

			if (NewPawn)
			{
				SendMessageToConsole(PlayerController, L"Spawned!");
			}
			else
			{
				SendMessageToConsole(PlayerController, L"Failed to spawn!");
			}
		}
		else if (Command == "spawnbot")
		{
			if (GameState->GetGamePhase() < EAthenaGamePhase::Aircraft)
			{
				SendMessageToConsole(PlayerController, L"Bot spawning before aircraft is not allowed!");
				return;
			}

			auto Pawn = ReceivingController->GetPawn();

			if (!Pawn)
			{
				SendMessageToConsole(PlayerController, L"No pawn to spawn bot at!");
				return;
			}

			int Count = 1;

			if (Arguments.size() >= 2)
			{
				try { Count = std::stod(Arguments[1]); }
				catch (...) {}
			}

			constexpr int Max = 99;

			if (Count > Max)
			{
				SendMessageToConsole(PlayerController, (std::wstring(L"You went over the limit! Only spawning ") + std::to_wstring(Max) + L".").c_str());
				Count = Max;
			}

			int AmountSpawned = 0;

			for (int i = 0; i < Count; i++)
			{
				FActorSpawnParameters SpawnParameters{};
				// SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

				auto Loc = Pawn->GetActorLocation();
				Loc.Z += 1000;

				FTransform Transform;
				Transform.Translation = Loc;
				Transform.Scale3D = FVector(1, 1, 1);

				auto NewActor = Bots::SpawnBot(Transform, Pawn);

				if (!NewActor)
				{
					SendMessageToConsole(PlayerController, L"Failed to spawn an actor!");
				}
				else
				{
					AmountSpawned++;
				}
			}

			if (AmountSpawned > 0)
				SendMessageToConsole(PlayerController, L"Summoned!");
		}
		else if (Command == "sethealth")
		{
			auto Pawn = ReceivingController->GetMyFortPawn();

			if (!Pawn)
			{
				SendMessageToConsole(PlayerController, L"No pawn!");
				return;
			}

			float Health = 100.f;

			try { Health = std::stof(Arguments[1]); }
			catch (...) {}

			Pawn->SetHealth(Health);
			SendMessageToConsole(PlayerController, L"Set health!\n");
		}
		else if (Command == "pausesafezone")
		{
			UKismetSystemLibrary::ExecuteConsoleCommand(GetWorld(), L"pausesafezone", nullptr);
			// GameMode->PauseSafeZone(GameState->IsSafeZonePaused() == 0);
		}
		else if (Command == "teleport" || Command == "tp")
		{
			UCheatManager*& CheatManager = ReceivingController->SpawnCheatManager(UCheatManager::StaticClass());

			if (!CheatManager)
			{
				SendMessageToConsole(PlayerController, L"Failed to spawn player's cheat manager!");
				return;
			}

			CheatManager->Teleport();
			CheatManager = nullptr;
			SendMessageToConsole(PlayerController, L"Teleported!");
		}
		else if (Command == "savewaypoint")
		{
			if (NumArgs < 1) 
			{
				SendMessageToConsole(PlayerController, L"Please provide a phrase to save the waypoint.");
				return;
			}

			auto Pawn = ReceivingController->GetMyFortPawn();

			if (!Pawn) 
			{
				SendMessageToConsole(PlayerController, L"No pawn to get location from!");
				return;
			}

			auto PawnLocation = Pawn->GetActorLocation();
			Waypoints[Arguments[1]] = PawnLocation;

			SendMessageToConsole(PlayerController, L"Waypoint saved! Use « cheat waypoint (phrase) » to teleport to that location!");
		}
		else if (Command == "waypoint")
		{
			if (NumArgs < 1) 
			{
				SendMessageToConsole(PlayerController, L"Please provide a waypoint phrase to teleport to.");
				return;
			}

			std::string Phrase = Arguments[1];

			if (Waypoints.find(Phrase) == Waypoints.end()) 
			{
				SendMessageToConsole(PlayerController, L"A saved waypoint with this phrase was not found!");
				return;
			}

			FVector Destination = Waypoints[Phrase];

			auto Pawn = ReceivingController->GetMyFortPawn();

			if (Pawn) 
			{
				Pawn->TeleportTo(Destination, Pawn->GetActorRotation());
				SendMessageToConsole(PlayerController, L"Teleported to waypoint!");
			}
			else 
			{
				SendMessageToConsole(PlayerController, L"No pawn to teleport!");
			}
		}
		else if (Command == "startaircraft")
		{
			GameMode->StartAircraftPhase();
			SendMessageToConsole(PlayerController, L"Started aircraft!");
		}
		else if (Command == "wipequickbar" || Command == "wipequickbars")
		{
			bool bWipePrimary = false;
			bool bWipeSecondary = false;
			bool bCheckShouldBeDropped = true;

			bool bWipeSingularQuickbar = Command != "wipequickbars";

			if (bWipeSingularQuickbar)
			{
				if (Arguments.size() <= 1)
				{
					SendMessageToConsole(PlayerController, L"Please provide \"primary\" or \"secondary\"!\n");
					return;
				}

				std::string quickbarType = Arguments[1];
				std::transform(quickbarType.begin(), quickbarType.end(), quickbarType.begin(), ::tolower);

				if (quickbarType == "primary") bWipePrimary = true;
				if (quickbarType == "secondary") bWipeSecondary = true;
			}
			else
			{
				bWipePrimary = true;
				bWipeSecondary = true;
			}

			if (!bWipePrimary && !bWipeSecondary)
			{
				SendMessageToConsole(PlayerController, L"Please provide \"primary\" or \"secondary\"!\n");
				return;
			}

			if (Arguments.size() > 1 + bWipeSingularQuickbar)
			{
				std::string bypassCanBeDropped = Arguments[1 + bWipeSingularQuickbar];
				std::transform(bypassCanBeDropped.begin(), bypassCanBeDropped.end(), bypassCanBeDropped.begin(), ::tolower);

				if (bypassCanBeDropped == "true") bCheckShouldBeDropped = true;
				else if (bypassCanBeDropped == "false") bCheckShouldBeDropped = false;
			}

			auto WorldInventory = ReceivingController->GetWorldInventory();

			if (!WorldInventory)
			{
				SendMessageToConsole(PlayerController, L"Player does not have a WorldInventory!\n");
				return;
			}

			static auto FortEditToolItemDefinitionClass = FindObject<UClass>(L"/Script/FortniteGame.FortEditToolItemDefinition");
			static auto FortBuildingItemDefinitionClass = FindObject<UClass>(L"/Script/FortniteGame.FortBuildingItemDefinition");

			std::vector<std::pair<FGuid, int>> GuidsAndCountsToRemove;
			const auto& ItemInstances = WorldInventory->GetItemList().GetItemInstances();
			auto PickaxeInstance = WorldInventory->GetPickaxeInstance();

			for (int i = 0; i < ItemInstances.Num(); ++i)
			{
				auto ItemInstance = ItemInstances.at(i);
				const auto ItemDefinition = Cast<UFortWorldItemDefinition>(ItemInstance->GetItemEntry()->GetItemDefinition());

				if (bCheckShouldBeDropped
					? ItemDefinition->CanBeDropped()
					: !ItemDefinition->IsA(FortBuildingItemDefinitionClass)
					&& !ItemDefinition->IsA(FortEditToolItemDefinitionClass)
					&& ItemInstance != PickaxeInstance
					)
				{
					bool IsPrimary = IsPrimaryQuickbar(ItemDefinition);

					if ((bWipePrimary && IsPrimary) || (bWipeSecondary && !IsPrimary))
					{
						GuidsAndCountsToRemove.push_back({ ItemInstance->GetItemEntry()->GetItemGuid(), ItemInstance->GetItemEntry()->GetCount() });
					}
				}
			}

			for (auto& [Guid, Count] : GuidsAndCountsToRemove)
			{
				WorldInventory->RemoveItem(Guid, nullptr, Count, true);
			}

			WorldInventory->Update();

			SendMessageToConsole(PlayerController, L"Cleared!\n");
		}
		else if (Command == "destroytarget")
		{
			UCheatManager*& CheatManager = ReceivingController->SpawnCheatManager(UCheatManager::StaticClass());

			if (!CheatManager)
			{
				SendMessageToConsole(PlayerController, L"Failed to spawn player's cheat manager!");
				return;
			}

			CheatManager->DestroyTarget();
			CheatManager = nullptr;
			SendMessageToConsole(PlayerController, L"Destroyed target!");
		}
		else if (Command == "slomo")
		{
			// Radarnite (fnmods.dll's slow motion): the match world's time dilation, as UCheatManager::Slomo sets
			// WorldSettings->TimeDilation; UGameplayStatics::SetGlobalTimeDilation clamps it to the world settings' range.
			// TimeDilation is replicated: every client follows the server.
			float Dilation = 1.f;

			if (NumArgs >= 1)
			{
				try { Dilation = std::stof(Arguments[1]); }
				catch (...) {}
			}

			if (!(Dilation > 0.f))
				Dilation = 1.f;

			static auto SetGlobalTimeDilationFn = FindObject<UFunction>(L"/Script/Engine.GameplayStatics.SetGlobalTimeDilation");

			if (!SetGlobalTimeDilationFn)
			{
				SendMessageToConsole(PlayerController, L"SetGlobalTimeDilation not found!");
				return;
			}

			struct { UObject* WorldContextObject; float TimeDilation; } SetGlobalTimeDilationParams{ GetWorld(), Dilation };
			static auto GameplayStaticsClass = UGameplayStatics::StaticClass();
			GameplayStaticsClass->ProcessEvent(SetGlobalTimeDilationFn, &SetGlobalTimeDilationParams);

			LOG_INFO(LogDev, "slomo {}", Dilation);
			SendMessageToConsole(PlayerController, std::wstring(L"Time dilation " + std::to_wstring(Dilation)).c_str());
		}
		else if (Command == "fly" || Command == "ghost" || Command == "walk")
		{
			// Radarnite (fnmods.dll's fly): UCheatManager::Fly / Ghost / Walk (UE 4.26). The owning client runs
			// ACharacter::ClientCheatFly / ClientCheatGhost / ClientCheatWalk (an RPC: its own movement prediction), and the
			// server's copy of the pawn gets the same: bCheatFlying, flying movement (falling when walking again) and, for
			// ghost, no collision.
			auto Pawn = Cast<APawn>(ReceivingController->GetPawn());

			if (!Pawn)
			{
				SendMessageToConsole(PlayerController, L"No pawn!");
				return;
			}

			bool bFly = Command != "walk", bGhost = Command == "ghost";

			static auto ClientCheatFlyFn = FindObject<UFunction>(L"/Script/Engine.Character.ClientCheatFly");
			static auto ClientCheatGhostFn = FindObject<UFunction>(L"/Script/Engine.Character.ClientCheatGhost");
			static auto ClientCheatWalkFn = FindObject<UFunction>(L"/Script/Engine.Character.ClientCheatWalk");
			auto ClientFn = bGhost ? ClientCheatGhostFn : bFly ? ClientCheatFlyFn : ClientCheatWalkFn;

			if (ClientFn)
				Pawn->ProcessEvent(ClientFn, nullptr);

			static auto SetActorEnableCollisionFn = FindObject<UFunction>(L"/Script/Engine.Actor.SetActorEnableCollision");

			if (SetActorEnableCollisionFn)
			{
				struct { bool bNewActorEnableCollision; } SetActorEnableCollisionParams{ !bGhost };
				Pawn->ProcessEvent(SetActorEnableCollisionFn, &SetActorEnableCollisionParams);
			}

			static auto CharacterMovementOffset = Pawn->GetOffset("CharacterMovement", false);
			auto CMC = CharacterMovementOffset != -1 ? Pawn->Get<UObject*>(CharacterMovementOffset) : nullptr;

			if (CMC)
			{
				static auto bCheatFlyingOffset = CMC->GetOffset("bCheatFlying", false);

				if (bCheatFlyingOffset != -1)
				{
					static auto bCheatFlyingFieldMask = GetFieldMask(CMC->GetProperty("bCheatFlying"));
					CMC->SetBitfieldValue(bCheatFlyingOffset, bCheatFlyingFieldMask, bFly);
				}

				static auto SetMovementModeFn = FindObject<UFunction>(L"/Script/Engine.CharacterMovementComponent.SetMovementMode");

				if (SetMovementModeFn)
				{
					struct { uint8 NewMovementMode; uint8 NewCustomMode; } SetMovementModeParams{ (uint8)(bFly ? 5 : 3), 0 };   // EMovementMode: MOVE_Flying 5, MOVE_Falling 3
					CMC->ProcessEvent(SetMovementModeFn, &SetMovementModeParams);
				}
			}

			LOG_INFO(LogDev, "{} for {} (client RPC {}, movement component {})", Command, (void*)Pawn, (void*)ClientFn, (void*)CMC);
			SendMessageToConsole(PlayerController, bGhost ? L"Ghost: flying through walls" : bFly ? L"Flying" : L"Walking");
		}
		else if (Command == "tpto")
		{
			// Radarnite (fnmods.dll's teleport to the crosshair): the client traced its camera's ray, X Y Z is the hit point and
			// NX NY NZ its normal. The feet go to the point, moved away from a wall by the capsule's radius times the normal's
			// horizontal part (a wall's full radius, a floor's nothing); under a ceiling (a normal pointing down) the head
			// goes to the point instead. AActor::TeleportTo moves the pawn out of anything it would still overlap.
			if (NumArgs < 3)
			{
				SendMessageToConsole(PlayerController, L"Please provide X, Y and Z!");
				return;
			}

			float V[6] = { 0, 0, 0, 0, 0, 1 };

			for (size_t i = 0; i < 6 && i < NumArgs; i++)
			{
				try { V[i] = std::stof(Arguments[i + 1]); }
				catch (...) {}
			}

			auto Pawn = Cast<APawn>(ReceivingController->GetPawn());

			if (!Pawn)
			{
				SendMessageToConsole(PlayerController, L"No pawn to teleport!");
				return;
			}

			float Radius = RnCapsule(Pawn, true);
			FVector Feet(V[0] + V[3] * Radius, V[1] + V[4] * Radius, V[5] < 0 ? V[2] - 2 * RnCapsule(Pawn, false) : V[2]);

			RnTeleportFeet(PlayerController, Pawn, Feet, "crosshair");
		}
		else if (Command == "tpground")
		{
			// Radarnite (fnmods.dll's teleport to the map pin): the feet on the ground at X Y (the highest surface there);
			// Z is used when the trace finds nothing
			if (NumArgs < 2)
			{
				SendMessageToConsole(PlayerController, L"Please provide X and Y!");
				return;
			}

			float X{}, Y{}, Z{};
			try { X = std::stof(Arguments[1]); } catch (...) {}
			try { Y = std::stof(Arguments[2]); } catch (...) {}
			if (NumArgs >= 3) { try { Z = std::stof(Arguments[3]); } catch (...) {} }

			auto Pawn = Cast<APawn>(ReceivingController->GetPawn());

			if (!Pawn)
			{
				SendMessageToConsole(PlayerController, L"No pawn to teleport!");
				return;
			}

			FVector Ground(X, Y, Z);
			bool bHit = RnGround(X, Y, Z, Pawn, &Ground);
			LOG_INFO(LogDev, "[radarnite] tpground {:.0f} {:.0f} {:.0f}: ground {} at {:.0f}", X, Y, Z, bHit ? "found" : "not found", Ground.Z);
			RnTeleportFeet(PlayerController, Pawn, bHit ? Ground : FVector(X, Y, Z), "map pin");
		}
		else if (Command == "tppoi")
		{
			// Radarnite (fnmods.dll's random POI): one of the map's POI volumes (FortPoiVolume), the feet on the ground at its
			// centre
			auto Pawn = Cast<APawn>(ReceivingController->GetPawn());

			if (!Pawn)
			{
				SendMessageToConsole(PlayerController, L"No pawn to teleport!");
				return;
			}

			static auto PoiClass = FindObject<UClass>(L"/Script/FortniteGame.FortPoiVolume");
			std::vector<AActor*> Pois;

			if (PoiClass)
			{
				auto Actors = UGameplayStatics::GetAllActorsOfClass(GetWorld(), PoiClass);

				for (int i = 0; i < Actors.Num(); i++)
					if (Actors.at(i))
						Pois.push_back(Actors.at(i));

				Actors.Free();
			}

			LOG_INFO(LogDev, "[radarnite] tppoi: FortPoiVolume {}, {} in the world", (void*)PoiClass, Pois.size());

			if (Pois.empty())
			{
				SendMessageToConsole(PlayerController, L"No POIs on this map!");
				return;
			}

			// a volume's actor sits at the origin and has no bounds of its own: the place is its PoiCollisionComp's location.
			// The named locations (FortPoiVolume_Named_*) first, the landmarks (_UnNamed_) when there are none.
			static auto CompLocFn = FindObject<UFunction>(L"/Script/Engine.SceneComponent.K2_GetComponentLocation");
			auto PlaceOf = [&](AActor* A) {
				static auto CompOffset = A->GetOffset("PoiCollisionComp", false);
				auto Comp = CompOffset != -1 ? A->Get<UObject*>(CompOffset) : nullptr;
				struct { FVector ReturnValue; } P{};
				if (Comp && CompLocFn)
					Comp->ProcessEvent(CompLocFn, &P);
				return P.ReturnValue;
			};
			std::vector<std::pair<AActor*, FVector>> Named, Any;

			for (auto A : Pois)
			{
				auto At = PlaceOf(A);
				if (At.X == 0 && At.Y == 0 && At.Z == 0)
					continue;
				Any.push_back({ A, At });
				if (A->GetName().find("_Named_") != std::string::npos)
					Named.push_back({ A, At });
			}

			// the volumes of 14.30 have no place (their component sits at the origin, their bounds are empty): the POIs'
			// building foundations instead (BuildingFoundation actors whose name has "POI"), each where its POI stands
			if (Any.empty())
			{
				static auto FoundationClass = FindObject<UClass>(L"/Script/FortniteGame.BuildingFoundation");
				auto Actors = FoundationClass ? UGameplayStatics::GetAllActorsOfClass(GetWorld(), FoundationClass) : TArray<AActor*>();
				std::string Some;
				int Total = Actors.Num();

				for (int i = 0; i < Actors.Num(); i++)
				{
					auto A = Actors.at(i);
					if (!A)
						continue;
					auto Name = A->GetName(), Upper = Name;
					std::transform(Upper.begin(), Upper.end(), Upper.begin(), ::toupper);
					if (Upper.find("POI") == std::string::npos)
						continue;
					auto At = A->GetActorLocation();
					Any.push_back({ A, At });
					if (Any.size() <= 12)
						Some += " " + Name;
				}

				Actors.Free();
				LOG_INFO(LogDev, "[radarnite] tppoi: {} building foundations, {} of POIs:{}", Total, Any.size(), Some);
			}

			auto& From = Named.empty() ? Any : Named;
			LOG_INFO(LogDev, "[radarnite] tppoi: {} named locations, {} with a place", Named.size(), Any.size());

			if (From.empty())
			{
				SendMessageToConsole(PlayerController, L"No POIs on this map!");
				return;
			}

			auto [Poi, L] = From[std::rand() % From.size()];
			FVector Ground(L.X, L.Y, L.Z);
			bool bHit = RnGround(L.X, L.Y, L.Z, Pawn, &Ground);
			LOG_INFO(LogDev, "[radarnite] tppoi: {} at {:.0f} {:.0f} {:.0f}, ground {}", Poi->GetFullName(), L.X, L.Y, L.Z, bHit ? "found" : "not found");
			RnTeleportFeet(PlayerController, Pawn, Ground, "POI");
		}
		else if (Command == "spawnbots")
		{
			// Radarnite (fnmods.dll's bot spawn): <Count> bots of the match's bot code on a ring around the player, each on
			// the ground; "team" puts them in the player's team, "enemy" (default) in the teams PickTeam gives
			auto Pawn = Cast<APawn>(ReceivingController->GetPawn());

			if (!Pawn)
			{
				SendMessageToConsole(PlayerController, L"No pawn to spawn bots at!");
				return;
			}

			if (!PlayerBot::IsReadyToSpawnBot())
			{
				SendMessageToConsole(PlayerController, L"Bots can't be spawned yet!");
				return;
			}

			int Count = 1;

			if (NumArgs >= 1)
			{
				try { Count = std::stoi(Arguments[1]); }
				catch (...) {}
			}

			Count = (std::max)(1, (std::min)(Count, 20));
			bool bTeam = NumArgs >= 2 && Arguments[2] == "team";
			int Team = bTeam ? (int)ReceivingPlayerState->GetTeamIndex() : -1;
			auto Center = Pawn->GetActorLocation();
			float Half = RnCapsule(Pawn, false);
			float Radius = (std::max)(500.f, Count * 150.f / 6.2831853f);   // 5 m, or 1.5 m between bots
			float Phase = (float)(std::rand() % 360) * 0.0174532925f;
			int AmountSpawned = 0;

			for (int i = 0; i < Count; i++)
			{
				float A = Phase + 6.2831853f * i / Count;
				FVector At(Center.X + std::cos(A) * Radius, Center.Y + std::sin(A) * Radius, Center.Z);
				FVector Ground;

				if (RnTrace(FVector(At.X, At.Y, Center.Z + 500), FVector(At.X, At.Y, Center.Z - 5000), Pawn, &Ground))
					At.Z = Ground.Z + Half;

				FRotator Facing{};
				Facing.Yaw = (float)(std::atan2(Center.Y - At.Y, Center.X - At.X) * 180.0 / 3.14159265358979);
				FTransform Transform;
				Transform.Translation = At;
				Transform.Rotation = Facing.Quaternion();
				Transform.Scale3D = FVector(1, 1, 1);

				auto NewBot = Bots::SpawnBot(Transform, Pawn, Team);
				auto BotPawn = NewBot ? NewBot->GetPawn() : nullptr;

				if (BotPawn)
				{
					BotPawn->SetCanBeDamaged(true);
					AmountSpawned++;
					auto BotState = Cast<AFortPlayerStateAthena>(NewBot->GetPlayerState());
					auto L = BotPawn->GetActorLocation();
					LOG_INFO(LogDev, "[radarnite] spawnbots: bot {} team {} at {:.0f} {:.0f} {:.0f}", AmountSpawned, BotState ? (int)BotState->GetTeamIndex() : -1, L.X, L.Y, L.Z);
				}
			}

			LOG_INFO(LogDev, "[radarnite] spawnbots: {} of {} spawned ({}, player team {})", AmountSpawned, Count, bTeam ? "team" : "enemy", (int)ReceivingPlayerState->GetTeamIndex());
			SendMessageToConsole(PlayerController, (std::wstring(L"Spawned ") + std::to_wstring(AmountSpawned) + L" bots").c_str());
		}
		else if (Command == "bugitgo")
		{
			if (Arguments.size() <= 3)
			{
				SendMessageToConsole(PlayerController, L"Please provide X, Y, and Z!\n");
				return;
			}

			float X{}, Y{}, Z{};

			try { X = std::stof(Arguments[1]); }
			catch (...) {}
			try { Y = std::stof(Arguments[2]); }
			catch (...) {}
			try { Z = std::stof(Arguments[3]); }
			catch (...) {}

			auto Pawn = Cast<APawn>(ReceivingController->GetPawn());

			if (!Pawn)
			{
				SendMessageToConsole(PlayerController, L"No pawn to teleport!");
				return;
			}

			Pawn->TeleportTo(FVector(X, Y, Z), Pawn->GetActorRotation());
			SendMessageToConsole(PlayerController, L"Teleported!");
		}
		else { bSendHelpMessage = true; };
	}
	else { bSendHelpMessage = true; };

	if (bSendHelpMessage)
	{
		FString HelpMessage = LR"(
cheat giveitem <ShortWID> <Count=1> - Gives a weapon to the executing player, if inventory is full drops a pickup on the player.
cheat summon <BlueprintClassPathName> <Count=1> - Summons the specified blueprint class at the executing player's location. Note: There is a limit on the count.
cheat bugitgo <X> <Y> <Z> - Teleport to a location.
cheat tpto <X> <Y> <Z> <NX=0> <NY=0> <NZ=1> - Feet on a point (and its surface normal).
cheat tpground <X> <Y> <Z> - Feet on the ground at X Y.
cheat tppoi - Teleport to a random POI.
cheat spawnbots <Count=1> <enemy|team> - Spawns bots around the player.
cheat god <on|off> - God mode (no argument toggles).
cheat slomo <Dilation=1> - Slow motion (time dilation of the match).
cheat fly / ghost / walk - Fly, fly through walls, walk again.
cheat launch <X> <Y> <Z> - Launches a player.
cheat listplayers - Gives you all players names.
cheat pausesafezone - Pauses the zone.
cheat sethealth <Health=100.f> - Sets executing player's health.
cheat setshield <Shield=0.f> - Sets executing player's shield.
cheat applycid <CIDShortName> - Sets a player's character.
cheat spawnpickup <ShortWID> <ItemCount=1> <PickupCount=1> - Spawns a pickup at specified player.
cheat teleport/tp - Teleports to what the player is looking at.
cheat savewaypoint (phrase/number) - Gets the location of where you are standing and saves it as a waypoint.
cheat waypoint (saved phrase/number) - Teleports the player to the selected existing waypoint.
cheat spawnbot <Amount=1> - Spawns a bot at the player (experimental).
cheat setpickaxe <PickaxeID> - Set player's pickaxe. Can be either the PID or WID
cheat destroytarget - Destroys the actor that the player is looking at.
cheat wipequickbar <Primary|Secondary> <RemoveUndroppables=false> - Wipes the specified quickbar (parameters is not case sensitive).
cheat wipequickbars <RemoveUndroppables=false> - Wipes primary and secondary quickbar of targeted player (parameter is not case sensitive).
cheat suicide - Makes targeted player suicide.
cheat startaircraft - Starts the aircraft (may work).

If you want to execute a command on a certain player, surround their name (case sensitive) with \, and put the param with their name anywhere. Example: cheat sethealth \Milxnor\ 100
)";

		SendMessageToConsole(PlayerController, HelpMessage);
	}
}
