#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mmsystem.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <new>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")

namespace {

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
WAVEFORMATEX gFmt{};  // populated by QueryDeviceFormat; used for decode + playback
CRITICAL_SECTION gPlayLock;  // guards PlayEvent -- GSI thread + key-poll thread both call it

// Query the preferred render device's mix format via WASAPI so we can decode and
// play at the device's native sample rate (avoids the OS resampling our audio).
// Keeps PCM 16-bit for waveOut compatibility.
bool QueryDeviceFormat(const wchar_t* nameHint, WAVEFORMATEX& out) {
    bool ok = false;
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), (void**)&en)) && en) {
        IMMDevice* target = nullptr;
        IMMDeviceCollection* col = nullptr;
        if (nameHint && SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col)) && col) {
            UINT n = 0; col->GetCount(&n);
            for (UINT i = 0; i < n && !target; ++i) {
                IMMDevice* d = nullptr; col->Item(i, &d);
                if (!d) continue;
                IPropertyStore* ps = nullptr;
                if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)) && ps) {
                    PROPVARIANT pv; PropVariantInit(&pv);
                    if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv))
                        && pv.vt == VT_LPWSTR && wcsstr(pv.pwszVal, nameHint)) {
                        target = d; d = nullptr;
                    }
                    PropVariantClear(&pv);
                    ps->Release();
                }
                if (d) d->Release();
            }
            col->Release();
        }
        if (!target) en->GetDefaultAudioEndpoint(eRender, eConsole, &target);
        if (target) {
            IAudioClient* ac = nullptr;
            if (SUCCEEDED(target->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac)) && ac) {
                WAVEFORMATEX* mix = nullptr;
                if (SUCCEEDED(ac->GetMixFormat(&mix)) && mix) {
                    out.wFormatTag = WAVE_FORMAT_PCM;
                    out.nChannels = mix->nChannels;
                    out.nSamplesPerSec = mix->nSamplesPerSec;
                    out.wBitsPerSample = 16;
                    out.nBlockAlign = (WORD)(out.nChannels * out.wBitsPerSample / 8);
                    out.nAvgBytesPerSec = out.nSamplesPerSec * out.nBlockAlign;
                    out.cbSize = 0;
                    CoTaskMemFree(mix);
                    ok = true;
                }
                ac->Release();
            }
            target->Release();
        }
        en->Release();
    }
    if (SUCCEEDED(coHr)) CoUninitialize();
    return ok;
}

bool DecodeToPCM(const wchar_t* path, BYTE*& outBuf, DWORD& outLen) {
    outBuf = nullptr; outLen = 0;
    IMFSourceReader* reader = nullptr;
    if (FAILED(MFCreateSourceReaderFromURL(path, nullptr, &reader)) || !reader) return false;

    IMFMediaType* t = nullptr;
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, gFmt.nChannels);
    t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, gFmt.nSamplesPerSec);
    t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, gFmt.wBitsPerSample);
    t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, gFmt.nBlockAlign);
    t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, gFmt.nAvgBytesPerSec);
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
    EnterCriticalSection(&gPlayLock);
    bool played = false;
    if (force || !gHdr.lpData || (gHdr.dwFlags & WHDR_DONE)) {
        if (force) waveOutReset(gWaveOut);
        for (int i = 0; i < gEventCount; ++i) {
            if (std::strcmp(gEvents[i].name, event) != 0) continue;
            int idx = gEvents[i].Next();
            if (idx < 0 || !gEvents[i].clips[idx].pcm) break;
            if (gHdr.dwFlags & WHDR_PREPARED)
                waveOutUnprepareHeader(gWaveOut, &gHdr, sizeof(gHdr));
            std::memset(&gHdr, 0, sizeof(gHdr));
            gHdr.lpData = reinterpret_cast<LPSTR>(gEvents[i].clips[idx].pcm);
            gHdr.dwBufferLength = gEvents[i].clips[idx].pcmLen;
            waveOutPrepareHeader(gWaveOut, &gHdr, sizeof(gHdr));
            waveOutWrite(gWaveOut, &gHdr, sizeof(gHdr));
            std::printf("[sounds] %s #%d\n", event, idx + 1);
            played = true;
            break;
        }
    }
    LeaveCriticalSection(&gPlayLock);
    return played;
}

