#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <mmsystem.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <new>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include "resolver.h"

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace {

// ── Process ──

HANDLE gProcess{};
uintptr_t gClient{};
SoundOffsets gOff{};

template <class T>
T RPM(uintptr_t addr) {
    T val{};
    ReadProcessMemory(gProcess, reinterpret_cast<void*>(addr), &val, sizeof(T), nullptr);
    return val;
}

DWORD FindProcess(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe))
        do { if (_wcsicmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; } }
        while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    return pid;
}

// ── Audio ──

constexpr int kMaxEvents = 64;
constexpr int kMaxSoundsPerEvent = 64;
constexpr DWORD kMaxDecode = 16 * 1024 * 1024;

struct DecodedClip {
    BYTE* pcm = nullptr;
    DWORD pcmLen = 0;
};

struct EventSounds {
    char name[64]{};
    DecodedClip clips[kMaxSoundsPerEvent];
    int count = 0;
    int order[kMaxSoundsPerEvent]{};
    int pos = 0;
    int vkey = 0;
    bool keyDown = false;

    void Shuffle() {
        for (int i = 0; i < count; ++i) order[i] = i;
        for (int i = count - 1; i > 0; --i) {
            int j = rand() % (i + 1);
            int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
        }
        pos = 0;
    }

    int Next() {
        if (count == 0) return -1;
        if (pos >= count) Shuffle();
        return order[pos++];
    }
};

EventSounds gEvents[kMaxEvents];
int gEventCount = 0;
HWAVEOUT gWaveOut = nullptr;
WAVEHDR gHdr{};

bool DecodeToPCM(const wchar_t* path, BYTE*& outBuf, DWORD& outLen) {
    outBuf = nullptr; outLen = 0;
    IMFSourceReader* reader = nullptr;
    if (FAILED(MFCreateSourceReaderFromURL(path, nullptr, &reader)) || !reader) return false;

    IMFMediaType* t = nullptr;
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
    t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 44100 * 4);
    reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, t);
    t->Release();

    BYTE* buf = new(std::nothrow) BYTE[kMaxDecode];
    if (!buf) { reader->Release(); return false; }
    DWORD len = 0;
    for (;;) {
        DWORD flags = 0; IMFSample* s = nullptr;
        if (FAILED(reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &s))
            || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) { if (s) s->Release(); break; }
        if (s) {
            IMFMediaBuffer* mb = nullptr; s->ConvertToContiguousBuffer(&mb);
            if (mb) {
                BYTE* d = nullptr; DWORD n = 0; mb->Lock(&d, nullptr, &n);
                if (len + n <= kMaxDecode) { std::memcpy(buf + len, d, n); len += n; }
                mb->Unlock(); mb->Release();
            }
            s->Release();
        }
    }
    reader->Release();
    if (!len) { delete[] buf; return false; }
    outBuf = new(std::nothrow) BYTE[len];
    if (outBuf) { std::memcpy(outBuf, buf, len); outLen = len; }
    delete[] buf;
    return outLen > 0;
}

bool HasEvent(const char* event) {
    for (int i = 0; i < gEventCount; ++i)
        if (std::strcmp(gEvents[i].name, event) == 0) return true;
    return false;
}

bool PlayEvent(const char* event, bool force = false) {
    if (!gWaveOut) return false;
    if (!force && gHdr.lpData && !(gHdr.dwFlags & WHDR_DONE)) return false;
    if (force) waveOutReset(gWaveOut);
    for (int i = 0; i < gEventCount; ++i) {
        if (std::strcmp(gEvents[i].name, event) != 0) continue;
        int idx = gEvents[i].Next();
        if (idx < 0 || !gEvents[i].clips[idx].pcm) return false;
        if (gHdr.dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(gWaveOut, &gHdr, sizeof(gHdr));
        std::memset(&gHdr, 0, sizeof(gHdr));
        gHdr.lpData = reinterpret_cast<LPSTR>(gEvents[i].clips[idx].pcm);
        gHdr.dwBufferLength = gEvents[i].clips[idx].pcmLen;
        waveOutPrepareHeader(gWaveOut, &gHdr, sizeof(gHdr));
        waveOutWrite(gWaveOut, &gHdr, sizeof(gHdr));
        std::printf("[sounds] %s #%d\n", event, idx + 1);
        return true;
    }
    return false;
}

UINT FindDevice() {
    UINT n = waveOutGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        WAVEOUTCAPSW caps{};
        if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
            if (wcsstr(caps.szPname, L"CABLE Input")) {
                wprintf(L"[sounds] output: %s\n", caps.szPname);
                return i;
            }
    }
    std::printf("[sounds] CABLE Input not found, using default\n");
    return WAVE_MAPPER;
}

