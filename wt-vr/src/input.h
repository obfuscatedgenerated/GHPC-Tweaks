// Hotkeys: every action the mod has a key for, bound in vr.ini [keys].
// Written as e.g. "save_spot=F8" or "hud_dump=Ctrl+F7" (modifiers: Ctrl, Shift, Alt;
// keys: A-Z, 0-9, F1-F24, Home, End, Insert, Delete, PageUp, PageDown, [, ], Space, Shift, Ctrl, Alt, Tab,
// Up, Down, Left, Right, Num0-Num9; "none" turns one off). A press counts only with exactly its modifiers;
// held keys (moving, turning) ignore them. Debug ones only work with debug_tools=1.
#pragma once

enum Action {
    ActionRecenter, ActionSaveSettings, ActionToggleHud, ActionToggleVrMenus, ActionMirrorHud,
    ActionWorldScaleDown, ActionWorldScaleUp, ActionHudCloser, ActionHudFurther, ActionHudWider, ActionHudNarrower,
    ActionHudHigher, ActionHudLower, ActionToggleDepthOfField, ActionSaveSpot, ActionForgetSpot,
    ActionPickBandMember, ActionBandMemberMode, ActionBandMemberUp, ActionBandMemberDown, ActionBandMemberForward,
    ActionBandMemberBack, ActionXrRetry,
    ActionMoveForward, ActionMoveBack, ActionMoveLeft, ActionMoveRight, ActionMoveUp, ActionMoveDown,
    ActionTurnLeft, ActionTurnRight, ActionMoveFast,
    // debug_tools
    ActionToggleFollowHead, ActionToggleWideCulling, ActionCullingFovDown, ActionCullingFovUp, ActionCameraScan,
    ActionCameraScanList, ActionHudDump, ActionRoleProbe, ActionFinderNext, ActionFinderPrevious, ActionTestPattern,
    ActionCount
};

// [keys] from vr.ini (writeMissing: add any action that isn't listed, with its default key).
void LoadKeys(bool writeMissing);
// Once a frame, before checking actions.
void UpdateKeyStates();
bool KeyDown(int virtualKey);
// Pressed this frame with exactly its modifiers.
bool ActionPressed(Action action);
// Held / pressed this frame, whatever modifiers are held (moving, turning).
bool ActionHeld(Action action);
bool ActionTapped(Action action);
// Keys only count while the game window is focused (so typing elsewhere doesn't move you).
bool GameHasFocus();
