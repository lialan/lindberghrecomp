/* game_states.h - the state functions Sega's Lindbergh games run once a frame
 * in game mode (_Z<n>Game<Name>P8GAMEWORK): menus' names first, then play's.
 * A menu state running means the frame is a menu, whose 3D belongs on the
 * screen with its 2D. Shared by the GL host (stereo.c) and the Vulkan host. */
#pragma once
static const char *const lg_menu_states[] = {
    "Init", "Calibration", "Instruction", "Card", "Custom", "MatchUpSelect", "CharacterSelect", "WeaponSelect",
    "Information", "Entry", "StartWait", "StageSelect", "WaitCard", "NameEntry", "Ranking", "Record",
    "Password", "Result", "Start",
};
static const char *const lg_play_states[] = {
    "Opening", "RoundStart", "Loop", "RoundResult", "Replay", "SceneClear", "ChallengerComes",
    "TimeUp", "Continue", "Close", "Ending", "CleanUp", "Pause", "Exit", "Over", "StageClear",
};