bool LoadEventFromFolder(const wchar_t* exeDir, const wchar_t* relFolder, const char* eventName) {
    if (gEventCount >= kMaxEvents) return false;

    EventSounds& ev = gEvents[gEventCount];
    lstrcpynA(ev.name, eventName, sizeof(ev.name));

    // Single letter folder = key-triggered event
    if (ev.name[0] && !ev.name[1]) {
        char c = ev.name[0];
        if (c >= 'a' && c <= 'z') ev.vkey = c - 'a' + 'A';
        else if (c >= 'A' && c <= 'Z') ev.vkey = c;
        else if (c >= '0' && c <= '9') ev.vkey = c;
    }

    wchar_t pattern[MAX_PATH]{};
    lstrcpynW(pattern, exeDir, MAX_PATH);
    lstrcatW(pattern, L"sounds\\");
    lstrcatW(pattern, relFolder);
    lstrcatW(pattern, L"\\*");

    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return false;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (ev.count >= kMaxSoundsPerEvent) break;

        wchar_t filePath[MAX_PATH]{};
        lstrcpynW(filePath, exeDir, MAX_PATH);
        lstrcatW(filePath, L"sounds\\");
        lstrcatW(filePath, relFolder);
        lstrcatW(filePath, L"\\");
        lstrcatW(filePath, fd.cFileName);

        if (DecodeToPCM(filePath, ev.clips[ev.count].pcm, ev.clips[ev.count].pcmLen)) {
            char nameA[MAX_PATH]{};
            WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, nameA, sizeof(nameA), nullptr, nullptr);
            std::printf("[sounds]   %s/%s (%lu bytes)\n", ev.name, nameA, ev.clips[ev.count].pcmLen);
            ++ev.count;
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    if (ev.count > 0) {
        ev.Shuffle();
        std::printf("[sounds] %s: %d sounds\n", ev.name, ev.count);
        ++gEventCount;
        return true;
    }
    // No sounds: wipe the slot so a stale name/vkey can't leak
    ev = EventSounds{};
    return false;
}