UINT PromptDevice(wchar_t* nameOut, size_t nameOutCch) {
    if (nameOut && nameOutCch) nameOut[0] = L'\0';

    // Enumerate via WASAPI for full (untruncated) friendly names, then map each
    // back to a waveOut device ID via szPname prefix match -- waveOut's name is
    // the first ~31 chars of the WASAPI friendly name.
    struct Dev { wchar_t name[256]; UINT waveId; };
    Dev devs[32]{};
    int count = 0;

    HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), (void**)&en)) && en) {
        IMMDeviceCollection* col = nullptr;
        if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col)) && col) {
            UINT n = 0; col->GetCount(&n);
            UINT waveCount = waveOutGetNumDevs();
            for (UINT i = 0; i < n && count < (int)_countof(devs); ++i) {
                IMMDevice* d = nullptr; col->Item(i, &d);
                if (!d) continue;
                IPropertyStore* ps = nullptr;
                if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)) && ps) {
                    PROPVARIANT pv; PropVariantInit(&pv);
                    if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) {
                        lstrcpynW(devs[count].name, pv.pwszVal, (int)_countof(devs[0].name));
                        UINT matched = (UINT)-1;
                        for (UINT w = 0; w < waveCount; ++w) {
                            WAVEOUTCAPSW caps{};
                            if (waveOutGetDevCapsW(w, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
                            size_t L = wcslen(caps.szPname);
                            if (L && wcsncmp(caps.szPname, devs[count].name, L) == 0) { matched = w; break; }
                        }
                        if (matched != (UINT)-1) {
                            devs[count].waveId = matched;
                            ++count;
                        }
                    }
                    PropVariantClear(&pv);
                    ps->Release();
                }
                d->Release();
            }
            col->Release();
        }
        en->Release();
    }
    if (SUCCEEDED(coHr)) CoUninitialize();

    if (count == 0) {
        std::printf("[sounds] no audio output devices found, using system default\n");
        return WAVE_MAPPER;
    }

    std::printf("\n[sounds] audio output devices:\n");
    for (int i = 0; i < count; ++i)
        wprintf(L"  %d: %s\n", i, devs[i].name);
    wprintf(L"  %d: (system default)\n", count);
    std::printf("select device [0-%d]: ", count);
    std::fflush(stdout);

    unsigned pick = (unsigned)count;
    if (scanf_s("%u", &pick) != 1 || pick > (unsigned)count) pick = (unsigned)count;

    if ((int)pick == count) {
        std::printf("[sounds] using system default output\n");
        return WAVE_MAPPER;
    }
    if (nameOut && nameOutCch) lstrcpynW(nameOut, devs[pick].name, (int)nameOutCch);
    wprintf(L"[sounds] selected: %s\n", devs[pick].name);
    return devs[pick].waveId;
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

// ── Minimal JSON reader (object-scoped lookups; good enough for GSI payloads) ──

const char* JsonSkip(const char* p) {
    if (!p) return nullptr;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (*p == '"') {
        ++p;
        while (*p && *p != '"') { if (*p == '\\' && p[1]) ++p; ++p; }
        return *p == '"' ? p + 1 : nullptr;
    }
    if (*p == '{' || *p == '[') {
        char close = (*p == '{') ? '}' : ']';
        int d = 1; ++p;
        bool inStr = false;
        while (*p && d > 0) {
            if (inStr) {
                if (*p == '\\' && p[1]) p += 2;
                else { if (*p == '"') inStr = false; ++p; }
                continue;
            }
            if (*p == '"') inStr = true;
            else if (*p == '{' || *p == '[') ++d;
            else if (*p == '}' || *p == ']') --d;
            ++p;
        }
        return d == 0 ? p : nullptr;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') ++p;
    return p;
}

const char* JsonGet(const char* obj, const char* key) {
    if (!obj) return nullptr;
    while (*obj == ' ' || *obj == '\t' || *obj == '\n' || *obj == '\r') ++obj;
    if (*obj != '{') return nullptr;
    size_t klen = std::strlen(key);
    const char* p = obj + 1;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') ++p;
        if (*p == '}') return nullptr;
        if (*p != '"') return nullptr;
        const char* keyStart = p + 1;
        const char* keyEnd = std::strchr(keyStart, '"');
        if (!keyEnd) return nullptr;
        bool match = (size_t)(keyEnd - keyStart) == klen && std::memcmp(keyStart, key, klen) == 0;
        p = keyEnd + 1;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':') ++p;
        if (match) return p;
        p = JsonSkip(p);
        if (!p) return nullptr;
    }
    return nullptr;
}

int JsonInt(const char* v, int def) { return v ? std::atoi(v) : def; }

bool JsonStr(const char* v, char* out, size_t cap) {
    if (!v || *v != '"' || !out || cap == 0) { if (out && cap) out[0] = 0; return false; }
    ++v;
    size_t i = 0;
    while (*v && *v != '"' && i < cap - 1) {
        if (*v == '\\' && v[1]) { out[i++] = v[1]; v += 2; } else out[i++] = *v++;
    }
    out[i] = '\0';
    return true;
}

