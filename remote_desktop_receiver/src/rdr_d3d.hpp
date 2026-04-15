#pragma once

using HWND = struct HWND__*;

void RdrUpdateLetterboxRect();
void RdrUpdateLetterboxRectFor(int vw, int vh);

void RdrEnsureD3D(HWND hwnd);
void RdrResizeD3D(HWND hwnd);
void RdrEnsureTextureIfNeeded(int w, int h);
void RdrRenderOneFrame(int vw, int vh);

void RdrApplyFullscreenAllMonitors(HWND hwnd);
void RdrApplyFullscreenPrimary(HWND hwnd);
void RdrResizeWindowToVideoResolution(HWND hwnd, int videoW, int videoH);