bool LoadSoundsFolder() {
    wchar_t exeDir[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    if (wchar_t* s = wcsrchr(exeDir, L'\\')) *(s + 1) = L'\0';

    wchar_t soundsDir[MAX_PATH]{};
    lstrcpynW(soundsDir, exeDir, MAX_PATH);
    lstrcatW(soundsDir, L"sounds\\*");

    // Scan for event subfolders
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(soundsDir, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        std::printf("[sounds] sounds/ folder not found\n");
        return false;
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;

        char eventName[64]{};
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, eventName, sizeof(eventName), nullptr, nullptr);

        // Load the parent event (audio files directly inside the folder)
        LoadEventFromFolder(exeDir, fd.cFileName, eventName);

        // Scan for numeric sub-folders (e.g. onKill/1, onKill/2, ...)
        wchar_t subPattern[MAX_PATH]{};
        lstrcpynW(subPattern, exeDir, MAX_PATH);
        lstrcatW(subPattern, L"sounds\\");
        lstrcatW(subPattern, fd.cFileName);
        lstrcatW(subPattern, L"\\*");

        WIN32_FIND_DATAW fd2{};
        HANDLE hFind2 = FindFirstFileW(subPattern, &fd2);
        if (hFind2 == INVALID_HANDLE_VALUE) continue;

        do {
            if (!(fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fd2.cFileName[0] == L'.') continue;

            bool numeric = fd2.cFileName[0] != L'\0';
            for (int i = 0; fd2.cFileName[i]; ++i) {
                if (fd2.cFileName[i] < L'0' || fd2.cFileName[i] > L'9') { numeric = false; break; }
            }
            if (!numeric) continue;

            wchar_t relPath[MAX_PATH]{};
            lstrcpynW(relPath, fd.cFileName, MAX_PATH);
            lstrcatW(relPath, L"\\");
            lstrcatW(relPath, fd2.cFileName);

            char numStr[32]{};
            WideCharToMultiByte(CP_UTF8, 0, fd2.cFileName, -1, numStr, sizeof(numStr), nullptr, nullptr);
            char subEventName[64]{};
            _snprintf_s(subEventName, _TRUNCATE, "%s/%s", eventName, numStr);

            LoadEventFromFolder(exeDir, relPath, subEventName);
        } while (FindNextFileW(hFind2, &fd2));
        FindClose(hFind2);
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    MFShutdown();
    CoUninitialize();

    std::printf("[sounds] %d events loaded\n", gEventCount);
    return gEventCount > 0;
}

} // namespace

int wmain() {
    std::srand((unsigned)std::time(nullptr));
    std::printf("[sounds] starting...\n");

    // Load + decode sounds
    if (!LoadSoundsFolder()) return 1;

    // Open audio device
    UINT devId = FindDevice();
    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = 44100;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 4;
    wfx.nAvgBytesPerSec = 44100 * 4;
    if (waveOutOpen(&gWaveOut, devId, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        std::printf("[sounds] failed to open audio device\n");
        return 1;
    }

    // Attach to cs2
    DWORD pid = FindProcess(L"cs2.exe");
    if (!pid) {
        std::printf("[sounds] waiting for cs2.exe...\n");
        while (!(pid = FindProcess(L"cs2.exe"))) Sleep(1000);
    }
    gProcess = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!gProcess) { std::printf("[sounds] can't open cs2 -- run as admin\n"); return 1; }
    std::printf("[sounds] cs2 pid %lu\n", pid);

    // Resolve offsets
    std::printf("[sounds] resolving offsets...\n");
    std::string err;
    for (int attempt = 0; attempt < 60; ++attempt) {
        if (resolver::Resolve(gProcess, gOff, err)) break;
        if (attempt < 59) { std::printf("[sounds] %s, retrying...\n", err.c_str()); Sleep(2000); }
    }
    if (!gOff.dwLocalPlayerController) {
        std::printf("[sounds] resolve failed: %s\n", err.c_str());
        return 1;
    }
    gClient = 0;
    { uintptr_t base = 0; uint32_t sz = 0;
      resolver::FindModule(resolver::Process(gProcess), L"client.dll", base, sz);
      gClient = base; }
    std::printf("[sounds] client.dll @ %llX\n", gClient);

    int prevKillCount = -1, prevTotalRounds = -1;
    int roundKills = 0;
    bool prevAlive = false, prevFreeze = false;

    std::printf("[sounds] listening...\n");

    while (true) {
        Sleep(50);

        // Key-triggered events (single-letter folders)
        for (int i = 0; i < gEventCount; ++i) {
            if (!gEvents[i].vkey) continue;
            bool down = (GetAsyncKeyState(gEvents[i].vkey) & 0x8000) != 0;
            if (down && !gEvents[i].keyDown)
                PlayEvent(gEvents[i].name, true);
            gEvents[i].keyDown = down;
        }

        Sleep(150);

        uintptr_t ctrl = RPM<uintptr_t>(gClient + gOff.dwLocalPlayerController);
        if (!ctrl) continue;

        uintptr_t rules = RPM<uintptr_t>(gClient + gOff.dwGameRules);

        bool alive = RPM<uint8_t>(ctrl + gOff.pawnIsAlive) != 0;
        int32_t kills = RPM<int32_t>(ctrl + gOff.killCount);

        // Death: alive → dead
        if (prevAlive && !alive) {
            PlayEvent("onDeath");
            roundKills = 0;
        }

        // Respawn/bot takeover: dead → alive, reset score baseline
        if (!prevAlive && alive)
            prevKillCount = kills;
        prevAlive = alive;

        // Kill detection -- only advance baseline if played
        if (alive && prevKillCount >= 0 && kills > prevKillCount) {
            roundKills += (kills - prevKillCount);
            char subEvent[80];
            _snprintf_s(subEvent, _TRUNCATE, "onKill/%d", roundKills);
            if (HasEvent(subEvent)) PlayEvent(subEvent, true);
            else PlayEvent("onKill");
        }
        prevKillCount = kills;

        if (!rules) continue;

        // Round events force-stop any playing kill/death sound
        bool freeze = RPM<uint8_t>(rules + gOff.freezePeriod) != 0;
        if (freeze && !prevFreeze) {
            PlayEvent("onRoundFreeze", true);
            prevKillCount = kills;
            roundKills = 0;
        }
        else if (prevFreeze && !freeze) {
            PlayEvent("onRoundStart", true);
            prevKillCount = kills;
            roundKills = 0;
        }
        prevFreeze = freeze;

        int rounds = RPM<int>(rules + gOff.totalRoundsPlayed);
        if (prevTotalRounds > 0 && rounds > prevTotalRounds) {
            int win = RPM<int>(rules + gOff.roundWinStatus);
            uint8_t team = RPM<uint8_t>(ctrl + gOff.teamNum);
            if (win == team) PlayEvent("onRoundWin", true);
            else if (win > 0 && win != team) PlayEvent("onRoundLose", true);
            prevKillCount = kills;
            roundKills = 0;
        }
        prevTotalRounds = rounds;
    }
}