// ── GSI event processing ──

void ProcessGSI(const char* json) {
    static int prevKills = -1, prevHealth = -1, prevCtScore = -1, prevTScore = -1;
    static char prevPhase[32] = "";
    static int roundKills = 0;

    const char* player = JsonGet(json, "player");
    const char* state  = JsonGet(player, "state");
    const char* round  = JsonGet(json, "round");
    const char* map    = JsonGet(json, "map");
    const char* teamCt = JsonGet(map, "team_ct");
    const char* teamT  = JsonGet(map, "team_t");

    int health  = JsonInt(JsonGet(state,  "health"),      -1);
    int kills   = JsonInt(JsonGet(state,  "round_kills"), -1);
    int ctScore = JsonInt(JsonGet(teamCt, "score"),       -1);
    int tScore  = JsonInt(JsonGet(teamT,  "score"),       -1);

    char phase[32] = ""; JsonStr(JsonGet(round,  "phase"), phase, sizeof(phase));
    char team[8]   = ""; JsonStr(JsonGet(player, "team"),  team,  sizeof(team));

    // Death: health went from >0 to 0
    if (prevHealth > 0 && health == 0) {
        PlayEvent("onDeath");
        roundKills = 0;
    }

    // Kill: round_kills increased
    if (prevKills >= 0 && kills > prevKills) {
        roundKills += (kills - prevKills);
        char subEvent[80];
        _snprintf_s(subEvent, _TRUNCATE, "onKill/%d", roundKills);
        if (HasEvent(subEvent)) PlayEvent(subEvent, true);
        else PlayEvent("onKill");
    }

    // Round phase transitions
    if (prevPhase[0] && std::strcmp(prevPhase, phase) != 0) {
        if (std::strcmp(phase, "freezetime") == 0) {
            PlayEvent("onRoundFreeze", true);
            roundKills = 0;
        } else if (std::strcmp(phase, "live") == 0 && std::strcmp(prevPhase, "freezetime") == 0) {
            PlayEvent("onRoundStart", true);
            roundKills = 0;
        }
    }

    // Round win/lose: team score changed (compared to previous payload)
    if (prevCtScore >= 0 && prevTScore >= 0 && team[0]) {
        bool ctWon = ctScore > prevCtScore;
        bool tWon  = tScore  > prevTScore;
        if (ctWon || tWon) {
            bool meWon = (ctWon && team[0] == 'C') || (tWon && team[0] == 'T');
            PlayEvent(meWon ? "onRoundWin" : "onRoundLose", true);
            roundKills = 0;
        }
    }

    prevKills   = kills;
    prevHealth  = health;
    prevCtScore = ctScore;
    prevTScore  = tScore;
    lstrcpynA(prevPhase, phase, sizeof(prevPhase));
}

// ── GSI HTTP server (127.0.0.1:3000) ──

constexpr int kGsiPort = 3000;

DWORD WINAPI GsiServerThread(LPVOID) {
    WSADATA wsa{}; WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET) return 1;
    BOOL reuse = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kGsiPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (sockaddr*)&addr, sizeof(addr)) != 0
        || listen(listener, SOMAXCONN) != 0) {
        std::printf("[gsi] bind/listen on 127.0.0.1:%d failed\n", kGsiPort);
        closesocket(listener);
        return 1;
    }
    std::printf("[gsi] listening on http://127.0.0.1:%d\n", kGsiPort);

    static char buf[65536];
    for (;;) {
        SOCKET c = accept(listener, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;

        int total = 0, headersEnd = -1, contentLength = 0;
        while (total < (int)sizeof(buf) - 1) {
            int r = recv(c, buf + total, (int)sizeof(buf) - 1 - total, 0);
            if (r <= 0) break;
            total += r;
            buf[total] = '\0';
            if (headersEnd < 0) {
                char* he = std::strstr(buf, "\r\n\r\n");
                if (he) {
                    headersEnd = (int)(he - buf) + 4;
                    char* cl = std::strstr(buf, "Content-Length:");
                    if (!cl) cl = std::strstr(buf, "content-length:");
                    if (cl) contentLength = std::atoi(cl + 15);
                }
            }
            if (headersEnd >= 0 && total >= headersEnd + contentLength) break;
        }

        const char* resp = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send(c, resp, (int)std::strlen(resp), 0);
        closesocket(c);

        if (headersEnd >= 0 && contentLength > 0 && headersEnd + contentLength < (int)sizeof(buf)) {
            buf[headersEnd + contentLength] = '\0';
            ProcessGSI(buf + headersEnd);
        }
    }
    // unreachable
}

