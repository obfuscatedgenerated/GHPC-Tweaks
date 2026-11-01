# 4GB patch (GHWT)

Sets the Large Address Aware flag on `GHWT.exe`, so the game can use up to 4 GB of memory instead of 2 GB. [WT:VR](../wt-vr/) needs it, and it helps any heavily loaded setup.

## Use

1. Put `4gb-wt.ps1` in the folder with `GHWT.exe`.
2. Right-click it and choose **Run with PowerShell**.

Or, from a terminal in that folder:

```
powershell -ExecutionPolicy Bypass -File .\4gb-wt.ps1
```

The bypass is needed because Windows blocks unsigned scripts by default; it only applies to this one run.

Running it again is harmless: it just says the game is already patched.

## Undo

It saves `GHWT.exe.backup` before patching. Rename it back to `GHWT.exe` to undo.