// ── GSI config auto-install into CS2 cfg folder ──

void EnsureGsiConfig() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &key) != ERROR_SUCCESS) {
        std::printf("[gsi] Steam not found in registry; install the GSI config manually (see README)\n");
        return;
    }
    wchar_t steamPath[MAX_PATH]{}; DWORD cb = sizeof(steamPath); DWORD type = 0;
    LONG r = RegQueryValueExW(key, L"SteamPath", nullptr, &type, (LPBYTE)steamPath, &cb);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS) {
        std::printf("[gsi] SteamPath value missing from registry\n");
        return;
    }
    for (wchar_t* p = steamPath; *p; ++p) if (*p == L'/') *p = L'\\';

    wchar_t cfgPath[MAX_PATH];
    _snwprintf_s(cfgPath, _TRUNCATE,
        L"%s\\steamapps\\common\\Counter-Strike Global Offensive\\game\\csgo\\cfg\\gamestate_integration_node.cfg",
        steamPath);

    HANDLE h = CreateFileW(cfgPath, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_EXISTS) wprintf(L"[gsi] config already present: %s\n", cfgPath);
        else wprintf(L"[gsi] could not write %s (err %lu) -- install it manually\n", cfgPath, e);
        return;
    }
    const char* cfg =
        "\"CS2 Sound Player\"\n"
        "{\n"
        "    \"uri\"       \"http://127.0.0.1:3000\"\n"
        "    \"timeout\"   \"5.0\"\n"
        "    \"buffer\"    \"0.1\"\n"
        "    \"throttle\"  \"0.1\"\n"
        "    \"heartbeat\" \"30.0\"\n"
        "    \"data\"\n"
        "    {\n"
        "        \"provider\"           \"1\"\n"
        "        \"map\"                \"1\"\n"
        "        \"round\"              \"1\"\n"
        "        \"player_id\"          \"1\"\n"
        "        \"player_state\"       \"1\"\n"
        "        \"player_match_stats\" \"1\"\n"
        "    }\n"
        "}\n";
    DWORD w = 0;
    WriteFile(h, cfg, (DWORD)std::strlen(cfg), &w, nullptr);
    CloseHandle(h);
    wprintf(L"[gsi] installed config: %s\n", cfgPath);
}

} // namespace

int wmain() {
    std::srand((unsigned)std::time(nullptr));
    std::printf("[sounds] starting...\n");

    // Prompt first so a mistake doesn't waste the decode pass.
    wchar_t devName[256]{};
    UINT devId = PromptDevice(devName, _countof(devName));

    // Match the chosen device's native sample rate so nothing gets resampled.
    if (!QueryDeviceFormat(devName[0] ? devName : nullptr, gFmt)) {
        std::printf("[sounds] device mix format query failed; falling back to 48kHz stereo 16-bit\n");
        gFmt.wFormatTag = WAVE_FORMAT_PCM;
        gFmt.nChannels = 2;
        gFmt.nSamplesPerSec = 48000;
        gFmt.wBitsPerSample = 16;
        gFmt.nBlockAlign = 4;
        gFmt.nAvgBytesPerSec = 48000 * 4;
    }
    std::printf("[sounds] format: %lu Hz, %u ch, %u-bit\n",
        gFmt.nSamplesPerSec, gFmt.nChannels, gFmt.wBitsPerSample);

    // Load + decode sounds (uses gFmt)
    if (!LoadSoundsFolder()) return 1;

    InitializeCriticalSection(&gPlayLock);

    // Open the chosen device at the same format
    if (waveOutOpen(&gWaveOut, devId, &gFmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        std::printf("[sounds] failed to open audio device\n");
        return 1;
    }

    // Install the Game State Integration config in CS2's cfg folder if missing.
    EnsureGsiConfig();

    // Spin up the HTTP server that receives CS2's state posts.
    HANDLE srv = CreateThread(nullptr, 0, GsiServerThread, nullptr, 0, nullptr);
    if (!srv) {
        std::printf("[sounds] failed to start GSI server thread\n");
        return 1;
    }

    std::printf("[sounds] listening...\n");

    // Key-triggered events (single-letter folders).
    while (true) {
        for (int i = 0; i < gEventCount; ++i) {
            if (!gEvents[i].vkey) continue;
            bool down = (GetAsyncKeyState(gEvents[i].vkey) & 0x8000) != 0;
            if (down && !gEvents[i].keyDown)
                PlayEvent(gEvents[i].name, true);
            gEvents[i].keyDown = down;
        }
        Sleep(30);
    }
}
